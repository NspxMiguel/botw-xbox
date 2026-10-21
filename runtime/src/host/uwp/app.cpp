// SPDX-License-Identifier: MIT
#if defined(BOTW_PLATFORM_UWP)
#include "../../host/window.h"
#include "../../latte/latte.h"
#include "../../mods/catalog.h"
#include "../../mods/gamebanana.h"
#include "../../mods/zip_install.h"
#include "../../settings.h"
#include "art_png.h"
#include "art_view.h"
#include "audio.h"
#include "controller.h"
#include "host.h"
#include "launcher.h"
#include "mod_network.h"
#include "runtime.h"
#include "strings.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <thread>
#include <vector>
#include <windows.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
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

struct App : implements<App, IFrameworkViewSource, IFrameworkView> {
    bool pt = false, menu_open = false, running = false, boot_attempted = false;
    bool failed = false, foreground = true, suspended = false, opening = false;
    uwp::Preferences preferences;
    uwp::Controller controller;
    uwp::GameArt art;
    uwp::ArtView home;
    bool mods_open = false, browser_open = false, text_entry = false, transfer_active = false;
    int browser_action = 0;
    std::wstring input_text;
    uwp::Transfer transfer;
    winrt::Windows::UI::Text::Core::CoreTextEditContext edit_context{nullptr};
    winrt::Windows::UI::Text::Core::CoreTextRange text_selection{};
    event_token pointer_moved{}, character_received{};
    int mods_action = 0;
    uint32_t home_buttons = ~uint32_t(0);
    std::chrono::steady_clock::time_point loading_since{};
    event_token key_down{}, pointer_pressed{};
    StorageFolder local{nullptr};
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
            if (running)
                latte::resize_window(pixel_width, pixel_height);
            else
                resize_home();
        });
        dpi = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().DpiChanged(
            [this](auto const &, auto const &) {
                update_size();
                if (running)
                    latte::resize_window(pixel_width, pixel_height);
                else
                    resize_home();
            });
        key_down = window.KeyDown([this](auto const &, KeyEventArgs const &e) {
            if ((!home.shown && !transfer_active && !text_entry && !browser_open) || home.loading)
                return;
            using winrt::Windows::System::VirtualKey;
            auto key = e.VirtualKey();
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
                    mods_action = -1;
                if (key == VirtualKey::Y)
                    mods_action = 2;
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
            if (!home.shown || home.loading)
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
            if (hit >= 0)
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
    }
    void save_preferences() {
        auto values = ApplicationData::Current().LocalSettings().Values();
        values.Insert(L"setupComplete", box_value(preferences.setup_complete));
        values.Insert(L"autoLaunch", box_value(preferences.auto_launch));
        values.Insert(L"fps", box_value(preferences.fps));
        values.Insert(L"volume", box_value(preferences.volume));
        values.Insert(L"layout", box_value(preferences.layout));
        Settings s = settings::get();
        s.fps = preferences.fps;
        s.volume = preferences.volume;
        s.layout = preferences.layout;
        s.gyro = false;
        settings::set(s, false);
    }
    IAsyncAction scan() {
        auto lifetime = get_strong();
        auto dispatcher = host_window.Dispatcher();
        auto saved_preferences = preferences;
        co_await resume_background();
        std::vector<uwp::Candidate> found;
        std::vector<StorageFolder> folders;
        auto add = [&](StorageFolder const &folder, uwp::Source source) {
            found.push_back(inspect(folder, source));
            folders.push_back(folder);
        };
        try {
            for (auto path : {L"game", L""}) {
                auto folder = *path ? child(local, path) : local;
                if (folder)
                    add(folder, uwp::Source::local);
            }
            auto access = AccessCache::StorageApplicationPermissions::FutureAccessList();
            if (access.ContainsItem(L"game")) {
                try {
                    add(access.GetFolderAsync(L"game").get(), uwp::Source::remembered);
                } catch (
                    hresult_error const &) { /* Removed drive or expired grant: continue auto detection. */
                }
            }
            auto drives = KnownFolders::RemovableDevices().GetFoldersAsync().get();
            for (auto const &drive : drives) {
                for (auto path : {L"game", L"BotW", L"BotWRecomp\\game", L""}) {
                    auto folder = *path ? child(drive, path) : drive;
                    if (folder)
                        add(folder, uwp::Source::removable);
                }
            }
        } catch (hresult_error const &) { /* LocalState remains usable if removable access is denied. */
        }
        uwp::GameArt loaded_art;
        auto decision = uwp::decide(found, saved_preferences);
        if (decision.game) {
            for (size_t i = 0; i < found.size(); ++i) {
                if (found[i].path != decision.game->path)
                    continue;
                auto meta = child(folders[i], L"meta");
                loaded_art.tv = read_art(meta, L"bootTvTex.tga");
                loaded_art.icon = read_art(meta, L"iconTex.tga");
                loaded_art.drc = read_art(meta, L"bootDrcTex.tga");
                break;
            }
        }
        if (decision.game) {
            auto custom = child(local, L"art");
            auto custom_disc = read_art(custom, L"disc.png", true);
            if (!custom_disc)
                custom_disc = read_art(custom, L"cover.png", true);
            if (custom_disc)
                loaded_art.icon = std::move(custom_disc);
        }
        co_await resume_foreground(dispatcher);
        art = std::move(loaded_art);
        candidates = std::move(found);
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
            home.shown = shown;
            home.loading = loading;
            home.focus = focus;
        } catch (std::exception const &) {
            home.close();
        }
    }
    void show_home() {
        home.open(static_cast<::IUnknown *>(uwp::core_window()), pixel_width, pixel_height, art);
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
    IAsyncAction select_folder() {
        Pickers::FolderPicker picker;
        picker.SuggestedStartLocation(Pickers::PickerLocationId::ComputerFolder);
        picker.FileTypeFilter().Append(L"*");
        StorageFolder folder{nullptr};
        try {
            folder = co_await picker.PickSingleFolderAsync();
        } catch (hresult_error const &) {
            failed = true;
            co_return;
        }
        if (!folder)
            co_return;
        auto dispatcher = host_window.Dispatcher();
        co_await resume_background();
        uwp::Candidate candidate;
        try {
            candidate = inspect(folder, uwp::Source::remembered);
        } catch (hresult_error const &) {
        }
        co_await resume_foreground(dispatcher);
        if (candidate.valid()) {
            AccessCache::StorageApplicationPermissions::FutureAccessList().AddOrReplace(L"game", folder);
            candidates.insert(candidates.begin(), std::move(candidate));
        } else
            failed = true;
    }
    IAsyncAction configure() {
        for (;;) {
            auto choice =
                co_await choose(label(uwp::Text::settings), label(uwp::Text::controls),
                                {label(uwp::Text::automatic), label(uwp::Text::fps), label(uwp::Text::back)});
            if (choice == 2)
                break;
            if (choice == 0) {
                preferences.auto_launch =
                    (co_await choose(label(uwp::Text::automatic), L"",
                                     {label(uwp::Text::yes), label(uwp::Text::no)})) == 0;
            } else {
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
        edit_context.LayoutRequested([](auto const &, CoreTextLayoutRequestedEventArgs const &args) {
            auto window = host_window.Bounds();
            auto bounds = args.Request().LayoutBounds();
            Rect field{window.X + 64 * window.Width / 1280, window.Y + 160 * window.Height / 720,
                       1152 * window.Width / 1280, 56 * window.Height / 720};
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
        transfer.pending = nullptr;
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
                    // ponytail: empty = cancelled, keeps the current search; a "clear search" row if players ask.
                    if (auto typed = to_string(co_await enter_text(uwp::Text::enter_search)); !typed.empty()) {
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
        transfer.pending = nullptr;
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
    IAsyncAction manage_mods() {
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
            if (boot_attempted) {
                failed = true;
                co_return;
            }
            boot_attempted = true;
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
                home.close();
                failed = true;
                co_return;
            }
            home.close(); // Release the CoreWindow swapchain before the game renderer takes ownership.
            uwp::start_runtime(uwp::core_window(), pixel_width, pixel_height);
            running = true;
        }
        neutral();
        uwp::pause_audio(!visible.load());
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
        for (;;) {
            auto decision = uwp::decide(candidates, preferences, !allow_auto);
            if (decision.ask_auto_launch) {
                preferences.auto_launch =
                    (co_await choose(label(uwp::Text::title), label(uwp::Text::auto_question),
                                     {label(uwp::Text::yes), label(uwp::Text::no)})) == 0;
                preferences.setup_complete = true;
                save_preferences();
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
                    co_await select_folder();
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
                save_preferences();
                co_await local.CreateFolderAsync(L"mods", CreationCollisionOption::OpenIfExists);
                co_await local.CreateFolderAsync(L"art", CreationCollisionOption::OpenIfExists);
                co_await scan();
            }
            co_await menu(startup);
        } catch (hresult_error const &e) {
            failed = true;
            OutputDebugStringW(e.message().c_str());
        } catch (std::exception const &) {
            failed = true;
            OutputDebugStringW(L"BotW: host operation failed.\n");
        }
    }
    void poll() {
        auto pads = Gamepad::Gamepads();
        Gamepad current = pads.Size() ? pads.GetAt(0) : nullptr;
        if (current != pad) {
            neutral();
            pad = current;
        }
        if ((home.shown || transfer_active || text_entry || browser_open) && !home.loading && pad &&
            visible) {
            auto buttons = static_cast<uint32_t>(pad.GetCurrentReading().Buttons);
            if (home_buttons == ~uint32_t(0)) {
                if (!buttons)
                    home_buttons = 0;
            } else {
                auto pressed = buttons & ~home_buttons;
                if (transfer_active) {
                    if (pressed & uwp::b)
                        transfer.cancel();
                    home_buttons = buttons;
                    return;
                }
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
                    if (pressed & (uwp::x | uwp::lb))
                        mods_action = -1;
                    if (pressed & (uwp::y | uwp::rb))
                        mods_action = 2;
                }
                if (!mods_open && !browser_open && (pressed & uwp::menu))
                    home.selected = 1;
                if (pressed & uwp::a)
                    home.selected = home.focus;
                home_buttons = buttons;
            }
        }
        if (!pad || !running || menu_open || !visible)
            return;
        auto r = pad.GetCurrentReading();
        auto mapped =
            controller.update({static_cast<uint32_t>(r.Buttons), r.LeftThumbstickX, r.LeftThumbstickY,
                               r.RightThumbstickX, r.RightThumbstickY, r.LeftTrigger, r.RightTrigger},
                              layout);
        input::set_gamepad(mapped.pad);
        if (mapped.return_to_menu) {
            open_menu(false);
            return;
        }
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
    init_apartment(apartment_type::single_threaded);
    CoreApplication::Run(make<App>());
    return 0;
}
#endif
