// Launch-time renderer selection. Static PPC patches and timing remain
// installed.
#pragma once
#include "packs.h"

namespace packs {
enum class Visuals { Remaster = 0, Original = 1 };
constexpr Visuals parse_visuals(int value) {
  return value == 1 ? Visuals::Original : Visuals::Remaster;
}
constexpr Visuals next_visuals(Visuals value) {
  return value == Visuals::Original ? Visuals::Remaster : Visuals::Original;
}
// Call after mod filtering, once before guest startup. The returned plan must
// outlive the renderer; changing preferences never mutates an installed plan.
BuildPlan select_visuals(BuildPlan plan, Visuals visuals);
} // namespace packs
