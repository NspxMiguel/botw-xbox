// SPDX-License-Identifier: MIT
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace uwp {
enum class Source { local, remembered, removable };
struct Candidate {
    std::string path;
    Source source = Source::local;
    bool rpx = false, content = false, meta = false;
    bool valid() const { return !path.empty() && rpx && content && meta; }
};
struct Preferences {
    bool setup_complete = false;
    bool auto_launch = false;
    int fps = 60;
    int volume = 100;
    int layout = 0;
};
struct LaunchDecision {
    std::optional<Candidate> game;
    bool select_game = true;
    bool ask_auto_launch = false;
    bool launch = false;
};
// Stable priority: LocalState, remembered picker grant, removable drive. Revalidate on every launch.
LaunchDecision decide(const std::vector<Candidate> &candidates, const Preferences &preferences,
                      bool returned_to_menu = false);
} // namespace uwp
