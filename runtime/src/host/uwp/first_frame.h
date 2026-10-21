// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
namespace uwp {
// Renderer publishes only after DXGI accepts a real present; host reads without renderer locks.
inline std::atomic<bool> first_frame_presented{false};
inline void signal_first_frame() { first_frame_presented.store(true, std::memory_order_release); }
} // namespace uwp
