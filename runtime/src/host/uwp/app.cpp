// SPDX-License-Identifier: MIT
#if defined(BOTW_PLATFORM_UWP)
#include "../../host/window.h"
#include "../../latte/latte.h"
#include "../../mods/catalog.h"
#include "../../settings.h"
#include "art_png.h"
#include "art_view.h"
#include "audio.h"
#include "controller.h"
#include "host.h"
#include "launcher.h"
#include "runtime.h"
#include "strings.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
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
#include <winrt/Windows.UI.ViewManagement.h>

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation;
using namespace Windows::Gaming::Input;
using namespace Windows::Storage;
using namespace Windows::UI::Core;
using namespace Windows::UI::Popups;

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
    auto dpi = Windows::Graphics::Display::DisplayInformation::GetForCurrentView().LogicalDpi();
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

struct App final : implements<App, IFrameworkViewSource, IFrameworkView> {
    bool pt = false, menu_open = false, running = false, boot_attempted = false;
    bool failed = false, foreground = true, suspended = false, opening = false;
    uwp::Preferences preferences;
    uwp::Controller controller;
    uwp::GameArt art;
    uwp::ArtView home;
    bool mods_open = false;
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
        dpi = Windows::Graphics::Display::DisplayInformation::GetForCurrentView().DpiChanged(
            [this](auto const &, auto const &) {
                update_size();
                if (running)
                    latte::resize_window(pixel_width, pixel_height);
                else
                    resize_home();
            });
        key_down = window.KeyDown([this](auto const &, KeyEventArgs const &e) {
            if (!home.shown || home.loading)
                return;
            using Windows::System::VirtualKey;
            auto key = e.VirtualKey();
            if (mods_open) {
                if (key == VirtualKey::Escape)
                    mods_action = 1;
                if (key == VirtualKey::X)
                    mods_action = -1;
                if (key == VirtualKey::Y)
                    mods_action = 2;
            }
            if (key == VirtualKey::Left || key == VirtualKey::Up)
                home.step(-1);
            if (key == VirtualKey::Right || key == VirtualKey::Down || key == VirtualKey::Tab)
                home.step(1);
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
        host_window.Closed(closed);
        host_window.VisibilityChanged(visibility);
        host_window.Activated(activated);
        host_window.SizeChanged(size);
        Windows::Graphics::Display::DisplayInformation::GetForCurrentView().DpiChanged(dpi);
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
        auto buttons = home.buttons;
        auto rows = home.rows;
        bool mods_mode = home.mods_mode;
        bool shown = home.shown, loading = home.loading;
        int focus = home.focus;
        try {
            home.open(static_cast<IUnknown *>(uwp::core_window()), pixel_width, pixel_height, art);
            home.heading = std::move(heading);
            home.detail = std::move(detail);
            home.loading_text = std::move(loading_text);
            home.buttons = std::move(buttons);
            home.rows = std::move(rows);
            home.mods_mode = mods_mode;
            home.shown = shown;
            home.loading = loading;
            home.focus = focus;
        } catch (std::exception const &) {
            home.close();
        }
    }
    void show_home() {
        home.open(static_cast<IUnknown *>(uwp::core_window()), pixel_width, pixel_height, art);
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
        for (size_t i = 0; i < 3; ++i)
            home.buttons[i] = choices[i].c_str();
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
    std::wstring mod_label(const mods::ModInfo &entry) const {
        auto kind = entry.kind == mods::Kind::translation ? uwp::Text::translation
                    : entry.kind == mods::Kind::dubbing   ? uwp::Text::dubbing
                    : entry.kind == mods::Kind::graphics  ? uwp::Text::graphics
                                                          : uwp::Text::files;
        return std::wstring(to_hstring(entry.name)) + L" · " + label(kind).c_str() + L" · " +
               label(entry.enabled ? uwp::Text::mod_on : uwp::Text::mod_off).c_str() +
               (entry.locked ? L" · " + std::wstring(label(uwp::Text::rebuild)) : L"");
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
                for (auto const &entry : catalog.list())
                    home.rows.push_back(mod_label(entry));
                home.rows.push_back(label(uwp::Text::import_usb).c_str());
                home.rows.push_back(label(uwp::Text::save_mods).c_str());
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
                cursor %= count + 2;
                auto body = cursor < count
                                ? hstring(mod_label(catalog.list()[cursor]))
                                : label(cursor == count ? uwp::Text::import_usb : uwp::Text::save_mods);
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
                if (selected == static_cast<int>(catalog.list().size())) {
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
                                                  : label(uwp::Text::controls);
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
                auto languages = Windows::Globalization::ApplicationLanguages::Languages();
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
        if (home.shown && !home.loading && pad && visible) {
            auto buttons = static_cast<uint32_t>(pad.GetCurrentReading().Buttons);
            if (home_buttons == ~uint32_t(0)) {
                if (!buttons)
                    home_buttons = 0;
            } else {
                auto pressed = buttons & ~home_buttons;
                if (pressed & (uwp::left | uwp::up))
                    home.step(-1);
                if (pressed & (uwp::right | uwp::down))
                    home.step(1);
                if (mods_open) {
                    if (pressed & uwp::b)
                        mods_action = 1;
                    if (pressed & (uwp::x | uwp::lb))
                        mods_action = -1;
                    if (pressed & (uwp::y | uwp::rb))
                        mods_action = 2;
                }
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
        Windows::UI::ViewManagement::UISettings ui_settings;
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
