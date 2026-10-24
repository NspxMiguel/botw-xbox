// SPDX-License-Identifier: MIT
#pragma once
#include "../../fsr3_policy.h"

namespace uwp {
enum class ConsoleModel { Desktop, SeriesS, SeriesX, UnknownXbox };
constexpr const char *console_model_name(ConsoleModel model) {
    switch (model) {
    case ConsoleModel::SeriesS:
        return "Xbox Series S";
    case ConsoleModel::SeriesX:
        return "Xbox Series X";
    case ConsoleModel::UnknownXbox:
        return "unidentified Xbox (conservative Series S policy)";
    default:
        return "desktop";
    }
}
// Gaming-device information may report an emulated/older console. Never grant
// desktop settings to an unidentified Xbox. Simulation changes policy, not identity.
constexpr fsr3::Console select_console_profile(ConsoleModel model, bool simulate_s) {
    if (simulate_s || model == ConsoleModel::SeriesS || model == ConsoleModel::UnknownXbox)
        return fsr3::Console::SeriesS;
    return model == ConsoleModel::SeriesX ? fsr3::Console::SeriesX : fsr3::Console::Desktop;
}
} // namespace uwp
