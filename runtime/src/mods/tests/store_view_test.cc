// SPDX-License-Identifier: MIT
#include "../../host/uwp/request_deadline.h"
#include "../../host/uwp/setup.h"
#include "../../host/uwp/store_view.h"
#include <atomic>
#include <cassert>
#include <future>
int main() {
    using namespace std::chrono_literals;
    std::promise<void> cancelled;
    auto done = cancelled.get_future();
    {
        uwp::RequestDeadline deadline(20ms, [&] { cancelled.set_value(); });
        assert(done.wait_for(2s) == std::future_status::ready);
    }
    bool unexpected = false;
    auto before = std::chrono::steady_clock::now();
    {
        uwp::RequestDeadline deadline(20s, [&] { unexpected = true; });
    }
    assert(!unexpected && std::chrono::steady_clock::now() - before < 1s);
    struct FakeTransfer {
        std::atomic<bool> cancelled{false};
        void cancel() { cancelled = true; }
    };
    uwp::RequestGroup<FakeTransfer> group;
    auto one = group.start(), two = group.start(), three = group.start(), four = group.start();
    auto fifth = std::async(std::launch::async, [&] { return group.start(); });
    assert(fifth.wait_for(20ms) == std::future_status::timeout);
    one.reset();
    assert(fifth.wait_for(1s) == std::future_status::ready);
    auto next = fifth.get();
    group.cancel();
    assert(two->cancelled && three->cancelled && four->cancelled && next->cancelled);
    assert(group.start()->cancelled);
    assert(group.is_closed());
    // The poller samples the repeat state even while a transfer/dialog blocks actions.
    uwp::setup::Repeat repeat;
    constexpr uint32_t accept = 1, back = 2, direction = 4;
    assert(repeat.update(accept, direction, 0) == accept);
    (void)repeat.update(accept | back, direction, 1); // Transfer or confirm dialog.
    assert(repeat.update(accept | back, direction, 2) == 0);
    assert(repeat.update(0, direction, 3) == 0);
    assert(repeat.update(accept, direction, 4) == accept);
    uwp::StoreView view;
    view.cards.resize(20);
    int focus = 0;
    view.navigate(3, focus);
    assert(focus == -20);
    view.navigate(1, focus);
    assert(focus == 0);
    view.navigate(0, focus);
    assert(focus == -40);
    view.navigate(0, focus);
    assert(focus == -50);
    view.navigate(0, focus);
    assert(focus == -60);
    view.navigate(1, focus);
    assert(focus == -61);
    view.navigate(1, focus);
    assert(focus == -62);
    view.navigate(1, focus);
    assert(focus == -60);
    focus = 0;
    for (int i = 0; i < 20; ++i)
        view.navigate(2, focus);
    assert(focus == 19);
    assert(view.first(focus) == 18);
    view.detail = mods::store::Card{};
    view.detail->installed_id = "file:test";
    view.detail->order = 1;
    view.detail->total = 3;
    assert(!view.action_enabled(0));
    assert(view.action_enabled(1));
    assert(view.action_enabled(2));
    assert(view.action_enabled(3));
    assert(view.action_enabled(4));
    view.detail->locked = true;
    assert(!view.action_enabled(1));
    view.detail->installed_id = "pack:compiled";
    assert(!view.action_enabled(2));
    view.detail.reset();
    view.cards.clear();
    focus = 0;
    view.navigate(0, focus);
    assert(focus == -20);
    // A featured row reserves its unused columns without adding selectable phantom cards.
    view.cards.resize(12);
    view.cards[0].featured = true;
    assert(view.featured_count() == 1);
    assert(view.slot(0) == 0 && view.slot(1) == 3);
    assert(view.card_at(1) == view.cards.size() && view.card_at(2) == view.cards.size());
    assert(view.card_at(3) == 1 && view.first(4) == 4);
    focus = 0;
    view.navigate(2, focus);
    assert(focus == 1);
    view.navigate(1, focus);
    assert(focus == 2);
    view.navigate(0, focus);
    assert(focus == 0);
    view.navigate(1, focus);
    assert(focus == 1);
    view.navigate(3, focus);
    assert(focus == -20);
    view.cards[1].featured = true;
    view.cards[2].featured = true;
    view.cards[3].featured = true;
    assert(view.featured_count() == 4 && view.slot(4) == 6);
    focus = 4;
    view.navigate(0, focus);
    assert(focus == 3);
    view.query = L"voice";
    assert(view.featured_count() == 0 && view.slot(4) == 4);
    view.query.clear();
    view.tab = 1;
    assert(view.featured_count() == 0);
    assert(uwp::text(uwp::Text::store_featured, false) == L"Featured");
    assert(uwp::text(uwp::Text::store_featured, true) == L"Destaques");
    assert(uwp::text(uwp::Text::translation_offer, true) ==
           L"Instalar a tradução em português do Brasil (mod da comunidade, baixado do GameBanana)?");
    assert(!uwp::text(uwp::Text::translation_offer, false).empty());
}
