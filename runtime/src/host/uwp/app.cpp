// SPDX-License-Identifier: MIT
#if defined(BOTW_PLATFORM_UWP)
#include "../../host/window.h"
#include "../../latte/latte.h"
#include "../../settings.h"
#include "audio.h"
#include "controller.h"
#include "host.h"
#include "launcher.h"
#include "runtime.h"
#include "strings.h"
#include <algorithm>
#include <atomic>
#include <chrono>
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
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Popups.h>

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

struct App final : implements<App, IFrameworkViewSource, IFrameworkView> {
    bool pt = false, menu_open = false, running = false, boot_attempted = false;
    bool failed = false, foreground = true, suspended = false, opening = false;
    uwp::Preferences preferences;
    uwp::Controller controller;
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
        });
        dpi = Windows::Graphics::Display::DisplayInformation::GetForCurrentView().DpiChanged(
            [this](auto const &, auto const &) {
                update_size();
                if (running)
                    latte::resize_window(pixel_width, pixel_height);
            });
        update_size();
    }
    void Load(hstring const &) {}
    void Uninitialize() {
        neutral();
        active(false);
        uwp::shutdown_audio();
        CoreApplication::Suspending(suspending);
        CoreApplication::Resuming(resuming);
        CoreApplication::MainView().Activated(activating);
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
        co_await resume_background();
        std::vector<uwp::Candidate> found;
        try {
            for (auto path : {L"game", L""}) {
                auto folder = *path ? child(local, path) : local;
                if (folder)
                    found.push_back(inspect(folder, uwp::Source::local));
            }
            auto access = AccessCache::StorageApplicationPermissions::FutureAccessList();
            if (access.ContainsItem(L"game")) {
                try {
                    found.push_back(inspect(access.GetFolderAsync(L"game").get(), uwp::Source::remembered));
                } catch (
                    hresult_error const &) { /* Removed drive or expired grant: continue auto detection. */
                }
            }
            auto drives = KnownFolders::RemovableDevices().GetFoldersAsync().get();
            for (auto const &drive : drives) {
                for (auto path : {L"game", L"BotW", L"BotWRecomp\\game", L""}) {
                    auto folder = *path ? child(drive, path) : drive;
                    if (folder)
                        found.push_back(inspect(folder, uwp::Source::removable));
                }
            }
        } catch (hresult_error const &) { /* LocalState remains usable if removable access is denied. */
        }
        co_await resume_foreground(dispatcher);
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
    IAsyncAction launch(uwp::Candidate candidate) {
        if (!running) {
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
                failed = true;
                co_return;
            }
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
            allow_auto = false;
            hstring body = failed ? label(boot_attempted && !running ? uwp::Text::restart : uwp::Text::error)
                           : decision.select_game ? label(uwp::Text::missing)
                                                  : label(uwp::Text::controls);
            int choice = co_await choose(label(uwp::Text::title), body,
                                         {label(running                ? uwp::Text::resume
                                                : decision.select_game ? uwp::Text::select
                                                                       : uwp::Text::play),
                                          label(uwp::Text::settings), label(uwp::Text::mods)});
            failed = false;
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
            else {
                auto mods = co_await local.CreateFolderAsync(L"mods", CreationCollisionOption::OpenIfExists);
                co_await choose(
                    label(uwp::Text::mods),
                    hstring(std::wstring(uwp::text(uwp::Text::mods_help, pt)) + L"\n" + mods.Path().c_str()),
                    {label(uwp::Text::back)});
            }
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
        while (!quit) {
            host_window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            if (failed && local && !opening && visible)
                open_menu(false);
            try {
                poll();
            } catch (hresult_error const &) {
                neutral();
                pad = nullptr;
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
