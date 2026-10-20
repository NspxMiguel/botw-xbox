// SPDX-License-Identifier: MIT
#include "overlay.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace vfs {
namespace {
namespace fs = std::filesystem;
fs::path utf8_path(const std::string &text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
}
std::string utf8_string(const fs::path &path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}
void stderr_logger(const std::string &message) { std::fprintf(stderr, "%s\n", message.c_str()); }
std::string fold(std::string value) {
    for (char &c : value) {
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    }
    return value;
}
bool directory(const fs::path &path) {
    std::error_code error;
    return fs::is_directory(path, error);
}
bool regular(const fs::path &path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}
// Component-wise lookup also works on case-sensitive APFS and Linux. Sorting
// makes ambiguous case collisions deterministic, independent of directory order.
fs::path case_path(fs::path root, const fs::path &relative) {
    for (const auto &part : relative) {
        if (part == "." || part.empty())
            continue;
        std::vector<fs::path> matches;
        std::error_code error;
        for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
            if (fold(utf8_string(it->path().filename())) == fold(utf8_string(part)))
                matches.push_back(it->path());
        }
        if (matches.empty())
            return {};
        std::sort(matches.begin(), matches.end());
        root = matches.front();
    }
    return root;
}
std::string trim(std::string line) {
    auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    auto last = line.find_last_not_of(" \t\r\n");
    return line.substr(first, last - first + 1);
}
} // namespace

void normalize_content_volume(std::string &guest) {
    const auto end = guest.find('/', 5);
    const auto prefix = fold(guest.substr(0, end));
    if (prefix == "/vol/content" || prefix == "/vol/aoc")
        guest.replace(0, prefix.size(), prefix);
}

Overlay &content_overlay() {
    static Overlay overlay;
    return overlay;
}

void Overlay::configure(const fs::path &game, const std::vector<Mod> &mods, Logger logger) {
    configured_ = true;
    game_ = fs::absolute(game).lexically_normal();
    if (game_.filename().empty())
        game_ = game_.parent_path();
    nodes_.clear();
    logged_.clear();
    logger_ = logger ? logger : stderr_logger;
    for (const auto &mod : mods) {
        if (!mod.enabled)
            continue;
        for (const std::string volume : {"content", "aoc"}) {
            if (!mod.volume.empty() && mod.volume != volume)
                continue;
            fs::path root = mod.volume.empty() ? case_path(mod.path, volume) : mod.path;
            // Plain replacement folders have no volume wrapper. A pack with
            // either wrapper must never expose its rules.txt as game content.
            if (root.empty() && mod.volume.empty() && volume == "content" && case_path(mod.path, "aoc").empty() &&
                case_path(mod.path, "rules.txt").empty())
                root = mod.path;
            if (root.empty() || !directory(root))
                continue;
            std::vector<fs::path> paths{root};
            std::error_code error;
            for (fs::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
                // Do not follow links out of a user-selected mod tree.
                if (it->is_symlink(error)) {
                    it.disable_recursion_pending();
                    continue;
                }
                paths.push_back(it->path());
            }
            std::sort(paths.begin(), paths.end());
            for (const auto &path : paths) {
                bool is_dir = directory(path);
                if (!is_dir && !regular(path))
                    continue;
                std::string relative = utf8_string(path.lexically_relative(root));
                std::string k = volume + (relative == "." ? "" : "/" + fold(relative));
                // A later file masks an earlier directory and all its descendants.
                if (!is_dir) {
                    auto child = nodes_.lower_bound(k + "/");
                    while (child != nodes_.end() && child->first.starts_with(k + "/"))
                        child = nodes_.erase(child);
                }
                nodes_[k] = {path, mod.name.empty() ? utf8_string(mod.path.filename()) : mod.name, is_dir};
            }
        }
    }
}

