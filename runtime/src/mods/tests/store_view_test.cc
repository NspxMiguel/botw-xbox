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
}
