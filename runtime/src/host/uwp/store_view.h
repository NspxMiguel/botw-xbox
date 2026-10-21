// SPDX-License-Identifier: MIT
#pragma once
#include "../../mods/store.h"
#include "strings.h"
#include <optional>
namespace uwp {
namespace store_layout {
inline constexpr float rail_x = 48, rail_w = 228, grid_x = 303, grid_y = 238;
inline constexpr float card_w = 299, card_h = 187, gap = 16, image_h = 119;
inline constexpr float hero_x = 48, hero_y = 140, hero_w = 640, hero_h = 310;
inline constexpr int columns = 3, visible = 6;
} // namespace store_layout
struct StoreView {
    bool active = false, busy = false, offline = false, error = false, detail_error = false, pt = false;
    int tab = 0, source = 0, category = 0, image = 0, file = 0;
    std::wstring query, recent, status;
    std::vector<mods::store::Card> cards;
    std::optional<mods::store::Card> detail;
    float progress = -1;
    size_t first(int focus) const { return focus < 0 ? 0 : size_t(focus / 6) * 6; }
    std::wstring label(Text t) const { return std::wstring(text(t, pt)); }
    std::wstring category_label(int i) const { return label(static_cast<Text>(int(Text::store_all) + i)); }
    std::wstring source_label(int i) const {
        return i == 0   ? label(Text::store_all_sources)
               : i == 1 ? L"GameBanana"
               : i == 2 ? L"GitHub packs"
               : i == 3 ? L"Link"
                        : L"USB";
    }
    bool action_enabled(int i) const {
        if (!detail)
            return false;
        auto &c = *detail;
        if (i == 0 && detail_error)
            return true;
        if (i == 0)
            return (c.installed_id.empty() || c.update()) && (!c.downloads.empty() || c.source == "USB");
        if (i == 1)
            return !c.installed_id.empty() && !c.locked;
        if (i == 2)
            return c.installed_id.starts_with("file:");
        if (i == 3)
            return !c.installed_id.empty() && c.order > 0;
        if (i == 4)
            return !c.installed_id.empty() && c.order + 1 < c.total;
        if (i == 5)
            return !c.downloads.empty();
        return i >= 6 && size_t(i - 6) < c.downloads.size();
    }
    // Negative IDs are persistent controls; -1 remains the no-action sentinel.
    void navigate(int direction, int &focus) const {
        if (detail) {
            int n = 6 + int(detail->downloads.size());
            for (int i = 0; i < n; ++i) {
                focus = (std::max(0, focus) + (direction == 0 || direction == 3 ? -1 : 1) + n) % n;
                if (action_enabled(focus))
                    break;
            }
            return;
        }
        if (focus <= -60 && focus >= -62) {
            if (direction == 2)
                focus = -50;
            else
                focus = -60 + (((-focus - 60) + (direction == 3 ? 2 : 1)) % 3) * -1;
            return;
        }
        if (focus == -50) {
            focus = direction == 0 ? -60 : direction == 3 ? -20 : -40;
            return;
        }
        if (focus <= -40 && focus >= -44) {
            if (direction == 0)
                focus = -50;
            else if (direction == 2)
                focus = cards.empty() ? -20 : 0;
            else
                focus = -40 - ((-focus - 40 + (direction == 3 ? 4 : 1)) % 5);
            return;
        }
        if (focus <= -20 && focus >= -28) {
            if (direction == 1)
                focus = cards.empty() ? -40 : 0;
            else if (direction == 0 && focus == -20)
                focus = -40;
            else
                focus = -20 - std::clamp(-focus - 20 + (direction == 0 ? -1 : 1), 0, 8);
            return;
        }
        if (focus == -70) {
            focus = direction == 0 && !cards.empty() ? int(cards.size()) - 1 : -50;
            return;
        }
        if (error && direction == 2 && focus >= 0 && focus + 3 >= int(cards.size())) {
            focus = -70;
            return;
        }
        if (cards.empty()) {
            focus = -20;
            return;
        }
        if (direction == 0 && focus < 3) {
            focus = -40;
            return;
        }
        if (direction == 3 && focus % 3 == 0) {
            focus = -20 - category;
            return;
        }
        focus = std::clamp(focus + (direction == 0   ? -3
                                    : direction == 2 ? 3
                                    : direction == 3 ? -1
                                                     : 1),
                           0, int(cards.size()) - 1);
    }
};
} // namespace uwp
