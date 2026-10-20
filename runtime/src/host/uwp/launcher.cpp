// SPDX-License-Identifier: MIT
#include "launcher.h"
#include <algorithm>

namespace uwp {
LaunchDecision decide(const std::vector<Candidate> &candidates, const Preferences &p, bool returned) {
    LaunchDecision result;
    for (auto source : {Source::local, Source::remembered, Source::removable}) {
        auto it = std::find_if(candidates.begin(), candidates.end(),
                               [source](const Candidate &c) { return c.source == source && c.valid(); });
        if (it != candidates.end()) {
            result.game = *it;
            break;
        }
    }
    result.select_game = !result.game;
    result.ask_auto_launch = result.game.has_value() && !p.setup_complete;
    result.launch = result.game.has_value() && p.setup_complete && p.auto_launch && !returned;
    return result;
}
} // namespace uwp
