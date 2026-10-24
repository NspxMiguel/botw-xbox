// SPDX-License-Identifier: MIT
#include "../../../../cemu/Cafe/HW/Latte/Renderer/D3D12/D3D12PackHooks.h"
#include "../../../mods/catalog.h"
#include "../launcher.h"
#include "../strings.h"
#include <cassert>
#include <filesystem>

namespace {
unsigned compilations = 0;
bool compile_shader(const packs::ShaderReplacement &, std::string &) {
    ++compilations;
    return true;
}
} // namespace

int main() {
    using namespace packs;
    assert(uwp::Preferences{}.visuals == Visuals::Remaster);
    assert(parse_visuals(0) == Visuals::Remaster);
    assert(parse_visuals(1) == Visuals::Original);
    assert(parse_visuals(-1) == Visuals::Remaster);
    assert(parse_visuals(2) == Visuals::Remaster);
    assert(next_visuals(next_visuals(Visuals::Remaster)) == Visuals::Remaster);
    assert(next_visuals(Visuals::Remaster) == Visuals::Original);

    BuildPlan compiled;
    compiled.rpx_sha256 = "synthetic-rpx";
    compiled.fingerprint = "synthetic-selection";
    compiled.module_checksum = 123;
    compiled.external_timing = true;
    compiled.initial_memory = {{0x1000, {1, 2, 3, 4}}};
    const TextureRule rule{{{"width", {1280}}, {"overwriteWidth", {2560}}}};
    const ShaderReplacement shader{1, 2, ShaderStage::pixel, "synthetic shader"};
    compiled.packs = {{"FPS++", {}, {}, {{"vsyncFrequency", {60}}}},
                      {"Graphics", {rule}, {shader}, {}},
                      {"Enhancements", {rule}, {shader}, {}},
                      {"Draw Distance", {rule}, {}, {}}};

    // Real mod filtering locks packs with static writes. Visuals must still
    // suppress their renderer content without removing compiled code/data.
    mods::ModCatalog catalog;
    catalog.load(std::filesystem::temp_directory_path() / "absent-visuals-mods", compiled);
    const auto filtered = mods::filtered_plan(compiled, catalog);
    auto original = select_visuals(filtered, Visuals::Original);
    const auto remaster = select_visuals(filtered, Visuals::Remaster);
    assert(original.fingerprint != remaster.fingerprint);
    assert(original.rpx_sha256 == compiled.rpx_sha256);
    assert(original.module_checksum == compiled.module_checksum);
    assert(original.initial_memory[0].address == 0x1000);
    assert(original.initial_memory[0].bytes == compiled.initial_memory[0].bytes);
    assert(original.packs.size() == compiled.packs.size());
    assert(has_timing_pack(original) && has_timing_pack(remaster));
    assert(original.external_timing);
    assert(original.packs[0].control[0].values[0] == 60);
    for (const auto &pack : original.packs)
        assert(pack.shaders.empty() && pack.textures.empty());
    assert(find_shader(original, 1, 2, ShaderStage::pixel) == nullptr);
    assert(find_shader(remaster, 1, 2, ShaderStage::pixel) != nullptr);
    assert(texture_overrides(original, {1280, 720, 1}).empty());
    assert(!texture_overrides(remaster, {1280, 720, 1}).empty());
    d3d12_packs::PackHooks renderer(compile_shader);
    std::string error;
    assert(renderer.prepare(original, error) && compilations == 0);
    renderer.commit(original);
    assert(d3d12_packs::active_plan == &original && !d3d12_packs::overrides_textures);
    assert(renderer.prepare(remaster, error) && compilations == 2);
    renderer.commit(remaster);
    assert(d3d12_packs::active_plan == &remaster && d3d12_packs::overrides_textures);
    assert(compiled.packs[1].shaders.size() == 1); // Input remains reusable on next launch.
    original.packs.clear();
    assert(remaster.packs[1].shaders.size() == 1); // Installed snapshots are independent.

    // A renderer-only build and an empty vanilla build need no special case.
    compiled.initial_memory.clear();
    compiled.external_timing = false;
    auto renderer_only = select_visuals(compiled, Visuals::Original);
    assert(has_timing_pack(renderer_only));
    assert(renderer_only.packs[1].shaders.empty());
    catalog.load(std::filesystem::temp_directory_path() / "absent-visuals-mods", compiled);
    for (const auto &entry : catalog.list())
        if (entry.name == "Graphics")
            assert(catalog.setEnabled(entry.id, false));
    const auto disabled = select_visuals(mods::filtered_plan(compiled, catalog), Visuals::Remaster);
    assert(disabled.packs[1].shaders.empty() && disabled.packs[1].textures.empty());
    assert(disabled.packs[2].shaders.size() == 1);
    const auto empty = select_visuals({}, Visuals::Original);
    assert(empty.packs.empty() && empty.initial_memory.empty());
    assert(!has_timing_pack(empty));
    assert(empty.fingerprint != select_visuals({}, Visuals::Remaster).fingerprint);

    for (auto text : {uwp::Text::visuals, uwp::Text::visuals_remaster, uwp::Text::visuals_original,
                      uwp::Text::visuals_limits}) {
        assert(!uwp::text(text, false).empty());
        assert(!uwp::text(text, true).empty());
    }
}
