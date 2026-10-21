// SPDX-License-Identifier: MIT
#pragma once
namespace uwp {
void play_drive_spinup(float volume);
void update_launcher_audio();
void stop_drive();
void play_ui_tick();
void pause_audio(bool paused);
void shutdown_audio();
} // namespace uwp
