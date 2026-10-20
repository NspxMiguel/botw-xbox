#include "logic.h"
#include "platform.h"
namespace platform {
uint64_t guest_timebase() {
  // Establish the origin before sampling now; concurrent first callers cannot
  // underflow.
  static const uint64_t origin = monotonic_ticks();
  static const uint64_t frequency = monotonic_frequency();
  return scale_ticks(monotonic_ticks() - origin, frequency, 62156250);
}
} // namespace platform
