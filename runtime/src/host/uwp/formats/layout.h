// SPDX-License-Identifier: MIT
#pragma once
#include "reader.h"
namespace uwp::formats {
// A decrypted archive/folder title set becomes one logical tree. Update replaces base,
// while DLC content is mapped to aoc/.
inline std::vector<Entry> merge_titles(std::vector<Entry> input) {
    struct Layer {
        std::string root;
        int priority;
    };
    std::vector<Layer> layers;
    std::set<std::string> roots;
    auto add = [&](std::string root, int priority) {
        if (roots.insert(root).second)
            layers.push_back({std::move(root), priority});
    };
    for (auto &entry : input) {
        auto marker = entry.path.find("/code/U-King.rpx");
        if (entry.path == "code/U-King.rpx")
            add("", 0);
        else if (marker != entry.path.npos) {
            std::string root = entry.path.substr(0, marker + 1), lower = root;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            int priority =
                (lower.find("0005000e") != lower.npos || lower.find("update") != lower.npos) ? 1 : 0;
            add(root, priority);
        }
    }
    // WUA uses title ID folders. Decrypted folders commonly use [DLC]/[Update].
    for (auto &entry : input) {
        auto slash = entry.path.find('/');
        if (slash == entry.path.npos)
            continue;
        auto root = entry.path.substr(0, slash + 1), lower = root;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (lower.find("0005000c") != lower.npos || lower.find("dlc") != lower.npos ||
            lower.find("aoc") != lower.npos)
            add(root, 2);
    }
    require(!layers.empty(), "Archive has no Wii U game folder");
    auto version = [](const std::string &name) {
        auto p = name.rfind("_v");
        if (p == name.npos)
            return 0ul;
        try {
            return std::stoul(name.substr(p + 2));
        } catch (...) {
            return 0ul;
        }
    };
    std::stable_sort(layers.begin(), layers.end(), [&](auto &a, auto &b) {
        return a.priority != b.priority ? a.priority < b.priority : version(a.root) < version(b.root);
    });
    std::map<std::string, Entry> merged;
    for (auto &layer : layers)
        for (auto &entry : input) {
            if (!entry.path.starts_with(layer.root))
                continue;
            std::string path = entry.path.substr(layer.root.size());
            if (layer.priority == 2) {
                if (!path.starts_with("content/"))
                    continue;
                path = "aoc/" + path.substr(8);
            } else if (!path.starts_with("code/") && !path.starts_with("content/") &&
                       !path.starts_with("meta/") && !path.starts_with("aoc/"))
                continue;
            Entry copy = entry;
            copy.path = path;
            std::string folded = path;
            std::transform(folded.begin(), folded.end(), folded.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            merged.insert_or_assign(folded, std::move(copy));
        }
    std::vector<Entry> result;
    for (auto &[name, entry] : merged)
        result.push_back(std::move(entry));
    validate_entries(result);
    return result;
}
} // namespace uwp::formats
