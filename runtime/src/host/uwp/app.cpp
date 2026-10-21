// SPDX-License-Identifier: MIT
#if defined(BOTW_PLATFORM_UWP)
#include "../../host/window.h"
#include "../../latte/latte.h"
#include "../../log.h"
#include "../../mods/archive_install.h"
#include "../../mods/catalog.h"
#include "../../mods/gamebanana.h"
#include "../../mods/store.h"
#include "../../mods/zip_install.h"
#include "../../settings.h"
#include "art_png.h"
#include "art_view.h"
#include "audio.h"
#include "controller.h"
#include "first_frame.h"
#include "host.h"
#include "launcher.h"
#include "mod_network.h"
#include "qr.h"
#include "quick_view.h"
#include "runtime.h"
#include "setup_storage.h"
#include "strings.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <thread>
#include <tuple>
#include <vector>
#include <windows.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Popups.h>
#include <winrt/Windows.UI.Text.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Gaming::Input;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Popups;

namespace {
CoreWindow host_window{nullptr};
std::atomic<int> pixel_width{1280}, pixel_height{720}, layout{0};
std::atomic<bool> visible{false}, drc{false}, rumble_enabled{true}, quit{false}, menu_visible{false};
std::atomic<uwp::VisibilityCallback> visibility_callback{nullptr};
void active(bool value) {
    visible = value;
    input::set_gamepad({});
    uwp::pause_audio(!value || menu_visible.load());
    if (auto callback = visibility_callback.load())
        callback(value);
}
void update_size() {
    auto bounds = host_window.Bounds();
    auto dpi = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().LogicalDpi();
    pixel_width = std::max(1, static_cast<int>(bounds.Width * dpi / 96.f));
    pixel_height = std::max(1, static_cast<int>(bounds.Height * dpi / 96.f));
}
StorageFolder child(StorageFolder const &root, wchar_t const *path) {
    try {
        return root.GetFolderAsync(path).get();
    } catch (hresult_error const &) {
        return nullptr;
    }
}
uwp::Candidate inspect(StorageFolder const &root, uwp::Source source) {
    uwp::Candidate candidate{to_string(root.Path()), source};
    auto code = child(root, L"code");
    if (code) {
        try {
            candidate.rpx = code.GetFileAsync(L"U-King.rpx").get() != nullptr;
        } catch (hresult_error const &) {
        }
    }
    candidate.content = child(root, L"content") != nullptr;
    candidate.meta = child(root, L"meta") != nullptr;
    return candidate;
}

std::optional<uwp::ArtImage> read_art(StorageFolder const &meta, wchar_t const *name, bool png = false) {
    if (!meta)
        return std::nullopt;
    try {
        auto file = meta.GetFileAsync(name).get();
        auto stream = file.OpenReadAsync().get();
        auto size = stream.Size();
        if (!size || size > uwp::art_byte_limit)
            return std::nullopt;
        Streams::DataReader reader(stream.GetInputStreamAt(0));
        if (reader.LoadAsync(static_cast<uint32_t>(size)).get() != size)
            return std::nullopt;
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        reader.ReadBytes(bytes);
        return png ? uwp::decode_art_png(bytes) : uwp::decode_tga(bytes);
    } catch (hresult_error const &) {
        return std::nullopt;
    }
}

template <class Operation> auto art_wait(Operation operation) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (operation.Status() == AsyncStatus::Started) {
        if (std::chrono::steady_clock::now() >= deadline) {
            operation.Cancel();
            throw hresult_canceled();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return operation.get();
}
std::optional<uwp::ArtImage> download_disc(StorageFolder const &local, StorageFolder const &game) {
    try {
        auto meta = child(game, L"meta");
        auto identity = uwp::disc_identity(uwp::setup::read_text(meta, L"meta.xml"));
        if (!identity)
            return {};
        auto cache = local.CreateFolderAsync(L"art", CreationCollisionOption::OpenIfExists).get();
        if (uwp::setup::read_text(cache, L"disc-id.txt") == identity->id) {
            if (auto image = read_art(cache, L"disc.png", true))
                return image;
        }
        using namespace winrt::Windows::Web::Http;
        using namespace winrt::Windows::Storage::Streams;
        HttpClient client;
        for (auto kind : {"discM", "disc"}) {
            try {
                auto url = "https://art.gametdb.com/wiiu/" + std::string(kind) + "/" + identity->region +
                           "/" + identity->id + ".png";
                auto response = art_wait(
                    client.GetAsync(Uri(to_hstring(url)), HttpCompletionOption::ResponseHeadersRead));
                if (response.StatusCode() != HttpStatusCode::Ok ||
                    response.RequestMessage().RequestUri().SchemeName() != L"https")
                    continue;
                auto stream = art_wait(response.Content().ReadAsInputStreamAsync());
                std::vector<uint8_t> bytes;
                for (;;) {
                    auto buffer =
                        art_wait(stream.ReadAsync(Buffer(65536), 65536, InputStreamOptions::Partial));
                    if (!buffer.Length())
                        break;
                    if (bytes.size() + buffer.Length() > uwp::art_byte_limit)
                        throw hresult_invalid_argument();
                    std::vector<uint8_t> chunk(buffer.Length());
                    DataReader::FromBuffer(buffer).ReadBytes(chunk);
                    bytes.insert(bytes.end(), chunk.begin(), chunk.end());
                }
                auto image = uwp::decode_art_png(bytes);
                if (!image)
                    continue;
                auto file =
                    cache.CreateFileAsync(L"disc.png", CreationCollisionOption::ReplaceExisting).get();
                FileIO::WriteBytesAsync(file, bytes).get();
                uwp::setup::write_text(cache, L"disc-id.txt", identity->id);
                return image;
            } catch (...) {
            }
        }
    } catch (...) {
    }
    return {};
}

struct App : implements<App, IFrameworkViewSource, IFrameworkView> {
    bool pt = false, menu_open = false, running = false, boot_attempted = false;
    bool failed = false, foreground = true, suspended = false, opening = false;
    uwp::Preferences preferences;
    uwp::Controller controller;
    uwp::HoldChord quick_chord;
    uwp::setup::Repeat quick_repeat;
    bool quick_was_open = false;
    float render_scale_choice = 0;
    uwp::GameArt art;
    uwp::ArtView home;
    bool mods_open = false, browser_open = false, text_entry = false, transfer_active = false;
    bool setup_active = false, setup_busy = false, setup_guide = false;
    int setup_action = 0;
    bool accepted_forms = false, setup_detect = false;
    uwp::setup::Progress setup_progress;
    uwp::setup::Repeat setup_repeat;
    std::string setup_error, saved_usb_mode, saved_game_source;
    int browser_action = 0;
    std::wstring input_text;
    uwp::Transfer transfer;
    winrt::Windows::UI::Text::Core::CoreTextEditContext edit_context{nullptr};
    winrt::Windows::UI::Text::Core::CoreTextRange text_selection{};
    event_token pointer_moved{}, character_received{};
    int mods_action = 0;
    uwp::setup::Repeat store_repeat;
    uint64_t store_generation = 0;
    std::atomic<bool> store_fetching{false};
    bool store_request = false;
    std::shared_ptr<uwp::StoreRequests> store_requests = std::make_shared<uwp::StoreRequests>();
    std::filesystem::path store_root;
    std::vector<mods::store::Card> store_installed;
    std::vector<mods::store::Card> store_available;
    uint32_t home_buttons = ~uint32_t(0);
    std::chrono::steady_clock::time_point loading_since{};
    event_token key_down{}, pointer_pressed{};
    StorageFolder local{nullptr}, selected_game{nullptr}, screenshots{nullptr};
    std::vector<uwp::Candidate> candidates;
    Gamepad pad{nullptr};
    std::chrono::steady_clock::time_point next_rumble{};
    event_token activating{}, suspending{}, resuming{}, closed{}, visibility{}, activated{}, size{}, dpi{};

    IFrameworkView CreateView() { return *this; }
    void Initialize(CoreApplicationView const &view) {
        activating = view.Activated([](auto const &, auto const &) { host_window.Activate(); });
        suspending = CoreApplication::Suspending([this](auto const &, SuspendingEventArgs const &) {
            suspended = true;
            neutral();
            active(false); // synchronous, bounded: no deferral needed for LocalSettings already persisted.
        });
        resuming = CoreApplication::Resuming([this](auto const &, auto const &) {
            suspended = false;
            controller.reset();
            active(foreground);
        });
    }
    void SetWindow(CoreWindow const &window) {
        host_window = window;
        closed = window.Closed([](auto const &, auto const &) { quit = true; });
        visibility = window.VisibilityChanged([this](auto const &, VisibilityChangedEventArgs const &e) {
            foreground = e.Visible();
            neutral();
            active(foreground && !suspended);
        });
        activated = window.Activated([this](auto const &, WindowActivatedEventArgs const &e) {
            foreground = e.WindowActivationState() != CoreWindowActivationState::Deactivated;
            neutral();
            active(foreground && !suspended);
        });
        size = window.SizeChanged([this](auto const &, auto const &) {
            update_size();
            if (running) {
                int output_width = pixel_width.load(), output_height = pixel_height.load();
                uwp::refresh_output_mode(output_width, output_height);
                latte::resize_window(output_width, output_height);
            } else
                resize_home();
        });
        dpi = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().DpiChanged(
            [this](auto const &, auto const &) {
                update_size();
                if (running) {
                    int output_width = pixel_width.load(), output_height = pixel_height.load();
                    uwp::refresh_output_mode(output_width, output_height);
                    latte::resize_window(output_width, output_height);
                } else
                    resize_home();
            });
        key_down = window.KeyDown([this](auto const &, KeyEventArgs const &e) {
            if ((!home.shown && !transfer_active && !text_entry && !browser_open) || home.loading ||
                home.flip_time >= 0)
                return;
            using winrt::Windows::System::VirtualKey;
            auto key = e.VirtualKey();
            if (key == VirtualKey::Enter || key == VirtualKey::Escape || key == VirtualKey::Space)
                ui_feedback();
            if (setup_active) {
                e.Handled(true);
                // Real pads use the repeat/edge sampler below; virtual keys also support remote input.
                if (pad && (key == VirtualKey::GamepadA || key == VirtualKey::GamepadB ||
                            key == VirtualKey::GamepadX ||
                            key == VirtualKey::GamepadDPadUp || key == VirtualKey::GamepadDPadDown ||
                            key == VirtualKey::GamepadDPadLeft || key == VirtualKey::GamepadDPadRight))
                    return;
                if (key == VirtualKey::GamepadX && setup_detect && !setup_busy) {
                    accepted_forms = !accepted_forms;
                    setup_action = -5;
                }
                if (key == VirtualKey::GamepadA)
                    key = VirtualKey::Enter;
                if (key == VirtualKey::GamepadDPadUp)
                    key = VirtualKey::Up;
                if (key == VirtualKey::GamepadDPadDown)
                    key = VirtualKey::Down;
                if (key == VirtualKey::GamepadDPadLeft)
                    key = VirtualKey::Left;
                if (key == VirtualKey::GamepadDPadRight)
                    key = VirtualKey::Right;
                if (key == VirtualKey::Escape || key == VirtualKey::GamepadB) {
                    if (setup_busy)
                        setup_progress.cancel = true;
                    else
                        setup_action = -2;
                }
                if (setup_busy)
                    return;
                if (key == VirtualKey::Left) {
                    if (setup_guide)
                        setup_action = -3;
                    else
                        home.navigate(3);
                }
                if (key == VirtualKey::Right) {
                    if (setup_guide)
                        setup_action = -4;
                    else
                        home.navigate(1);
                }
                if (key == VirtualKey::Up)
                    home.navigate(0);
                if (key == VirtualKey::Down || key == VirtualKey::Tab)
                    home.navigate(2);
                if ((key == VirtualKey::Enter || key == VirtualKey::Space) &&
                    home.enabled(size_t(home.focus)))
                    home.selected = home.focus;
                return;
            }

            if (transfer_active) {
                if (key == VirtualKey::Escape)
                    transfer.cancel();
                return;
            }
            if (text_entry) {
                if (key == VirtualKey::Escape)
                    home.selected = 2;
                if (key == VirtualKey::Enter)
                    home.selected = 1;
                if (key == VirtualKey::Back && !edit_context && !input_text.empty()) {
                    auto last = input_text.back();
                    input_text.pop_back();
                    if (last >= 0xdc00 && last <= 0xdfff && !input_text.empty())
                        input_text.pop_back();
                    int32_t end = static_cast<int32_t>(input_text.size());
                    text_selection = {end, end};
                    if (edit_context)
                        edit_context.NotifyTextChanged(
                            {end, end + (last >= 0xdc00 && last <= 0xdfff ? 2 : 1)}, 0, text_selection);
                }
                return;
            }
            if (browser_open) {
                if (key == VirtualKey::Escape)
                    browser_action = 1;
                if (key == VirtualKey::PageUp)
                    browser_action = -1;
                if (key == VirtualKey::PageDown)
                    browser_action = 2;
            } else if (mods_open) {
                if (key == VirtualKey::Escape)
                    mods_action = 1;
                if (key == VirtualKey::X)
                    mods_action = home.store.active ? 2 : -1;
                if (key == VirtualKey::Y)
                    mods_action = home.store.active ? 3 : 2;
                if (key == VirtualKey::PageUp)
                    mods_action = 4;
                if (key == VirtualKey::PageDown)
                    mods_action = 5;
                if (key == VirtualKey::Q)
                    mods_action = 6;
                if (key == VirtualKey::E)
                    mods_action = 7;
            }
            if (key == VirtualKey::Left || key == VirtualKey::Up)
                home.navigate(key == VirtualKey::Up ? 0 : 3);
            if (key == VirtualKey::Right || key == VirtualKey::Down || key == VirtualKey::Tab)
                home.navigate(key == VirtualKey::Down ? 2 : 1);
            if (!mods_open && !browser_open && key == VirtualKey::GamepadMenu)
                home.selected = 1;
            if (key == VirtualKey::Enter || key == VirtualKey::Space)
                home.selected = home.focus;
        });
        pointer_pressed = window.PointerPressed([this](auto const &, PointerEventArgs const &e) {
            if (!home.shown || home.loading || home.flip_time >= 0 || setup_busy || transfer_active)
                return;
            auto point = e.CurrentPoint().Position();
            auto bounds = host_window.Bounds();
            home.selected =
                home.hit(point.X * pixel_width / bounds.Width, point.Y * pixel_height / bounds.Height);
        });
        pointer_moved = window.PointerMoved([this](auto const &, PointerEventArgs const &e) {
            if (!home.shown || home.loading || transfer_active || text_entry)
                return;
            auto p = e.CurrentPoint().Position();
            auto bounds = host_window.Bounds();
            int hit = home.hit(p.X * pixel_width / bounds.Width, p.Y * pixel_height / bounds.Height);
            if (hit >= 0 || (home.store.active && hit != -1))
                home.focus = hit;
        });
        character_received =
            window.CharacterReceived([this](auto const &, CharacterReceivedEventArgs const &e) {
                // The focused CoreTextEditContext already receives typed text; feed it here only without one.
                if (!text_entry || edit_context)
                    return;
                auto c = e.KeyCode();
                auto old_length = static_cast<int32_t>(input_text.size());
                if (c >= 32 && c <= 0x10ffff && input_text.size() < 4096) {
                    if (c > 0xffff) {
                        c -= 0x10000;
                        input_text += wchar_t(0xd800 + (c >> 10));
                        input_text += wchar_t(0xdc00 + (c & 1023));
                    } else
                        input_text += wchar_t(c);
                    text_selection = {static_cast<int32_t>(input_text.size()),
                                      static_cast<int32_t>(input_text.size())};
                    if (edit_context)
                        edit_context.NotifyTextChanged({old_length, old_length},
                                                       static_cast<int32_t>(input_text.size()) - old_length,
                                                       text_selection);
                }
            });
        update_size();
    }
    void Load(hstring const &) {}
    void Uninitialize() {
        neutral();
        home.close();
        active(false);
        uwp::shutdown_audio();
        CoreApplication::Suspending(suspending);
        CoreApplication::Resuming(resuming);
        CoreApplication::MainView().Activated(activating);
        host_window.KeyDown(key_down);
        host_window.PointerPressed(pointer_pressed);
        host_window.PointerMoved(pointer_moved);
        host_window.CharacterReceived(character_received);
        host_window.Closed(closed);
        host_window.VisibilityChanged(visibility);
        host_window.Activated(activated);
        host_window.SizeChanged(size);
        winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().DpiChanged(dpi);
        host_window = nullptr;
    }
    void neutral() {
        input::set_gamepad({});
        input::clear_rumble();
        controller.reset();
        home_buttons = ~uint32_t(0);
        if (pad) {
            try {
                pad.Vibration(GamepadVibration{});
            } catch (hresult_error const &) {
            }
        }
    }
    hstring label(uwp::Text key) const { return hstring(uwp::text(key, pt)); }
    void read_preferences() {
        auto values = ApplicationData::Current().LocalSettings().Values();
        auto boolean = [&](wchar_t const *key, bool fallback) {
            auto value = values.TryLookup(key);
            return value ? unbox_value_or<bool>(value, fallback) : fallback;
        };
        auto number = [&](wchar_t const *key, int fallback) {
            auto value = values.TryLookup(key);
            return value ? unbox_value_or<int32_t>(value, fallback) : fallback;
        };
        preferences.setup_complete = boolean(L"setupComplete", false);
        preferences.auto_launch = boolean(L"autoLaunch", false);
        preferences.fps = number(L"fps", 60) >= 60 ? 60 : 30;
        preferences.volume = std::clamp(number(L"volume", 100), 0, 100);
        preferences.layout = number(L"layout", 0) == 1 ? 1 : 0;
        preferences.render_scale_percent = std::clamp(number(L"renderScalePercent", 0), 0, 300);
        preferences.upscaler = std::clamp(number(L"upscaler", 0), 0, 3);
        preferences.fsr_quality = std::clamp(number(L"fsrQuality", 0), 0, 4);
        preferences.frame_generation = std::clamp(number(L"frameGeneration", 0), 0, 2);
    }
    void save_preferences() {
        auto values = ApplicationData::Current().LocalSettings().Values();
        values.Insert(L"setupComplete", box_value(preferences.setup_complete));
        values.Insert(L"autoLaunch", box_value(preferences.auto_launch));
        values.Insert(L"fps", box_value(preferences.fps));
        values.Insert(L"volume", box_value(preferences.volume));
        values.Insert(L"layout", box_value(preferences.layout));
        values.Insert(L"renderScalePercent", box_value(preferences.render_scale_percent));
        values.Insert(L"upscaler", box_value(preferences.upscaler));
        values.Insert(L"fsrQuality", box_value(preferences.fsr_quality));
        values.Insert(L"frameGeneration", box_value(preferences.frame_generation));
        Settings s = settings::get();
        s.fps = preferences.fps;
        s.volume = preferences.volume;
        s.layout = preferences.layout;
        s.scale = preferences.render_scale_percent / 100.f;
        s.upscaler = static_cast<fsr::Upscaler>(preferences.upscaler);
        s.fsr_quality = static_cast<fsr::Quality>(preferences.fsr_quality);
        s.frame_generation = static_cast<fsr::FrameGeneration>(preferences.frame_generation);
        s.gyro = false;
        settings::set(s, false);
    }
    IAsyncAction scan() {
        auto lifetime = get_strong();
        auto dispatcher = host_window.Dispatcher();
        auto saved_preferences = preferences;
        auto previous = selected_game ? to_string(selected_game.Path()) : std::string{};
        co_await resume_background();
        std::vector<uwp::Candidate> found;
        std::vector<StorageFolder> folders;
        std::vector<std::string> source_ids;
        auto add = [&](StorageFolder const &folder, uwp::Source source,
                       std::string identity = std::string{}) {
            found.push_back(inspect(folder, source));
            folders.push_back(folder);
            source_ids.push_back(std::move(identity));
        };
        try {
            for (auto path : {L"game", L""}) {
                auto folder = *path ? child(local, path) : local;
                if (folder)
                    add(folder, uwp::Source::local);
            }
            for (auto &item : uwp::setup::scan(local)) {
                if (item.format == uwp::setup::Format::folder && item.drive.Path() != local.Path())
                    add(item.folder, uwp::Source::removable,
                        to_string(std::filesystem::path(item.folder.Path().c_str())
                                      .lexically_relative(std::filesystem::path(item.drive.Path().c_str()))
                                      .generic_wstring()));
            }
        } catch (hresult_error const &) { /* LocalState remains usable if removable access is denied. */
        }
        uwp::GameArt loaded_art;
        auto decision = uwp::decide(found, saved_preferences);
        if (!previous.empty())
            for (auto &candidate : found)
                if (candidate.path == previous && candidate.valid()) {
                    decision.game = candidate;
                    break;
                }
        if (uwp::setup::parse_mode(saved_usb_mode) == uwp::setup::Mode::external) {
            decision.game.reset();
            for (size_t i = 0; i < found.size(); ++i) {
                if (found[i].source != uwp::Source::removable || !found[i].valid())
                    continue;
                if (!saved_game_source.empty() && source_ids[i] != saved_game_source)
                    continue;
                decision.game = found[i];
                break;
            }
        }
        StorageFolder active_folder{nullptr};
        if (decision.game) {
            for (size_t i = 0; i < found.size(); ++i) {
                if (found[i].path != decision.game->path)
                    continue;
                active_folder = folders[i];
                auto meta = child(folders[i], L"meta");
                loaded_art.tv = read_art(meta, L"bootTvTex.tga");
                loaded_art.icon = read_art(meta, L"iconTex.tga");
                loaded_art.drc = read_art(meta, L"bootDrcTex.tga");
                break;
            }
        }
        if (decision.game) {
            if (auto downloaded = download_disc(local, active_folder))
                loaded_art.icon = std::move(downloaded);
        }
        co_await resume_foreground(dispatcher);
        art = std::move(loaded_art);
        candidates = std::move(found);
        selected_game = active_folder;
    }
    IAsyncOperation<int32_t> choose(hstring title, hstring body, std::vector<hstring> choices) {
        auto cancellation = co_await get_cancellation_token();
        cancellation.enable_propagation();
        MessageDialog dialog(body, title);
        for (size_t i = 0; i < choices.size(); ++i)
            dialog.Commands().Append(UICommand(choices[i], nullptr, box_value(static_cast<int32_t>(i))));
        dialog.DefaultCommandIndex(0);
        dialog.CancelCommandIndex(static_cast<uint32_t>(choices.size() - 1));
        auto command = co_await dialog.ShowAsync();
        co_return command ? unbox_value<int32_t>(command.Id()) : static_cast<int32_t>(choices.size() - 1);
    }
    void resize_home() {
        if (!home.ready())
            return;
        auto heading = home.heading, detail = home.detail, loading_text = home.loading_text;
        auto rows = home.rows;
        auto row_enabled = home.row_enabled;
        bool mods_mode = home.mods_mode;
        bool setup_mode = home.setup_mode;
        bool diagnostics_mode = home.diagnostics_mode;
        auto issue_kinds = home.issue_kinds;
        auto issue_notes = home.issue_notes;
        bool silver_setup = home.silver_setup, missing_game = home.missing_game;
        double flip_time = home.flip_time;
        auto modules = home.qr_modules;
        float progress = home.progress;
        bool shown = home.shown, loading = home.loading;
        int focus = home.focus;
        try {
            home.open(static_cast<::IUnknown *>(uwp::core_window()), pixel_width, pixel_height, art);
            home.heading = std::move(heading);
            home.detail = std::move(detail);
            home.loading_text = std::move(loading_text);
            home.rows = std::move(rows);
            home.row_enabled = std::move(row_enabled);
            home.mods_mode = mods_mode;
            home.setup_mode = setup_mode;
            home.diagnostics_mode = diagnostics_mode;
            home.issue_kinds = std::move(issue_kinds);
            home.issue_notes = std::move(issue_notes);
            home.silver_setup = silver_setup;
            home.missing_game = missing_game;
            home.flip_time = flip_time;
            home.qr_modules = std::move(modules);
            home.progress = progress;
            home.shown = shown;
            home.loading = loading;
            home.focus = focus;
        } catch (std::exception const &) {
            home.close();
        }
    }
    void show_home() {
        home.open(static_cast<::IUnknown *>(uwp::core_window()), pixel_width, pixel_height, art);
        home.missing_game = !selected_game;
        home.heading = label(uwp::Text::title).c_str();
        home.loading_text = label(uwp::Text::loading).c_str();
        home_buttons = ~uint32_t(0); // Require release before accepting a controller action.
    }
    IAsyncOperation<int32_t> choose_home(hstring body, std::vector<hstring> choices) {
        try {
            show_home();
        } catch (std::exception const &) {
            home.close();
        }
        if (!home.ready())
            co_return co_await choose(label(uwp::Text::title), body, choices);
        home.detail = body.c_str();
        auto dispatcher = host_window.Dispatcher();
        while (home.selected < 0 && home.shown && !quit) {
            co_await resume_after(std::chrono::milliseconds(16));
            co_await resume_foreground(dispatcher);
        }
        if (!home.ready() && !quit)
            co_return co_await choose(label(uwp::Text::title), body, choices);
        int result = home.selected < 0 ? 0 : home.selected;
        home.shown = false;
        co_return result;
    }
    IAsyncAction select_folder() { co_await run_setup(true); }
    std::wstring setup_label(uwp::Text key) const { return std::wstring(uwp::text(key, pt)); }
    void setup_page(uwp::Text title, std::wstring body, std::vector<std::wstring> rows = {}, int focus = 0) {
        if (!home.ready())
            show_home();
        home.shown = true;
        home.setup_mode = true;
        home.silver_setup = true;
        home.diagnostics_mode = false;
        home.mods_mode = false;
        home.loading = false;
        home.heading = setup_label(title);
        home.detail = std::move(body);
        home.rows = std::move(rows);
        home.row_enabled.clear();
        home.qr_modules.clear();
        home.progress = -1;
        home.focus = focus;
        home.selected = -1;
        home.loading_text = setup_label(uwp::Text::setup_hints);
        setup_guide = false;
        setup_action = 0;
        home_buttons = ~uint32_t(0);
    }
    IAsyncOperation<int32_t> setup_choice() {
        auto dispatcher = host_window.Dispatcher();
        while (!quit && home.selected < 0 && !setup_action) {
            co_await resume_after(std::chrono::milliseconds(16));
            co_await resume_foreground(dispatcher);
        }
        if (setup_action)
            co_return setup_action;
        co_return quit ? -2 : home.selected;
    }
    IAsyncAction setup_work(std::function<void()> work, bool quiet = false) {
        auto lifetime = get_strong();
        auto dispatcher = host_window.Dispatcher();
        setup_busy = !quiet;
        setup_progress.cancel = false;
        setup_progress.done = 0;
        setup_progress.total = 0;
        setup_error.clear();
        if (!quiet) {
            home.selected = -1;
            if (!home.diagnostics_mode)
                home.rows.clear();
            home.loading_text = setup_label(uwp::Text::setup_cancel);
        }
        std::string error;
        co_await resume_background();
        try {
            work();
        } catch (uwp::setup::Cancelled const &) {
            error = "cancelled";
        } catch (winrt::hresult_error const &) {
            error = "storage";
        } catch (std::exception const &e) {
            error = e.what();
        }
        co_await resume_foreground(dispatcher);
        setup_error = std::move(error);
        setup_busy = false;
        home.progress = -1;
    }
    std::wstring free_space(uwp::setup::Space value) const {
        if (!value.known)
            return setup_label(uwp::Text::space_unknown);
        return setup_label(uwp::Text::space_free) + uwp::setup::gigabytes(value.free, pt) +
               setup_label(uwp::Text::space_of) + uwp::setup::gigabytes(value.capacity, pt);
    }
    IAsyncAction run_setup(bool storage_only = false, bool recover = false) {
        auto lifetime = get_strong();
        auto dispatcher = host_window.Dispatcher();
        using uwp::Text;
        using uwp::setup::Step;
        uwp::setup::State state;
        state.mode = uwp::setup::parse_mode(saved_usb_mode);
        if (storage_only || recover)
            state.step = Step::detect;
        state.drive_removed = recover;
        setup_active = true;
        struct SetupGuard {
            App &app;
            ~SetupGuard() {
                app.setup_active = false;
                app.setup_detect = false;
                app.setup_busy = false;
                app.home.setup_mode = false;
                app.home.diagnostics_mode = false;
                app.home.shown = false;
                app.home.qr_modules.clear();
            }
        } guard{*this};
        uwp::setup::DiagnosticCache diagnostic_cache;
        uwp::setup::Item selected;
        StorageFolder game{nullptr};
        uint64_t game_size = 0;
        bool dlc = false;
        std::string hash, xml;
        for (;;) {
            if (quit)
                co_return;
            setup_detect = state.step == Step::detect;
            if (state.step == Step::welcome) {
                setup_page(Text::setup_welcome, setup_label(Text::setup_intro),
                           {setup_label(Text::setup_next)});
                int choice = co_await setup_choice();
                if (choice >= 0)
                    state.next();
            } else if (state.step == Step::guide) {
                const Text pages[] = {Text::guide_aroma, Text::guide_dump, Text::guide_merge,
                                      Text::guide_usb};
                setup_page(Text::setup_guide,
                           setup_label(pages[state.page]) +
                               L"\n\nhttps://wiiu.hacks.guide/\nhttps://github.com/emiyl/dumpling\n\n" +
                               std::to_wstring(state.page + 1) + L" / 4",
                           {setup_label(Text::setup_next)});
                home.loading_text = setup_label(Text::guide_hints);
                setup_guide = true;
                const std::string url = "https://wiiu.hacks.guide/";
                auto qr = qrcodegen::QrCode::encodeSegments(
                    {qrcodegen::QrSegment::makeBytes(std::vector<uint8_t>(url.begin(), url.end()))},
                    qrcodegen::QrCode::Ecc::MEDIUM, 1, 5, -1, false);
                home.qr_modules.resize(qr.getSize(), std::vector<bool>(qr.getSize()));
                for (int y = 0; y < qr.getSize(); ++y)
                    for (int x = 0; x < qr.getSize(); ++x)
                        home.qr_modules[y][x] = qr.getModule(x, y);
                int choice = co_await setup_choice();
                if (choice == -2 || choice == -3)
                    state.back();
                else if (choice == -4 || choice >= 0)
                    state.next();
            } else if (state.step == Step::detect) {
                setup_page(Text::setup_insert,
                           setup_label(state.drive_removed ? Text::drive_removed : Text::setup_search));
                std::vector<uwp::setup::Item> items;
                bool first_scan = true;
                while (state.step == Step::detect && !quit) {
                    std::vector<uwp::setup::Diagnosis> diagnoses;
                    co_await setup_work(
                        [&] {
                            items = uwp::setup::scan(local);
                            diagnoses = uwp::setup::scan_diagnostics(local, diagnostic_cache);
                            for (auto &item : items) {
                                if (item.format != uwp::setup::Format::folder || item.layers.empty())
                                    continue;
                                std::vector<uwp::setup::Probe> layers;
                                for (auto &layer : item.layers) {
                                    if (layer.prefix == "dlc/")
                                        continue;
                                    layers.push_back(uwp::setup::storage_probe(layer.folder));
                                    auto path = to_string(layer.folder.Path());
                                    std::erase_if(diagnoses, [&](auto &d) {
                                        return d.folder == path && d.format == uwp::setup::Format::folder;
                                    });
                                }
                                diagnoses.push_back(
                                    uwp::setup::diagnose(uwp::setup::layered_probe(std::move(layers)),
                                                         to_string(item.folder.Path()), item.format));
                                uwp::setup::log_diagnosis(diagnoses.back());
                            }
                        },
                        true);
                    std::sort(items.begin(), items.end(), [](auto &a, auto &b) {
                        auto key = [](auto &item) {
                            return std::wstring(item.folder.Path()) + L"/" + item.name;
                        };
                        return key(a) < key(b);
                    });
                    std::sort(diagnoses.begin(), diagnoses.end(), [](auto &a, auto &b) {
                        return std::tie(a.folder, a.format, a.source) <
                               std::tie(b.folder, b.format, b.source);
                    });
                    std::vector<int> valid_items;
                    for (size_t i = 0; i < items.size(); ++i) {
                        auto &item = items[i];
                        if (uwp::setup::usable_source(diagnoses, to_string(item.folder.Path()), item.format,
                                                      item.file ? to_string(item.file.Name()) : ""))
                            valid_items.push_back(int(i));
                    }
                    if (valid_items.size() == 1) {
                        selected = items[size_t(valid_items.front())];
                        home.diagnostics_mode = false;
                        state.found();
                        break;
                    }
                    std::vector<std::wstring> rows, notes;
                    std::vector<int> kinds, item_rows;
                    auto row = [&](std::wstring path, std::wstring note, int kind = -1, int item = -1) {
                        rows.push_back(std::move(path));
                        notes.push_back(std::move(note));
                        kinds.push_back(kind);
                        item_rows.push_back(item);
                    };
                    bool partial = false, keys_only = true;
                    if (valid_items.empty()) {
                        row(setup_label(Text::select_game_folder), setup_label(Text::game_not_found), -1, -2);
                        row(setup_label(Text::scan_again), L"", -1, -3);
                        row(setup_label(Text::back), L"", -1, -4);
                    }
                    for (auto &d : diagnoses)
                        if (valid_items.empty() && d.incomplete()) {
                            partial = true;
                            keys_only &= std::all_of(d.issues.begin(), d.issues.end(), [](auto &issue) {
                                return issue.kind == uwp::setup::IssueKind::keys;
                            });
                            row(to_hstring(d.source.empty() ? d.folder : d.folder + "/" + d.source).c_str(),
                                L"");
                            for (auto &issue : d.issues)
                                row(to_hstring(issue.path).c_str(), uwp::setup::issue_text(issue, pt),
                                    int(issue.kind));
                            if (d.format == uwp::setup::Format::wud || d.format == uwp::setup::Format::wux)
                                row(setup_label(Text::keys_missing), setup_label(Text::own_keys));
                            else
                                row(setup_label(Text::copy_missing) + std::wstring(to_hstring(d.folder)) +
                                        setup_label(Text::copy_missing_end),
                                    L"");
                        }
                    for (auto i : valid_items)
                        row(items[size_t(i)].name, std::wstring(items[size_t(i)].folder.Path()), -1, i);
                    if (valid_items.empty() && accepted_forms) {
                        for (auto key : {Text::accepted_folder, Text::accepted_disc, Text::accepted_nus,
                                         Text::accepted_wua, Text::accepted_archive})
                            row(setup_label(key), L"");
                    }
                    bool diagnostics_mode = partial || (valid_items.empty() && accepted_forms);
                    auto heading =
                        setup_label(partial ? (keys_only ? Text::keys_missing : Text::files_missing)
                                            : Text::setup_insert);
                    auto detail = valid_items.empty() ? setup_label(Text::game_not_found) : L"";
                    // Keep focus, scroll and pending input intact across identical scan snapshots.
                    if (first_scan || rows != home.rows || notes != home.issue_notes ||
                        kinds != home.issue_kinds || heading != home.heading || detail != home.detail ||
                        diagnostics_mode != home.diagnostics_mode) {
                        home.rows = std::move(rows);
                        home.issue_notes = std::move(notes);
                        home.issue_kinds = std::move(kinds);
                        home.heading = std::move(heading);
                        home.detail = std::move(detail);
                        home.diagnostics_mode = diagnostics_mode;
                        home.selected = -1;
                        home.focus = std::clamp(home.focus, 0, std::max(0, int(home.rows.size()) - 1));
                        first_scan = false;
                    }
                    home.loading_text = setup_label(Text::diagnostic_hints);
                    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    while (!quit && home.selected < 0 && !setup_action &&
                           std::chrono::steady_clock::now() < until) {
                        co_await resume_after(std::chrono::milliseconds(16));
                        co_await resume_foreground(dispatcher);
                    }
                    int action = home.selected >= 0 && size_t(home.selected) < item_rows.size()
                                     ? item_rows[size_t(home.selected)]
                                     : -1;
                    if (setup_action == -2 || action == -4) {
                        if (storage_only)
                            co_return;
                        state.back();
                        break;
                    }
                    if (setup_action == -5 || action == -3) {
                        setup_action = 0;
                        home.selected = -1;
                        continue;
                    }
                    if (action == -2) {
                        // Pick on the UI apartment, then use the brokered grant in the same scan.
                        StorageFolder picked{nullptr};
                        try {
                            Pickers::FolderPicker picker;
                            picker.SuggestedStartLocation(Pickers::PickerLocationId::ComputerFolder);
                            picker.FileTypeFilter().Append(L"*");
                            picked = co_await picker.PickSingleFolderAsync();
                            if (picked) {
                                AccessCache::StorageApplicationPermissions::FutureAccessList().AddOrReplace(
                                    L"game-folder", picked);
                                LOG("Setup picked folder: %s", to_string(picked.Path()).c_str());
                            }
                        } catch (hresult_error const &e) {
                            LOG("Setup folder picker failed: 0x%08x", unsigned(e.code().value));
                        }
                        home.selected = -1;
                        continue;
                    }
                    if (action >= 0) {
                        selected = items[size_t(action)];
                        home.diagnostics_mode = false;
                        state.found();
                    }
                }
            } else if (state.step == Step::verify) {
                game = selected.folder;
                if (selected.format != uwp::setup::Format::folder || !selected.layers.empty()) {
                    setup_page(Text::setup_extract, L"");
                    co_await setup_work([&] {
                        auto entries = uwp::setup::source_entries(selected, setup_progress);
                        uint64_t size = 0;
                        for (auto &entry : entries)
                            size += entry.size;

                        auto suffix = uwp::setup::source_identity(selected, setup_progress);
                        game = uwp::setup::extraction_folder(selected, local, suffix);
                        auto existing = uwp::setup::tree_size(game, setup_progress);
                        auto available = uwp::setup::space(selected.drive);
                        if (!available.fits(size > existing ? size - existing : 0))
                            throw std::runtime_error("USB space");
                        uwp::setup::extract(entries, game, local,
                                            std::wstring(L"extract-") + std::wstring(to_hstring(suffix)) +
                                                L".txt",
                                            setup_progress, true);
                    });
                    if (!setup_error.empty()) {
                        Text message = setup_error.find("otp.bin") != std::string::npos ? Text::missing_common
                                       : setup_error.find("disc key") != std::string::npos
                                           ? Text::missing_disc
                                       : setup_error == "USB space" ? Text::usb_space_insufficient
                                                                    : Text::setup_failed;
                        if (setup_error != "cancelled") {
                            setup_page(Text::setup_extract,
                                       setup_label(message) +
                                           ((message == Text::missing_common || message == Text::missing_disc)
                                                ? L"\n\n" + setup_label(Text::setup_keys)
                                                : L""),
                                       {setup_label(Text::retry)});
                            if (selected.file && message == Text::setup_failed) {
                                home.heading = setup_label(Text::files_missing);
                                home.diagnostics_mode = true;
                                home.rows = {
                                    std::wstring(selected.folder.Path()), std::wstring(selected.file.Name()),
                                    setup_label(Text::copy_missing) + std::wstring(selected.folder.Path()) +
                                        setup_label(Text::copy_missing_end)};
                                home.issue_kinds = {-1, int(uwp::setup::IssueKind::incomplete), -1};
                                home.issue_notes = {L"",
                                                    setup_error == "Extract .7z on a PC"
                                                        ? setup_label(Text::unsupported)
                                                        : setup_label(Text::fix_incomplete),
                                                    L""};
                                home.loading_text = setup_label(Text::diagnostic_hints);
                            }
                            co_await setup_choice();
                        }
                        state.step = Step::detect;
                        state.verified = false;
                        state.drive_removed = setup_error == "storage";
                        continue;
                    }
                }
                setup_page(Text::setup_verify, L"");
                co_await setup_work([&] {
                    hash =
                        uwp::setup::hash_file(game.GetFileAsync(L"code\\U-King.rpx").get(), setup_progress);
                    xml = uwp::setup::read_text(game, L"code\\app.xml");
                    dlc = game.TryGetItemAsync(L"aoc").get() != nullptr;
                    game_size = uwp::setup::tree_size(game, setup_progress);
                    if (hash == uwp::setup::expected_rpx && game.TryGetItemAsync(L"setup-source.txt").get())
                        uwp::setup::write_text(game, L"setup-complete.txt", "v208\n");
                });
                if (!setup_error.empty()) {
                    state.removed();
                    continue;
                }
                state.hash(hash);
            } else if (state.step == Step::wrong_version) {
                auto body = setup_label(Text::setup_wrong);
                if (auto version = uwp::setup::title_version(xml))
                    body += L"\n\n" + setup_label(Text::version_found) + L"v" + std::to_wstring(*version);
                setup_page(Text::setup_verify, body, {setup_label(Text::back)});
                co_await setup_choice();
                state.back();
            } else if (state.step == Step::storage) {
                uwp::setup::Space internal, external;
                bool existing = false, available = false;
                bool refreshing =
                    home.setup_mode && home.heading == setup_label(Text::setup_storage) && !home.rows.empty();
                if (!refreshing)
                    setup_page(Text::setup_storage, L"");
                co_await resume_background();
                try {
                    internal = uwp::setup::space(local);
                    if (selected.drive.Path() != local.Path())
                        external = uwp::setup::space(selected.drive);
                    else {
                        auto drives = KnownFolders::RemovableDevices().GetFoldersAsync().get();
                        if (drives.Size())
                            external = uwp::setup::space(drives.GetAt(0));
                    }
                    existing = local.TryGetItemAsync(L"game").get() != nullptr;
                    available = uwp::setup::game_folder(game);
                } catch (hresult_error const &) {
                }
                co_await resume_foreground(dispatcher);
                int pending_selection = home.selected, pending_action = setup_action,
                    pending_focus = home.focus;
                uint32_t pending_buttons = home_buttons;
                if (!available) {
                    state.removed();
                    continue;
                }
                bool is_internal =
                    std::wstring_view(game.Path()).starts_with(std::wstring_view(local.Path()));
                auto body = setup_label(Text::space_game) + uwp::setup::gigabytes(game_size, pt) + L"\n" +
                            setup_label(Text::space_usb) + free_space(external) + L"\n" +
                            setup_label(Text::space_console) + free_space(internal);
                if (!dlc)
                    body += L"\n" + setup_label(Text::dlc_optional);
                if (!internal.fits(game_size))
                    body += L"\n" + setup_label(Text::space_insufficient);
                if (existing && !is_internal)
                    body += L"\n" + setup_label(Text::existing_import);
                setup_page(Text::setup_storage, body,
                           {setup_label(Text::play_usb), setup_label(Text::import_internal),
                            setup_label(Text::back)},
                           state.storage_focus(internal, game_size));
                home.row_enabled = {!is_internal, (internal.fits(game_size) && !existing) || is_internal,
                                    true};
                if (refreshing) {
                    home.selected = pending_selection;
                    setup_action = pending_action;
                    home.focus = pending_focus;
                    home_buttons = pending_buttons;
                }

                if (!home.enabled(size_t(home.focus)))
                    home.focus = 0;
                if (is_internal) {
                    home.focus = 1;
                    home.rows[1] = setup_label(Text::setup_next);
                }
                // Refresh capacities while the player is deciding, including external free space.
                auto next = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (!quit && home.selected < 0 && !setup_action) {
                    co_await resume_after(std::chrono::milliseconds(32));
                    co_await resume_foreground(dispatcher);
                    if (std::chrono::steady_clock::now() >= next)
                        break;
                }
                if (home.selected < 0 && !setup_action) {
                    state.mode = home.focus == 1 ? uwp::setup::Mode::copy : uwp::setup::Mode::external;
                    continue;
                }
                int choice = setup_action ? setup_action : home.selected;
                if (choice == -2 || choice == 2) {
                    if (storage_only)
                        co_return;
                    state.back();
                    continue;
                }
                if (choice == 1 && !is_internal) {
                    if (!internal.fits(game_size) || existing)
                        continue;
                    setup_page(Text::setup_copy, L"");
                    StorageFolder imported{nullptr};
                    co_await setup_work([&] {
                        StorageFolder staging{nullptr};
                        try {
                            require_no_existing_import();
                            // A suspend or kill mid-copy leaves a staging folder behind; reclaim it.
                            for (auto const &folder : local.GetFoldersAsync().get())
                                if (std::wstring_view(folder.Name()).starts_with(L"game-import"))
                                    folder.DeleteAsync(StorageDeleteOption::PermanentDelete).get();
                            staging = local
                                          .CreateFolderAsync(L"game-import",
                                                             CreationCollisionOption::GenerateUniqueName)
                                          .get();
                            std::vector<uwp::formats::Entry> entries;
                            uwp::setup::folder_entries(game, "", entries, setup_progress);
                            if (!uwp::setup::space(local).fits(game_size))
                                throw std::runtime_error("Internal space changed");
                            uwp::setup::extract(entries, staging, local, L"", setup_progress, false);
                            auto copied = uwp::setup::hash_file(
                                staging.GetFileAsync(L"code\\U-King.rpx").get(), setup_progress);
                            uwp::formats::require(copied == uwp::setup::expected_rpx,
                                                  "Imported RPX hash mismatch");
                            setup_progress.check();
                            staging.RenameAsync(L"game", NameCollisionOption::FailIfExists).get();
                            imported = staging;
                            uwp::setup::write_text(local, L"game-imported.txt", "1\n");
                        } catch (...) {
                            if (staging) {
                                try {
                                    staging.DeleteAsync(StorageDeleteOption::PermanentDelete).get();
                                } catch (...) {
                                }
                            }
                            throw;
                        }
                    });
                    if (!setup_error.empty()) {
                        if (setup_error != "cancelled") {
                            setup_page(Text::setup_copy, setup_label(Text::setup_failed),
                                       {setup_label(Text::retry)});
                            co_await setup_choice();
                        }
                        continue;
                    }
                    game = imported;
                }
                state.mode = choice == 0 ? uwp::setup::Mode::external : uwp::setup::Mode::copy;
                saved_usb_mode = uwp::setup::mode_name(state.mode);
                saved_game_source =
                    to_string(std::filesystem::path(game.Path().c_str())
                                  .lexically_relative(std::filesystem::path(selected.drive.Path().c_str()))
                                  .generic_wstring());
                co_await setup_work([&] {
                    uwp::setup::write_text(local, L"usb_mode.txt", saved_usb_mode + "\n");
                    uwp::setup::write_text(local, L"game_source.txt", saved_game_source);
                });
                if (!setup_error.empty())
                    continue;
                // Preserve the selected source in memory only. Each startup re-enumerates removable drives.
                selected_game = game;
                candidates = {{to_string(game.Path()),
                               std::wstring_view(game.Path()).starts_with(std::wstring_view(local.Path()))
                                   ? uwp::Source::local
                                   : uwp::Source::removable,
                               true, true, true}};
                if (storage_only)
                    co_return;
                state.next();
            } else if (state.step == Step::automatic) {
                setup_page(Text::title, setup_label(Text::auto_question),
                           {setup_label(Text::yes), setup_label(Text::no)});
                int choice = co_await setup_choice();
                if (choice == -2) {
                    state.back();
                    continue;
                }
                preferences.auto_launch = choice == 0;
                co_await setup_work([&] { uwp::setup::write_text(local, L"setup_done.txt", "1\n"); });
                if (!setup_error.empty())
                    continue;
                preferences.setup_complete = true;
                save_preferences();
                state.next();
            } else {
                co_await scan();
                co_return;
            }
        }
    }
    void require_no_existing_import() {
        uwp::formats::require(!local.TryGetItemAsync(L"game").get(),
                              "Remove existing imported copy before importing");
    }

    IAsyncAction configure() {
        for (;;) {
            auto choice = co_await choose(
                label(uwp::Text::settings), label(uwp::Text::controls),
                {label(uwp::Text::automatic), label(uwp::Text::settings_more), label(uwp::Text::back)});
            if (choice == 2)
                break;
            if (choice == 0) {
                preferences.auto_launch =
                    (co_await choose(label(uwp::Text::automatic), L"",
                                     {label(uwp::Text::yes), label(uwp::Text::no)})) == 0;
            } else {
                auto section = co_await choose(
                    label(uwp::Text::settings), L"",
                    {label(uwp::Text::storage_settings), label(uwp::Text::fps), label(uwp::Text::back)});
                if (section == 2)
                    continue;
                if (section == 0) {
                    if (running) {
                        co_await choose(label(uwp::Text::storage_settings), label(uwp::Text::restart),
                                        {label(uwp::Text::back)});
                        continue;
                    }
                    auto action = co_await choose(label(uwp::Text::storage_settings), L"",
                                                  {label(uwp::Text::setup_storage),
                                                   label(uwp::Text::settings_more), label(uwp::Text::back)});
                    if (action == 2)
                        continue;
                    if (action == 0) {
                        co_await run_setup(true);
                        continue;
                    }
                    auto maintenance =
                        co_await choose(label(uwp::Text::storage_settings), L"",
                                        {label(uwp::Text::remove_import), label(uwp::Text::setup_again),
                                         label(uwp::Text::back)});
                    if (maintenance == 2)
                        continue;
                    if (maintenance == 1) {
                        co_await run_setup();
                    } else {
                        uwp::setup::Space internal, external;
                        bool has_import = false;
                        auto dispatcher = host_window.Dispatcher();
                        co_await resume_background();
                        try {
                            internal = uwp::setup::space(local);
                            has_import = local.TryGetItemAsync(L"game-imported.txt").get() != nullptr;
                            auto drives = KnownFolders::RemovableDevices().GetFoldersAsync().get();
                            if (drives.Size())
                                external = uwp::setup::space(drives.GetAt(0));
                        } catch (hresult_error const &) {
                        }
                        co_await resume_foreground(dispatcher);
                        setup_active = true;
                        setup_page(
                            uwp::Text::remove_import,
                            setup_label(has_import ? uwp::Text::remove_question : uwp::Text::no_import) +
                                L"\n\n" + setup_label(uwp::Text::space_console) + free_space(internal) +
                                L"\n" + setup_label(uwp::Text::space_usb) + free_space(external),
                            {setup_label(uwp::Text::no), setup_label(uwp::Text::yes)});
                        home.row_enabled = {true, has_import};
                        auto confirmation = co_await setup_choice();
                        setup_active = false;
                        home.setup_mode = false;
                        home.shown = false;
                        if (confirmation == 1) {
                            auto dispatcher = host_window.Dispatcher();
                            bool removed = false;
                            co_await resume_background();
                            try {
                                if (local.TryGetItemAsync(L"game-imported.txt").get()) {
                                    auto folder = child(local, L"game");
                                    if (folder)
                                        folder.DeleteAsync(StorageDeleteOption::PermanentDelete).get();
                                    local.GetFileAsync(L"game-imported.txt")
                                        .get()
                                        .DeleteAsync(StorageDeleteOption::PermanentDelete)
                                        .get();
                                    removed = true;
                                }
                            } catch (hresult_error const &) {
                            }
                            co_await resume_foreground(dispatcher);
                            if (removed)
                                co_await scan();
                        }
                    }
                    continue;
                }
                auto selected =
                    co_await choose(label(uwp::Text::fps), L"", {L"30", L"60", label(uwp::Text::settings)});
                if (selected < 2)
                    preferences.fps = selected == 0 ? 30 : 60;
                else {
                    auto setting = co_await choose(
                        label(uwp::Text::settings), L"",
                        {label(uwp::Text::volume), label(uwp::Text::layout), label(uwp::Text::back)});
                    if (setting == 0) {
                        auto v = co_await choose(label(uwp::Text::volume), L"", {L"0%", L"50%", L"100%"});
                        preferences.volume = v * 50;
                    } else if (setting == 1) {
                        preferences.layout =
                            co_await choose(label(uwp::Text::layout), L"",
                                            {label(uwp::Text::positions), label(uwp::Text::labels)});
                    }
                }
            }
            save_preferences();
        }
    }
    // Use brokered StorageFolder operations: picker paths alone do not grant CRT access.
    static size_t copy_mod_tree(StorageFolder const &source, StorageFolder const &destination) {
        size_t count = 0;
        for (auto const &file : source.GetFilesAsync().get()) {
            file.CopyAsync(destination, file.Name(), NameCollisionOption::ReplaceExisting).get();
            ++count;
        }
        for (auto const &folder : source.GetFoldersAsync().get()) {
            auto target =
                destination.CreateFolderAsync(folder.Name(), CreationCollisionOption::OpenIfExists).get();
            count += copy_mod_tree(folder, target);
        }
        return count;
    }
    static bool import_candidates(StorageFolder const &source, StorageFolder const &destination) {
        // A picker can select LocalState itself. Never copy a tree into itself.
        auto from = std::filesystem::path(source.Path().c_str()).lexically_normal();
        auto to = std::filesystem::path(destination.Path().c_str()).lexically_normal();
        auto relative = to.lexically_relative(from);
        if (!relative.empty() && *relative.begin() != L"..")
            return false;
        std::vector<StorageFolder> folders;
        if (child(source, L"content") || child(source, L"aoc"))
            folders.push_back(source);
        else
            for (auto const &folder : source.GetFoldersAsync().get())
                folders.push_back(folder);
        bool copied = false;
        for (auto const &folder : folders) {
            if (!child(folder, L"content") && !child(folder, L"aoc"))
                continue;
            // Never overwrite an installed mod, and never import a selection file.
            auto target =
                destination.CreateFolderAsync(folder.Name(), CreationCollisionOption::GenerateUniqueName)
                    .get();
            try {
                if (!copy_mod_tree(folder, target))
                    throw std::runtime_error("No accessible mod files");
            } catch (...) {
                // Never leave a partially copied mod available for activation.
                try {
                    target.DeleteAsync(StorageDeleteOption::PermanentDelete).get();
                } catch (...) {
                }
                throw;
            }
            copied = true;
        }
        return copied;
    }
    IAsyncAction import_mods() {
        auto dispatcher = host_window.Dispatcher();
        auto destination = co_await local.CreateFolderAsync(L"mods", CreationCollisionOption::OpenIfExists);
        co_await resume_background();
        bool copied = false;
        std::exception_ptr import_failure;
        try {
            for (auto const &drive : KnownFolders::RemovableDevices().GetFoldersAsync().get()) {
                auto source = child(drive, L"mods");
                copied |= import_candidates(source ? source : drive, destination);
            }
        } catch (hresult_error const &) {
            // Removable access can be filtered by the manifest; try the picker.
        } catch (...) {
            import_failure = std::current_exception();
        }
        co_await resume_foreground(dispatcher);
        // Missing files/permissions on removableStorage also need the picker grant.
        if (import_failure && copied)
            std::rethrow_exception(import_failure);
        if (!copied) {
            if (winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() ==
                L"Windows.Xbox")
                co_return;
            Pickers::FolderPicker picker;
            picker.SuggestedStartLocation(Pickers::PickerLocationId::ComputerFolder);
            picker.FileTypeFilter().Append(L"*");
            auto folder = co_await picker.PickSingleFolderAsync();
            if (!folder)
                co_return;
            co_await resume_background();
            std::exception_ptr failure;
            try {
                copied = import_candidates(folder, destination);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await resume_foreground(dispatcher);
            if (failure)
                std::rethrow_exception(failure);
            if (!copied)
                throw std::runtime_error("No mod folders found");
        }
    }
    IAsyncOperation<int32_t> online_rows(hstring heading, std::vector<std::wstring> rows,
                                         hstring detail = L"", std::vector<bool> enabled = {}) {
        browser_action = 0;
        if (!home.ready() || running) {
            size_t cursor = 0;
            auto dispatcher = host_window.Dispatcher();
            for (;;) {
                auto operation =
                    choose(heading, hstring(std::wstring(detail) + L"\n\n" + rows.at(cursor)),
                           {label(uwp::Text::confirm), label(uwp::Text::mods_next), label(uwp::Text::back)});
                while (operation.Status() == AsyncStatus::Started && !browser_action && !quit) {
                    co_await resume_after(std::chrono::milliseconds(16));
                    co_await resume_foreground(dispatcher);
                }
                if (browser_action || quit) {
                    operation.Cancel();
                    co_return browser_action == -1 ? -2 : browser_action == 2 ? -3 : -1;
                }
                auto action = co_await operation;
                if (action == 0) {
                    if (cursor < enabled.size() && !enabled[cursor])
                        continue;
                    co_return static_cast<int32_t>(cursor);
                }
                if (action == 2)
                    co_return -1;
                cursor = (cursor + 1) % rows.size();
            }
        }
        home.mods_mode = true;
        home.shown = true;
        home.loading = false;
        home.heading = heading.c_str();
        home.detail = detail.c_str();
        home.loading_text = label(uwp::Text::cancel_transfer).c_str();
        home.rows = std::move(rows);
        home.row_enabled = std::move(enabled);
        home.focus = 0;
        if (!home.enabled(0))
            home.step(1);
        home.selected = -1;
        home_buttons = ~uint32_t(0);
        auto dispatcher = host_window.Dispatcher();
        while (home.selected < 0 && !browser_action && !quit && home.ready()) {
            co_await resume_after(std::chrono::milliseconds(16));
            co_await resume_foreground(dispatcher);
        }
        if (quit || browser_action == 1 || !home.ready())
            co_return -1;
        if (browser_action == -1)
            co_return -2;
        if (browser_action == 2)
            co_return -3;
        co_return home.selected;
    }
    void begin_text_entry() {
        using namespace winrt::Windows::UI::Text::Core;
        input_text.clear();
        text_selection = {0, 0};
        text_entry = true;
        edit_context = CoreTextServicesManager::GetForCurrentView().CreateEditContext();
        edit_context.InputScope(CoreTextInputScope::Default);
        edit_context.InputPaneDisplayPolicy(CoreTextInputPaneDisplayPolicy::Manual);
        edit_context.TextRequested([this](auto const &, CoreTextTextRequestedEventArgs const &args) {
            auto request = args.Request();
            auto range = request.Range();
            auto start = std::clamp(range.StartCaretPosition, 0, static_cast<int32_t>(input_text.size()));
            auto end = std::clamp(range.EndCaretPosition, start, static_cast<int32_t>(input_text.size()));
            request.Text(hstring(input_text.substr(start, end - start)));
        });
        edit_context.SelectionRequested(
            [this](auto const &, CoreTextSelectionRequestedEventArgs const &args) {
                args.Request().Selection(text_selection);
            });
        edit_context.TextUpdating([this](auto const &, CoreTextTextUpdatingEventArgs const &args) {
            auto range = args.Range();
            auto replacement = args.Text();
            if (range.StartCaretPosition < 0 || range.EndCaretPosition < range.StartCaretPosition ||
                range.EndCaretPosition > static_cast<int32_t>(input_text.size()) ||
                input_text.size() - (range.EndCaretPosition - range.StartCaretPosition) + replacement.size() >
                    4096) {
                args.Result(CoreTextTextUpdatingResult::Failed);
                return;
            }
            input_text.replace(range.StartCaretPosition, range.EndCaretPosition - range.StartCaretPosition,
                               replacement.c_str(), replacement.size());
            text_selection = args.NewSelection();
            args.Result(CoreTextTextUpdatingResult::Succeeded);
        });
        edit_context.SelectionUpdating([this](auto const &, CoreTextSelectionUpdatingEventArgs const &args) {
            text_selection = args.Selection();
            args.Result(CoreTextSelectionUpdatingResult::Succeeded);
        });
        edit_context.LayoutRequested([this](auto const &, CoreTextLayoutRequestedEventArgs const &args) {
            auto window = host_window.Bounds();
            auto bounds = args.Request().LayoutBounds();
            bool store = home.store.active;
            Rect field{window.X + (store ? 820 : 64) * window.Width / 1280,
                       window.Y + (store ? 52 : 160) * window.Height / 720,
                       (store ? 412 : 1152) * window.Width / 1280, (store ? 48 : 56) * window.Height / 720};
            bounds.ControlBounds(field);
            bounds.TextBounds(field);
        });
        edit_context.NotifyFocusEnter();
    }
    void end_text_entry() noexcept {
        try {
            if (edit_context)
                edit_context.NotifyFocusLeave();
            winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView().TryHide();
        } catch (...) {
        }
        edit_context = nullptr;
        text_entry = false;
    }
    // Native CoreWindow text dialog, with the system InputPane (including Xbox keyboard).
    // CharacterReceived retains UTF-16 input; no web view, XAML island or URL launch is needed.
    IAsyncOperation<hstring> enter_text(uwp::Text prompt) {
        struct Guard {
            App &app;
            ~Guard() { app.end_text_entry(); }
        } guard{*this};
        begin_text_entry();
        if (!home.ready() || running) {
            // A running guest owns the surface: collect typed input while its native dialog is open.

            auto pane = winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView();
            pane.TryShow();
            MessageDialog dialog(L"", label(prompt));
            dialog.Commands().Append(UICommand(label(uwp::Text::confirm), nullptr, box_value(0)));
            dialog.Commands().Append(UICommand(label(uwp::Text::back), nullptr, box_value(1)));
            dialog.CancelCommandIndex(1);
            auto operation = dialog.ShowAsync();
            auto dispatcher = host_window.Dispatcher();
            while (operation.Status() == AsyncStatus::Started) {
                co_await resume_after(std::chrono::milliseconds(16));
                co_await resume_foreground(dispatcher);
                if (quit || browser_action == 1) {
                    operation.Cancel();
                    co_return hstring{};
                }
                dialog.Content(hstring(input_text));
            }
            auto answer = co_await operation;
            if (answer && unbox_value<int32_t>(answer.Id()) == 0)
                co_return hstring(input_text);
            co_return hstring{};
        }
        browser_action = 0;
        auto pane = winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView();
        home.row_enabled.clear();
        home.heading = label(prompt).c_str();
        home.detail = L"";
        home.rows = {L"", std::wstring(label(uwp::Text::confirm)), std::wstring(label(uwp::Text::back))};
        home.focus = 0;
        home.selected = -1;
        home.shown = true;
        home.mods_mode = true;
        home.loading_text = L"";
        pane.TryShow();
        home_buttons = ~uint32_t(0);
        auto dispatcher = host_window.Dispatcher();
        for (;;) {
            co_await resume_after(std::chrono::milliseconds(16));
            co_await resume_foreground(dispatcher);
            if (quit || browser_action == 1 || home.selected == 2 || !home.ready()) {
                pane.TryHide();
                co_return hstring{};
            }
            if (home.selected == 1) {
                pane.TryHide();
                co_return hstring(input_text);
            }
            if (home.selected == 0) {
                pane.TryShow();
                home.selected = -1;
            }
            home.rows[0] = input_text + L"▏";
        }
    }
    IAsyncAction install_online(mods::OnlineFile file, std::string name) {
        if (!file.supported()) {
            co_await choose(label(uwp::Text::mods), label(uwp::Text::unsupported), {label(uwp::Text::back)});
            co_return;
        }
        auto folder = co_await local.CreateFolderAsync(L"mods", CreationCollisionOption::OpenIfExists);
        auto downloads =
            co_await folder.CreateFolderAsync(L".downloads", CreationCollisionOption::OpenIfExists);
        auto temporary =
            co_await downloads.CreateFileAsync(L"download.zip", CreationCollisionOption::GenerateUniqueName);
        auto path = std::filesystem::path(temporary.Path().c_str());
        auto root = std::filesystem::path(folder.Path().c_str());
        transfer = {};
        transfer_active = true;
        home.focus = 0;
        home.row_enabled.clear();
        home.heading = label(uwp::Text::download).c_str();
        home.detail = L"";
        home.rows = {std::wstring(to_hstring(name))};
        home.loading_text = label(uwp::Text::cancel_transfer).c_str();
        home.selected = -1;
        bool error = false;
        auto dispatcher = host_window.Dispatcher();
        MessageDialog progress_dialog(label(uwp::Text::download), to_hstring(name));
        IAsyncOperation<IUICommand> progress_operation{nullptr};
        auto progress_active = std::make_shared<bool>(true);
        try {
            if (running || !home.ready()) {
                progress_dialog.Commands().Append(UICommand(label(uwp::Text::back)));
                progress_operation = progress_dialog.ShowAsync();
                progress_operation.Completed([this, progress_active](auto const &, auto status) {
                    if (status == AsyncStatus::Completed)
                        host_window.Dispatcher().RunAsync(CoreDispatcherPriority::Normal,
                                                          [this, progress_active] {
                                                              if (*progress_active && transfer_active)
                                                                  transfer.cancel();
                                                          });
                });
            }
            co_await uwp::download_mod(
                file, path, transfer,
                [this, &progress_dialog, &progress_operation](uint64_t size, uint64_t total) {
                    home.rows = {std::to_wstring(size / 1024) + L" / " +
                                 (total ? std::to_wstring(total / 1024) : L"?") + L" KiB"};
                    if (progress_operation)
                        progress_dialog.Content(hstring(home.rows[0]));
                });
            transfer.check();
            // Extraction cannot be interrupted midway; rollback is owned by the installer.
            transfer_active = false;
            home.heading = label(uwp::Text::loading).c_str();
            co_await resume_background();
            std::exception_ptr failure;
            try {
                mods::install_zip(path, root, name);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await resume_foreground(dispatcher);
            if (failure)
                std::rethrow_exception(failure);
        } catch (...) {
            error = true;
        }
        transfer_active = false;
        transfer.set_pending(nullptr);
        *progress_active = false;
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (progress_operation) {
            try {
                progress_operation.Cancel();
                co_await progress_operation;
            } catch (...) {
            }
        }
        if (!transfer.cancelled)
            co_await choose(label(uwp::Text::mods),
                            label(error ? uwp::Text::online_error : uwp::Text::installed),
                            {label(uwp::Text::back)});
    }
    IAsyncAction online_mods(bool link) {
        browser_open = true;
        struct Guard {
            App &app;
            ~Guard() {
                app.browser_open = false;
                app.text_entry = false;
                app.transfer_active = false;
            }
        } guard{*this};
        bool error = false;
        try {
            if (link) {
                auto url = to_string(co_await enter_text(uwp::Text::enter_link));
                if (url.empty())
                    co_return;
                if (!mods::https_zip_url(url))
                    throw std::runtime_error("Invalid ZIP link");
                auto end = url.find('?');
                auto clean = url.substr(0, end);
                auto name = clean.substr(clean.rfind('/') + 1);
                co_await install_online({name, url, "", 0}, name.substr(0, name.size() - 4));
                co_return;
            }
            unsigned page = 1;
            std::string query;
            for (;;) {
                transfer = {};
                transfer_active = true;
                home.focus = 0;
                home.heading = label(uwp::Text::get_mods).c_str();
                home.detail = L"";
                home.rows = {std::wstring(label(uwp::Text::loading))};
                home.loading_text = label(uwp::Text::cancel_transfer).c_str();
                auto json = co_await uwp::fetch_json(mods::gamebanana_url(page, query), transfer);
                transfer_active = false;
                auto result = mods::parse_gamebanana(to_string(json));
                std::vector<std::wstring> rows;
                for (auto &m : result.records)
                    rows.push_back(std::wstring(to_hstring(m.name + " · " + m.category + " · " + m.author)));
                int search_index = static_cast<int>(rows.size());
                rows.push_back(label(uwp::Text::search).c_str());
                rows.push_back(label(uwp::Text::back).c_str());
                auto selected = co_await online_rows(
                    label(uwp::Text::get_mods), std::move(rows),
                    hstring(std::wstring(label(uwp::Text::page)) + L" " + std::to_wstring(page)));
                if (selected == -1 || selected == search_index + 1)
                    co_return;
                if (selected == -2) {
                    if (page > 1)
                        --page;
                    continue;
                }
                if (selected == -3) {
                    if (!result.records.empty() && (!result.count || page * 15 < result.count))
                        ++page;
                    continue;
                }
                if (selected == search_index) {
                    // ponytail: empty = cancelled, keeps the current search; a "clear search" row if players
                    // ask.
                    if (auto typed = to_string(co_await enter_text(uwp::Text::enter_search));
                        !typed.empty()) {
                        query = typed;
                        page = 1;
                    }
                    continue;
                }
                auto mod = result.records.at(selected);
                transfer = {};
                transfer_active = true;
                home.focus = 0;
                home.rows = {std::wstring(label(uwp::Text::loading))};
                auto files_json = co_await uwp::fetch_json(mods::gamebanana_files_url(mod.id), transfer);
                transfer_active = false;
                auto files = mods::parse_gamebanana_files(to_string(files_json));
                std::vector<std::wstring> file_rows;
                for (auto &f : files)
                    file_rows.push_back(
                        std::wstring(to_hstring(f.name)) + L" · " + std::to_wstring(f.size / 1024) + L" KiB" +
                        (f.supported() ? L"" : L" · " + std::wstring(label(uwp::Text::unsupported))));
                file_rows.push_back(label(uwp::Text::back).c_str());
                std::vector<bool> enabled;
                for (auto const &file : files)
                    enabled.push_back(file.supported());
                enabled.push_back(true);
                for (;;) {
                    auto index = co_await online_rows(to_hstring(mod.name), file_rows,
                                                      to_hstring(mod.category + " · " + mod.author), enabled);
                    if (index < 0 || index >= static_cast<int>(files.size()))
                        break;
                    if (!files[index].supported())
                        continue; // Listed, deliberately disabled.
                    co_await install_online(files[index], mod.name);
                    break;
                }
            }
        } catch (...) {
            error = true;
        }
        transfer_active = false;
        transfer.set_pending(nullptr);
        if (error && !transfer.cancelled)
            co_await choose(label(uwp::Text::mods), label(uwp::Text::online_error), {label(uwp::Text::back)});
    }
    std::wstring mod_label(const mods::ModInfo &entry) const {
        auto kind = entry.kind == mods::Kind::translation ? uwp::Text::translation
                    : entry.kind == mods::Kind::dubbing   ? uwp::Text::dubbing
                    : entry.kind == mods::Kind::graphics  ? uwp::Text::graphics
                                                          : uwp::Text::files;
        return std::wstring(to_hstring(entry.name)) + L" · " + label(kind).c_str() + L" · " +
               label(entry.enabled ? uwp::Text::mod_on : uwp::Text::mod_off).c_str() +
               (entry.locked ? L" · " + std::wstring(label(entry.note == "recompile to apply"
                                                               ? uwp::Text::recompile_apply
                                                               : uwp::Text::rebuild))
                             : L"");
    }
#include "store_app.inl"
    IAsyncAction manage_mods_legacy() {
        auto folder = co_await local.CreateFolderAsync(L"mods", CreationCollisionOption::OpenIfExists);
        mods::ModCatalog catalog;
        auto root = std::filesystem::path(folder.Path().c_str());
        catalog.load(root, packs::compiled_plan());
        // A live guest owns the surface. Keep the same management actions in native dialogs.
        bool graphical = !running;
        if (graphical) {
            try {
                show_home();
            } catch (std::exception const &) {
                home.close();
            }
            graphical = home.ready();
        }
        mods_open = true;
        struct Guard {
            App &app;
            ~Guard() {
                app.mods_open = false;
                app.home.mods_mode = false;
                app.home.shown = false;
            }
        } guard{*this};
        home.mods_mode = true;
        home.focus = 0;
        mods_action = 0;
        size_t cursor = 0;
        for (;;) {
            int selected = -1;
            if (graphical) {
                home.heading = label(uwp::Text::mods).c_str();
                home.detail =
                    label(catalog.list().empty() ? uwp::Text::mods_empty : uwp::Text::mods_help).c_str();
                home.loading_text = label(uwp::Text::mods_controls).c_str();
                home.rows.clear();
                home.row_enabled.clear();
                for (auto const &entry : catalog.list())
                    home.rows.push_back(mod_label(entry));
                home.rows.push_back(label(uwp::Text::import_usb).c_str());
                home.rows.push_back(label(uwp::Text::save_mods).c_str());
                home.rows.push_back(label(uwp::Text::get_mods).c_str());
                home.rows.push_back(label(uwp::Text::add_link).c_str());
                home.focus = std::clamp(home.focus, 0, static_cast<int>(home.rows.size()) - 1);
                home.selected = -1;
                auto dispatcher = host_window.Dispatcher();
                while (home.selected < 0 && !mods_action && !quit && home.ready()) {
                    co_await resume_after(std::chrono::milliseconds(16));
                    co_await resume_foreground(dispatcher);
                }
                if (quit)
                    co_return;
                if (!home.ready()) {
                    graphical = false;
                    continue;
                }
                selected = home.selected;
                if (mods_action == 1)
                    selected = static_cast<int>(catalog.list().size()) + 1;
                if ((mods_action == -1 || mods_action == 2) &&
                    home.focus < static_cast<int>(catalog.list().size())) {
                    auto id = catalog.list()[home.focus].id;
                    int target = std::clamp(home.focus + (mods_action == -1 ? -1 : 1), 0,
                                            static_cast<int>(catalog.list().size()) - 1);
                    catalog.move(id, target);
                    home.focus = target;
                }
                mods_action = 0;
            } else {
                auto count = catalog.list().size();
                cursor %= count + 4;
                auto body = cursor < count ? hstring(mod_label(catalog.list()[cursor]))
                                           : label(cursor == count       ? uwp::Text::import_usb
                                                   : cursor == count + 1 ? uwp::Text::save_mods
                                                   : cursor == count + 2 ? uwp::Text::get_mods
                                                                         : uwp::Text::add_link);
                auto action = co_await choose(
                    label(uwp::Text::mods), body,
                    {label(uwp::Text::yes), label(uwp::Text::mods_next), label(uwp::Text::save_mods)});
                if (action == 2)
                    selected = static_cast<int>(count + 1);
                else if (action == 1) {
                    cursor++;
                    continue;
                } else if (cursor < count) {
                    auto entry = catalog.list()[cursor];
                    auto edit =
                        co_await choose(label(uwp::Text::mods), hstring(mod_label(entry)),
                                        {label(entry.enabled ? uwp::Text::mod_off : uwp::Text::mod_on),
                                         label(uwp::Text::mods_up), label(uwp::Text::mods_down)});
                    if (edit == 0)
                        catalog.setEnabled(entry.id, !entry.enabled);
                    else
                        catalog.move(entry.id,
                                     edit == 1 ? (cursor ? cursor - 1 : 0) : std::min(cursor + 1, count - 1));
                    continue;
                } else
                    selected = static_cast<int>(cursor);
            }
            bool error = false;
            try {
                if (selected == static_cast<int>(catalog.list().size()) + 1) {
                    catalog.save();
                    co_return;
                }
                if (selected == static_cast<int>(catalog.list().size()) + 2 ||
                    selected == static_cast<int>(catalog.list().size()) + 3) {
                    catalog.save();
                    co_await online_mods(selected == static_cast<int>(catalog.list().size()) + 3);
                    catalog.load(root, packs::compiled_plan());
                } else if (selected == static_cast<int>(catalog.list().size())) {
                    catalog.save();
                    co_await import_mods();
                    catalog.load(root, packs::compiled_plan());
                } else if (selected >= 0) {
                    auto entry = catalog.list().at(selected);
                    catalog.setEnabled(entry.id, !entry.enabled);
                }
            } catch (std::exception const &) {
                error = true;
            } catch (hresult_error const &) {
                error = true;
            }
            if (error)
                co_await choose(label(uwp::Text::mods), label(uwp::Text::import_failed),
                                {label(uwp::Text::back)});
        }
    }
    IAsyncAction launch(uwp::Candidate candidate) {
        if (!running) {
            if (!boot_attempted) {
                if (!home.ready()) {
                    try {
                        show_home();
                    } catch (std::exception const &) {
                        home.close();
                    }
                }
                home.shown = home.ready();
                home.loading = true;
                loading_since = std::chrono::steady_clock::now();
                uwp::play_drive_spinup(preferences.volume / 100.f);
            }
            if (!boot_attempted) {
                auto dispatcher = host_window.Dispatcher();
                bool available = false, valid_hash = false;
                co_await resume_background();
                try {
                    available = selected_game && uwp::setup::game_folder(selected_game);
                    uwp::setup::Progress progress;
                    if (available)
                        valid_hash =
                            uwp::setup::hash_file(selected_game.GetFileAsync(L"code\\U-King.rpx").get(),
                                                  progress) == uwp::setup::expected_rpx;
                } catch (...) {
                }
                co_await resume_foreground(dispatcher);
                if (!available || !valid_hash) {
                    uwp::stop_drive();
                    home.loading = false;
                    co_await run_setup(false, true);
                    co_return;
                }
            }
            if (boot_attempted) {
                failed = true;
                co_return;
            }
            boot_attempted = true;
            auto dispatcher = host_window.Dispatcher();
            auto user = to_string(local.Path());
            co_await resume_background();
            bool loaded = false;
            try {
                loaded = uwp::prepare_runtime(candidate.path, user);
            } catch (std::exception const &) {
            }
            co_await resume_foreground(dispatcher);
            if (!loaded) {
                uwp::stop_drive();
                home.close();
                failed = true;
                co_return;
            }
            // Complete the press, spin and fade even when preparation finishes quickly.
            while (!quit &&
                   std::chrono::duration<double>(std::chrono::steady_clock::now() - loading_since).count() <
                       3.65) {
                co_await resume_after(std::chrono::milliseconds(16));
                co_await resume_foreground(dispatcher);
            }
            home.close(); // Release the CoreWindow swapchain before the game renderer takes ownership.
            uwp::start_runtime(uwp::core_window(), pixel_width, pixel_height);
            running = true;
        }
        neutral();
        uwp::pause_audio(!visible.load());
    }
    void update_quick_rows() {
        std::lock_guard lock(uwp::quick_menu.mutex);
        auto &q = uwp::quick_menu;
        q.title = setup_label(uwp::Text::quick_menu);
        q.hints = setup_label(uwp::Text::quick_hints);
        q.rows = {setup_label(uwp::Text::resume),
                  setup_label(uwp::Text::launcher_home),
                  setup_label(uwp::Text::render_scale) + L": " +
                      std::to_wstring(render_scale_choice).substr(0, 3) + L"× — " +
                      setup_label(uwp::Text::scale_restart),
                  setup_label(uwp::Text::fps) + L": " + std::to_wstring(preferences.fps),
                  setup_label(uwp::Text::mods),
                  setup_label(uwp::Text::volume) + L": " + std::to_wstring(preferences.volume) + L"%",
                  setup_label(uwp::Text::screenshot),
                  setup_label(uwp::Text::quit_game)};
    }
    fire_and_forget return_home(bool mods) {
        auto lifetime = get_strong();
        try {
            // Guest address-space teardown is unavailable. A restart preserves saves/preferences,
            // and the one-shot destination suppresses auto boot without changing its setting.
            auto values = ApplicationData::Current().LocalSettings().Values();
            values.Insert(L"launcherDestination", box_value(mods ? hstring(L"mods") : hstring(L"home")));
            auto reason = co_await CoreApplication::RequestRestartAsync(L"launcher");
            (void)reason;
            std::lock_guard lock(uwp::quick_menu.mutex);
            uwp::quick_menu.hints = setup_label(uwp::Text::restart_failed);
        } catch (...) {
            std::lock_guard lock(uwp::quick_menu.mutex);
            uwp::quick_menu.hints = setup_label(uwp::Text::restart_failed);
        }
    }
    fire_and_forget capture_screenshot() {
        auto lifetime = get_strong();
        try {
            if (!screenshots)
                screenshots =
                    co_await local.CreateFolderAsync(L"screenshots", CreationCollisionOption::OpenIfExists);
            auto name = L"BotW-" +
                        std::to_wstring(std::chrono::system_clock::now().time_since_epoch().count()) +
                        L".png";
            {
                std::lock_guard lock(uwp::quick_menu.mutex);
                uwp::quick_menu.screenshot_path = std::wstring(screenshots.Path()) + L"\\" + name;
            }
            if (uwp::quick_menu.requested)
                uwp::quick_menu.capture = true;
        } catch (...) {
            uwp::quick_menu.capture_result = -1;
        }
    }
    void poll_quick(const GamepadReading &reading, double now) {
        if (!uwp::quick_menu.paused)
            return;
        auto buttons = uint32_t(reading.Buttons);
        if (reading.LeftThumbstickY > .5)
            buttons |= uwp::up;
        if (reading.LeftThumbstickY < -.5)
            buttons |= uwp::down;
        if (reading.LeftThumbstickX < -.5)
            buttons |= uwp::left;
        if (reading.LeftThumbstickX > .5)
            buttons |= uwp::right;
        auto pressed = quick_repeat.update(buttons, uwp::up | uwp::down | uwp::left | uwp::right, now);
        if ((pressed & uwp::b) || ((pressed & uwp::menu) && !(buttons & uwp::view))) {
            uwp::quick_menu.requested = false;
            return;
        }
        int focus;
        {
            std::lock_guard lock(uwp::quick_menu.mutex);
            auto &q = uwp::quick_menu;
            if (pressed & uwp::up)
                q.focus = (q.focus + 7) % 8;
            if (pressed & uwp::down)
                q.focus = (q.focus + 1) % 8;
            focus = q.focus;
        }
        if (!(pressed & (uwp::a | uwp::left | uwp::right)))
            return;
        bool activate = (pressed & uwp::a) != 0;
        if (focus == 0 && activate)
            uwp::quick_menu.requested = false;
        else if (focus == 1 && activate)
            return_home(false);
        else if (focus == 2) {
            render_scale_choice = uwp::cycle_scale(render_scale_choice, uwp::runtime_max_render_scale());
            ApplicationData::Current().LocalSettings().Values().Insert(L"renderScale",
                                                                       box_value(render_scale_choice));
        } else if (focus == 3) {
            preferences.fps = preferences.fps == 30 ? 60 : 30;
            save_preferences();
        } else if (focus == 4 && activate)
            return_home(true);
        else if (focus == 5) {
            preferences.volume = std::clamp(preferences.volume + (pressed & uwp::left ? -10 : 10), 0, 100);
            save_preferences();
        } else if (focus == 6 && activate) {
            capture_screenshot();
        } else if (focus == 7 && activate) {
            quit = true;
            uwp::quick_menu.requested = false;
            CoreApplication::Exit();
        }
        update_quick_rows();
    }
    IAsyncAction menu(bool allow_auto) {
        if (menu_open)
            co_return;
        menu_open = true;
        menu_visible = true;
        struct Guard {
            bool &value;
            ~Guard() {
                value = false;
                menu_visible = false;
            }
        } guard{menu_open};
        neutral();
        uwp::pause_audio(true);
        if (!running && selected_game && !preferences.setup_complete) {
            co_await run_setup();
            allow_auto = false;
        }
        if (!preferences.setup_complete)
            allow_auto = false;
        for (;;) {
            auto decision = uwp::decide(candidates, preferences, !allow_auto);
            if (selected_game)
                for (auto &candidate : candidates)
                    if (candidate.path == to_string(selected_game.Path()) && candidate.valid()) {
                        decision.game = candidate;
                        decision.select_game = false;
                        break;
                    }
            if (decision.launch && !failed) {
                co_await launch(*decision.game);
                if (running)
                    co_return;
            }
            if (quit)
                co_return;
            allow_auto = false;
            hstring body = failed ? label(boot_attempted && !running ? uwp::Text::restart : uwp::Text::error)
                           : decision.select_game ? label(uwp::Text::missing)
                                                  : hstring{};
            std::vector<hstring> choices{label(running                ? uwp::Text::resume
                                               : decision.select_game ? uwp::Text::select
                                                                      : uwp::Text::play),
                                         label(uwp::Text::settings), label(uwp::Text::mods)};
            // The live game owns its CoreWindow swapchain; resume uses the system overlay.
            int choice = running ? co_await choose(label(uwp::Text::title), body, choices)
                                 : co_await choose_home(body, choices);
            failed = false;
            if (quit)
                co_return;
            if (choice == 0) {
                if (decision.select_game) {
                    home.shown = true;
                    home.flip_time = 0;
                    auto dispatcher = host_window.Dispatcher();
                    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(3500);
                    while (!quit && std::chrono::steady_clock::now() < until) {
                        co_await resume_after(std::chrono::milliseconds(16));
                        co_await resume_foreground(dispatcher);
                    }
                    home.silver_setup = true;
                    home.flip_time = -1;
                    co_await run_setup(false, preferences.setup_complete);
                    co_await scan();
                } else {
                    co_await launch(*decision.game);
                    if (running)
                        co_return;
                }
            } else if (choice == 1)
                co_await configure();
            else
                co_await manage_mods();
        }
    }
    fire_and_forget open_menu(bool startup) {
        if (opening)
            co_return;
        opening = true;
        auto lifetime = get_strong();
        struct OpeningGuard {
            bool &value;
            ~OpeningGuard() { value = false; }
        } guard{opening};
        try {
            if (startup) {
                auto languages = winrt::Windows::Globalization::ApplicationLanguages::Languages();
                pt = languages.Size() && std::wstring_view(languages.GetAt(0)).starts_with(L"pt");
                local = ApplicationData::Current().LocalFolder();
                read_preferences();
                auto values = ApplicationData::Current().LocalSettings().Values();
                render_scale_choice = unbox_value_or<float>(values.TryLookup(L"renderScale"), 0.f);
                uwp::set_runtime_render_scale(render_scale_choice);
                auto dispatcher = host_window.Dispatcher();
                co_await resume_background();
                bool setup_done = local.TryGetItemAsync(L"setup_done.txt").get() != nullptr;
                auto mode = uwp::setup::read_text(local, L"usb_mode.txt");
                auto source = uwp::setup::read_text(local, L"game_source.txt");
                co_await resume_foreground(dispatcher);
                preferences.setup_complete = setup_done;
                saved_usb_mode = mode;
                saved_game_source = source;
                save_preferences();
                co_await local.CreateFolderAsync(L"mods", CreationCollisionOption::OpenIfExists);
                co_await local.CreateFolderAsync(L"art", CreationCollisionOption::OpenIfExists);
                co_await scan();
            }
            auto values = ApplicationData::Current().LocalSettings().Values();
            auto destination = unbox_value_or<hstring>(values.TryLookup(L"launcherDestination"), L"");
            if (!destination.empty()) {
                values.Remove(L"launcherDestination");
                startup = false;
            }
            if (destination == L"mods")
                co_await manage_mods();
            co_await menu(startup);
        } catch (hresult_error const &e) {
            failed = true;
            OutputDebugStringW(e.message().c_str());
        } catch (std::exception const &) {
            uwp::stop_drive();
            failed = true;
            OutputDebugStringW(L"BotW: host operation failed.\n");
        }
    }
    int feedback_focus = -1;
    std::chrono::steady_clock::time_point ui_rumble_until{};
    void ui_feedback() {
        uwp::play_ui_tick();
        if (pad && rumble_enabled) {
            pad.Vibration(GamepadVibration{.08, .08, 0, 0});
            ui_rumble_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
        }
    }
    void poll() {
        uwp::update_launcher_audio();
        if (pad && ui_rumble_until.time_since_epoch().count() &&
            std::chrono::steady_clock::now() >= ui_rumble_until) {
            pad.Vibration(GamepadVibration{});
            ui_rumble_until = {};
        }
        if (home.shown && !home.loading) {
            if ((feedback_focus >= 0 && feedback_focus != home.focus))
                ui_feedback();
            feedback_focus = home.focus;
        } else {
            feedback_focus = -1;
        }
        home.stick_x = home.stick_y = 0;
        auto pads = Gamepad::Gamepads();
        Gamepad current = pads.Size() ? pads.GetAt(0) : nullptr;
        if (current != pad) {
            quick_chord = {};
            neutral();
            pad = current;
        }
        if ((home.shown || transfer_active || text_entry || browser_open) && !home.loading &&
            home.flip_time < 0 && pad && visible) {
            auto reading = pad.GetCurrentReading();
            if (home.focus == 0 && !home.setup_mode && !home.mods_mode) {
                home.stick_x = std::abs(reading.RightThumbstickX) > .12 ? float(reading.RightThumbstickX) : 0;
                home.stick_y = std::abs(reading.RightThumbstickY) > .12 ? float(reading.RightThumbstickY) : 0;
            }
            auto buttons = static_cast<uint32_t>(reading.Buttons);
            if (setup_active || home.store.active) {
                if (reading.LeftThumbstickX < -.5)
                    buttons |= uwp::left;
                if (reading.LeftThumbstickX > .5)
                    buttons |= uwp::right;
                if (reading.LeftThumbstickY < -.5)
                    buttons |= uwp::down;
                if (reading.LeftThumbstickY > .5)
                    buttons |= uwp::up;
            }
            if (home.store.active) {
                if (reading.LeftTrigger > .55)
                    buttons |= 1u << 20;
                if (reading.RightTrigger > .55)
                    buttons |= 1u << 21;
            }
            // Track edges through transfers and dialogs, even while store actions are blocked.
            double store_now =
                std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            auto store_pressed =
                store_repeat.update(buttons, uwp::up | uwp::down | uwp::left | uwp::right, store_now);
            if (home_buttons == ~uint32_t(0)) {
                if (!buttons) {
                    home_buttons = 0;
                    setup_repeat = {};
                }
            } else {
                auto pressed = buttons & ~home_buttons;
                if (pressed & (uwp::a | uwp::b))
                    ui_feedback();
                if (setup_active) {
                    double now =
                        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
                            .count();
                    pressed = setup_repeat.update(buttons, uwp::up | uwp::down | uwp::left | uwp::right, now);
                    if ((pressed & uwp::x) && setup_detect && !setup_busy) {
                        accepted_forms = !accepted_forms;
                        setup_action = -5;
                    }
                    if (pressed & uwp::b) {
                        if (setup_busy)
                            setup_progress.cancel = true;
                        else
                            setup_action = -2;
                    }
                    if (!setup_busy) {
                        if (pressed & uwp::left) {
                            if (setup_guide)
                                setup_action = -3;
                            else
                                home.navigate(3);
                        }
                        if (pressed & uwp::right) {
                            if (setup_guide)
                                setup_action = -4;
                            else
                                home.navigate(1);
                        }
                        if (pressed & uwp::up)
                            home.navigate(0);
                        if (pressed & uwp::down)
                            home.navigate(2);
                        if ((pressed & uwp::a) && home.enabled(size_t(home.focus)))
                            home.selected = home.focus;
                    }
                    home_buttons = buttons;
                    return;
                }
                if (transfer_active) {
                    if (pressed & uwp::b)
                        transfer.cancel();
                    home_buttons = buttons;
                    return;
                }
                if (home.store.active)
                    pressed = store_pressed;
                if (pressed & uwp::up)
                    home.navigate(0);
                if (pressed & uwp::right)
                    home.navigate(1);
                if (pressed & uwp::down)
                    home.navigate(2);
                if (pressed & uwp::left)
                    home.navigate(3);
                if (browser_open) {
                    if (pressed & uwp::b)
                        browser_action = 1;
                    if (!text_entry && (pressed & uwp::lb))
                        browser_action = -1;
                    if (!text_entry && (pressed & uwp::rb))
                        browser_action = 2;
                } else if (mods_open) {
                    if (pressed & uwp::b)
                        mods_action = 1;
                    if (pressed & uwp::x)
                        mods_action = home.store.active ? 2 : -1;
                    if (pressed & uwp::y)
                        mods_action = home.store.active ? 3 : 2;
                    if (pressed & uwp::lb)
                        mods_action = home.store.active ? 4 : -1;
                    if (pressed & uwp::rb)
                        mods_action = home.store.active ? 5 : 2;
                    if (pressed & (1u << 20))
                        mods_action = 6;
                    if (pressed & (1u << 21))
                        mods_action = 7;
                }
                if (!mods_open && !browser_open && (pressed & uwp::menu))
                    home.selected = 1;
                if (pressed & uwp::a)
                    home.selected = home.focus;
                home_buttons = buttons;
            }
        }
        if (quick_was_open && !uwp::quick_menu.requested && !uwp::quick_menu.paused) {
            quick_was_open = false;
            menu_visible = false;
            neutral();
            uwp::pause_audio(!visible.load());
        }
        if (!running || menu_open || !visible)
            return;
        GamepadReading r{};
        double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (uwp::quick_menu.requested) {
            // Sample all connected pads and a physical keyboard, even after the opening pad leaves.
            uint32_t buttons = 0;
            for (auto other : pads) {
                auto reading = other.GetCurrentReading();
                buttons |= uint32_t(reading.Buttons);
                if (reading.LeftThumbstickY > .5)
                    buttons |= uwp::up;
                if (reading.LeftThumbstickY < -.5)
                    buttons |= uwp::down;
                if (reading.LeftThumbstickX < -.5)
                    buttons |= uwp::left;
                if (reading.LeftThumbstickX > .5)
                    buttons |= uwp::right;
            }
            bool keyboard = winrt::Windows::Devices::Input::KeyboardCapabilities().KeyboardPresent() != 0;
            if (!pads.Size() && !keyboard) {
                uwp::quick_menu.requested = false;
                return;
            }
            if (keyboard) {
                using winrt::Windows::System::VirtualKey;
                for (auto [key, button] : {std::pair{VirtualKey::Up, uwp::up},
                                           {VirtualKey::Down, uwp::down},
                                           {VirtualKey::Left, uwp::left},
                                           {VirtualKey::Right, uwp::right},
                                           {VirtualKey::Enter, uwp::a},
                                           {VirtualKey::Space, uwp::a},
                                           {VirtualKey::Escape, uwp::b}})
                    if ((host_window.GetKeyState(key) & CoreVirtualKeyStates::Down) !=
                        CoreVirtualKeyStates::None)
                        buttons |= button;
            }
            r.Buttons = GamepadButtons(buttons);
            poll_quick(r, seconds);
            int capture = uwp::quick_menu.capture_result.exchange(0);
            if (capture) {
                std::lock_guard lock(uwp::quick_menu.mutex);
                uwp::quick_menu.hints =
                    setup_label(capture > 0 ? uwp::Text::screenshot_saved : uwp::Text::screenshot_failed);
            }
            return;
        }
        if (quick_was_open || !pad)
            return;
        r = pad.GetCurrentReading();
        if (quick_chord.update(uint32_t(r.Buttons), seconds)) {
            neutral();
            menu_visible = true;
            quick_was_open = true;
            uwp::pause_audio(true);
            if (render_scale_choice <= 0)
                render_scale_choice = uwp::runtime_render_scale();
            {
                std::lock_guard lock(uwp::quick_menu.mutex);
                uwp::quick_menu.focus = 0;
                uwp::quick_menu.reduced_motion =
                    !winrt::Windows::UI::ViewManagement::UISettings().AnimationsEnabled();
            }
            update_quick_rows();
            quick_repeat = {};
            quick_repeat.held = uint32_t(r.Buttons);
            uwp::quick_menu.requested = true;
            return;
        }
        auto guest_buttons = quick_chord.guest_buttons;
        bool replay_pair = (guest_buttons & (uwp::view | uwp::menu)) == (uwp::view | uwp::menu);
        // This host already arbitrates the chord. Replay an aborted pair without
        // re-triggering Controller's immediate return-to-menu shortcut.
        auto mapped = controller.update(
            {replay_pair ? guest_buttons & ~(uwp::view | uwp::menu) : guest_buttons, r.LeftThumbstickX,
             r.LeftThumbstickY, r.RightThumbstickX, r.RightThumbstickY, r.LeftTrigger, r.RightTrigger},
            layout);
        if (replay_pair)
            mapped.pad.buttons |= input::BTN_PLUS | input::BTN_MINUS;
        input::set_gamepad(mapped.pad);
        if (mapped.toggle_gamepad_screen)
            drc = !drc.load();
        auto now = std::chrono::steady_clock::now();
        if (now >= next_rumble) {
            next_rumble = now + std::chrono::microseconds(16667);
            double motor = input::rumble_step() && rumble_enabled ? 0.375 : 0;
            pad.Vibration(GamepadVibration{motor, motor, 0, 0});
        }
    }
    void Run() {
        active(true);
        open_menu(true);
        winrt::Windows::UI::ViewManagement::UISettings ui_settings;
        while (!quit) {
            host_window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            if (failed && local && !opening && visible)
                open_menu(false);
            try {
                poll();
                if (home.shown && visible && !suspended) {
                    if (setup_busy) {
                        auto total = setup_progress.total.load();
                        home.progress =
                            total ? float(double(setup_progress.done.load()) / double(total)) : 0.f;
                    }
                    bool reduced = !ui_settings.AnimationsEnabled();
                    home.draw(std::chrono::duration<double>(std::chrono::steady_clock::now() - loading_since)
                                  .count(),
                              reduced);
                }
            } catch (hresult_error const &) {
                neutral();
                pad = nullptr;
            } catch (std::exception const &) {
                home.close();
                failed = true;
            }
            if (!home.shown || !visible)
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }
};
static_assert(static_cast<uint32_t>(GamepadButtons::Menu) == uwp::menu);
static_assert(static_cast<uint32_t>(GamepadButtons::View) == uwp::view);
static_assert(static_cast<uint32_t>(GamepadButtons::RightThumbstick) == uwp::rs);
} // namespace
namespace uwp {
void *core_window() { return host_window ? get_abi(host_window) : nullptr; }
bool gamepad_screen_requested() { return drc; }
bool presentation_allowed() { return visible; }
void present_quick_menu(void *device, void *queue, void *chain) {
    try {
        draw_quick_menu(device, queue, chain);
    } catch (const std::exception &) {
        quick_menu.requested = false;
        quick_menu.paused = false;
        OutputDebugStringW(L"BotW: quick menu graphics failed.\n");
    }
}
void set_visibility_callback(VisibilityCallback callback) { visibility_callback = callback; }
} // namespace uwp
namespace window {
void create() {}
void run() {} // CoreApplication owns the pump; do not call os::host_main_loop on UWP.
void drawable_size(int *w, int *h) {
    *w = pixel_width;
    *h = pixel_height;
}
void display_pixel_size(int *w, int *h) { drawable_size(w, h); }
void set_gyro(bool) {} // Xbox controllers have no motion sensor.
void set_rumble(bool on) { rumble_enabled = on; }
void set_layout(int value) { layout = value == 1 ? 1 : 0; }
void set_script(const std::string &) {}
void on_frame(uint64_t) {}
bool quit_requested() { return quit; }
SDL_Window *g_win_ptr() { return nullptr; }
} // namespace window
void botw_window_phys_size(int &w, int &h) { window::drawable_size(&w, &h); }
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Multi-threaded like the C++/WinRT CoreApplication template: Run() sets up the view's own
    // ASTA, and a single-threaded apartment here kills the process before activation on Xbox.
    init_apartment();
    // Log and crash filter first, so a failure anywhere in the launcher or the game leaves a trace
    // in LocalState (the console shows no dialog).
    auto local = winrt::to_string(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
    log_set_file((local + "\\runtime.log").c_str());
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS *e) -> LONG {
        auto pc = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
        auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        LOG("FATAL exception 0x%08lx at exe+0x%llx (pc 0x%llx)", e->ExceptionRecord->ExceptionCode,
            (unsigned long long)(pc - exe), (unsigned long long)pc);
        return EXCEPTION_CONTINUE_SEARCH;
    });
    LOG("BotW Xbox Recomp starting");
    CoreApplication::Run(make<App>());
    return 0;
}
#endif
