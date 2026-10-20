// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace vfs {
// W5/launcher adapter: paths are pack roots containing content/ and/or aoc/.
// Plain content trees are supported by pointing path directly at that tree and
// setting volume to "content" (or "aoc"). Order is low to high priority.
struct Mod {
    std::string name;
    std::filesystem::path path;
    bool enabled = true;
    std::string volume{};
};
using Logger = void (*)(const std::string &message);
struct Entry {
    std::string name;
    std::string host;
};

class Overlay {
  public:
    // Configure before FS clients start; do not reconfigure while handles are open.
    void configure(const std::filesystem::path &game, const std::vector<Mod> &mods, Logger logger = nullptr);
    void configure_defaults(const std::filesystem::path &game, const std::filesystem::path &local_state,
                            const std::filesystem::path &mods_root = {}, Logger logger = nullptr);
    bool active() const { return !nodes_.empty(); }
    // Call only when active. Non-content volumes pass through unchanged.
    std::string resolve(const std::string &host);
    // False means use the native directory API. True includes missing directories;
    // exists tells the caller whether to return NOT_FOUND.
    bool list(const std::string &host, std::vector<Entry> &entries, bool &exists);

  private:
    struct Node {
        std::filesystem::path host;
        std::string mod;
        bool directory;
    };
    std::string key(const std::string &host) const;
    std::filesystem::path base(const std::string &key) const;
    void used(const std::string &key, const Node &node);
    std::filesystem::path game_;
    std::map<std::string, Node> nodes_;
    std::set<std::string> logged_;
    std::mutex log_mutex_;
    Logger logger_ = nullptr;
    bool configured_ = false;
};

// Runtime singleton. Host may configure it explicitly before FSInit; otherwise
// FSInit reads enabled.txt in <game>/../mods then <LocalState>/mods.
Overlay &content_overlay();
// Normalize only the two overlay volume prefixes; leave save/code paths alone.
void normalize_content_volume(std::string &guest);
} // namespace vfs