void Overlay::configure_defaults(const fs::path &game, const fs::path &local_state, const fs::path &mods_root,
                                 Logger logger) {
    if (configured_)
        return;
    std::vector<Mod> mods;
    std::vector<fs::path> roots;
    if (!mods_root.empty())
        roots.push_back(mods_root);
    else {
        auto game_path = fs::absolute(game).lexically_normal();
        if (game_path.filename().empty())
            game_path = game_path.parent_path();
        roots = {game_path.parent_path() / "mods", local_state / "mods"};
    }
    std::set<fs::path> seen;
    for (const auto &root : roots) {
        if (!seen.insert(fs::absolute(root).lexically_normal()).second)
            continue;
        std::ifstream input(root / "enabled.txt");
        std::string line;
        while (std::getline(input, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue;
            fs::path path = utf8_path(line);
            if (path.is_relative())
                path = root / path;
            mods.push_back({utf8_string(path.filename()), path, true, {}});
        }
    }
    configure(game, mods, logger);
}

std::string Overlay::key(const std::string &host) const {
    if (host.empty())
        return {};
    fs::path relative = fs::absolute(utf8_path(host)).lexically_normal().lexically_relative(game_);
    if (relative.empty())
        return {};
    auto first = relative.begin();
    std::string volume = fold(utf8_string(*first));
    if (volume != "content" && volume != "aoc")
        return {};
    for (const auto &part : relative)
        if (part == "..")
            return {};
    auto result = fold(utf8_string(relative));
    while (!result.empty() && result.back() == '/')
        result.pop_back();
    return result;
}

fs::path Overlay::base(const std::string &k) const {
    // A replacement file in any ancestor hides the base subtree as well.
    for (auto slash = k.find('/'); slash != std::string::npos; slash = k.find('/', slash + 1)) {
        auto parent = nodes_.find(k.substr(0, slash));
        if (parent != nodes_.end() && !parent->second.directory)
            return {};
    }
    return case_path(game_, utf8_path(k));
}

void Overlay::used(const std::string &k, const Node &node) {
    if (node.directory || !logger_)
        return;
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (logged_.insert(k).second)
        logger_("Mod [" + node.mod + "] /vol/" + k + " -> " + utf8_string(node.host));
}

std::string Overlay::resolve(const std::string &host) {
    if (!active())
        return host;
    auto k = key(host);
    if (k.empty())
        return host;
    if (auto it = nodes_.find(k); it != nodes_.end()) {
        used(k, it->second);
        return utf8_string(it->second.host);
    }
    auto path = base(k);
    // Keep blocked/missing paths inside a deliberately nonexistent child of a
    // regular file where necessary, rather than accidentally opening the base.
    if (path.empty()) {
        for (auto slash = k.find('/'); slash != std::string::npos; slash = k.find('/', slash + 1)) {
            auto parent = nodes_.find(k.substr(0, slash));
            if (parent != nodes_.end() && !parent->second.directory)
                return utf8_string(parent->second.host / utf8_path(k.substr(slash + 1)));
        }
        return host;
    }
    return utf8_string(path);
}

bool Overlay::list(const std::string &host, std::vector<Entry> &entries, bool &exists) {
    if (!active())
        return false;
    auto k = key(host);
    if (k.empty())
        return false;
    entries.clear();
    exists = directory(utf8_path(resolve(host)));
    if (!exists)
        return true;
    std::map<std::string, std::string> names;
    auto base_dir = base(k);
    std::error_code error;
    if (!base_dir.empty()) {
        for (fs::directory_iterator it(base_dir, error), end; !error && it != end; it.increment(error)) {
            auto name = utf8_string(it->path().filename());
            if (!name.empty() && name[0] != '.')
                names.emplace(fold(name), name);
        }
    }
    const auto prefix = k + "/";
    for (auto it = nodes_.lower_bound(prefix); it != nodes_.end() && it->first.starts_with(prefix); ++it) {
        auto rest = it->first.substr(prefix.size());
        if (rest.find('/') != std::string::npos)
            continue;
        auto name = utf8_string(it->second.host.filename());
        if (!name.empty() && name[0] != '.')
            names[rest] = name;
    }
    for (const auto &[unused, name] : names)
        entries.push_back({name, resolve(utf8_string(utf8_path(host) / utf8_path(name)))});
    return true;
}
} // namespace vfs
