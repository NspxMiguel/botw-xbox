// SPDX-License-Identifier: MIT
#include "catalog.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif
namespace mods {
namespace {
namespace fs = std::filesystem;
std::string utf8(const fs::path &path) {
    auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}
fs::path path8(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string lower(std::string s) {
    for (auto &c : s)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return s;
}
std::string pack_id(size_t i, const packs::Pack &p) {
    std::ostringstream id;
    id << "pack:" << i << ':';
    // Hex encoding keeps arbitrary compiled names on a single enabled.txt line.
    for (unsigned char c : p.name)
        id << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    return id.str();
}
Kind detect(const fs::path &root) {
    bool translation = false, dubbing = false;
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_symlink())
            continue;
        auto s = "/" + lower(utf8(entry.path().lexically_relative(root))) + "/";
        translation |= s.find("/mals/") != s.npos ||
                       (s.find("/message/") != s.npos && lower(utf8(entry.path().extension())) == ".msbt");
        dubbing |= s.find("/voice/") != s.npos || s.find("/sound/resource/stream/") != s.npos;
    }
    return translation ? Kind::translation : dubbing ? Kind::dubbing : Kind::files;
}
} // namespace
void ModCatalog::load(const fs::path &root, const packs::BuildPlan &plan) {
    root_ = root;
    entries_.clear();
    if (fs::exists(root))
        for (const auto &entry : fs::directory_iterator(root)) {
            if (entry.is_symlink() || !entry.is_directory())
                continue;
            bool wrapped = false;
            for (const auto &child : fs::directory_iterator(entry.path()))
                if (!child.is_symlink() && child.is_directory() &&
                    (lower(utf8(child.path().filename())) == "content" ||
                     lower(utf8(child.path().filename())) == "aoc"))
                    wrapped = true;
            if (!wrapped)
                continue;
            auto name = utf8(entry.path().filename());
            // enabled.txt cannot represent line breaks. The ./ prefix and /. suffix
            // protect leading # and whitespace from the overlay's line trimming.
            if (name.find_first_of("\r\n") != name.npos)
                continue;
            entries_.push_back({"file:" + name, name, detect(entry.path()), false, false, {}});
        }
    std::sort(entries_.begin(), entries_.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    for (size_t i = 0; i < plan.packs.size(); ++i)
        entries_.push_back({pack_id(i, plan.packs[i]), plan.packs[i].name, Kind::graphics, true,
                            !plan.initial_memory.empty(),
                            plan.initial_memory.empty() ? "" : "rebuild to change"});
    unknown_.clear();
    std::ifstream input(root / "enabled.txt");
    std::vector<std::string> order;
    std::set<std::string> enabled_files;
    std::string line;
    while (std::getline(input, line)) {
        auto first = line.find_first_not_of(" \t\r");
        if (first == line.npos)
            continue;
        line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
        if (line.starts_with("# w8-mod ")) {
            std::istringstream record(line.substr(9));
            std::string id;
            int enabled;
            if (!(record >> std::quoted(id) >> enabled))
                continue;
            auto it =
                std::find_if(entries_.begin(), entries_.end(), [&](const auto &e) { return e.id == id; });
            if (it != entries_.end()) {
                order.push_back(id);
                if (it->kind == Kind::graphics && !it->locked)
                    it->enabled = enabled != 0;
            }
        } else if (line[0] != '#') {
            auto path = path8(line);
            if (path.is_absolute())
                path = path.lexically_relative(fs::absolute(root));
            path = path.lexically_normal();
            if (path.filename().empty())
                path = path.parent_path();
            auto id = "file:" + utf8(path);
            if (std::none_of(entries_.begin(), entries_.end(), [&](const auto &e) { return e.id == id; })) {
                unknown_.push_back(line);  // plain trees, absolute paths: the overlay still loads them
                continue;
            }
            enabled_files.insert(id);
            order.push_back(id);
        }
    }
    for (auto &entry : entries_)
        if (entry.kind != Kind::graphics)
            entry.enabled = enabled_files.count(entry.id) != 0;
    std::vector<ModInfo> sorted;
    std::set<std::string> seen;
    // Legacy lists can repeat a path: its last occurrence sets its priority.
    for (auto it = order.rbegin(); it != order.rend(); ++it)
        for (const auto &entry : entries_)
            if (entry.id == *it && seen.insert(*it).second)
                sorted.push_back(entry);
    std::reverse(sorted.begin(), sorted.end());
    for (const auto &entry : entries_)
        if (seen.insert(entry.id).second)
            sorted.push_back(entry);
    entries_ = std::move(sorted);
}
bool ModCatalog::setEnabled(const std::string &id, bool enabled) {
    for (auto &entry : entries_)
        if (entry.id == id) {
            if (entry.locked)
                return false;
            entry.enabled = enabled;
            return true;
        }
    return false;
}
bool ModCatalog::move(const std::string &id, size_t index) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const auto &e) { return e.id == id; });
    if (it == entries_.end() || index >= entries_.size())
        return false;
    auto entry = *it;
    entries_.erase(it);
    entries_.insert(entries_.begin() + index, entry);
    return true;
}
void ModCatalog::save() const {
    fs::create_directories(root_);
    auto temporary = root_ / "enabled.txt.tmp";
    std::ofstream out;
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.open(temporary, std::ios::trunc);
    out << "# Lowest priority first. W8 comments retain disabled entries and "
           "compiled packs.\n";
    // ponytail: unlisted lines keep their text but go first (lowest priority); per-line order if it matters.
    for (const auto &line : unknown_)
        out << line << '\n';
    for (const auto &e : entries_) {
        out << "# w8-mod " << std::quoted(e.id) << ' ' << e.enabled << '\n';
        if (e.enabled && e.kind != Kind::graphics) {
            out << "./" << e.name;
            if (!e.name.empty() && (e.name.back() == ' ' || e.name.back() == '\t'))
                out << "/.";
            out << '\n';
        }
    }
    out.close();
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), (root_ / "enabled.txt").c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw fs::filesystem_error("Replace enabled.txt", temporary,
                                   std::error_code(GetLastError(), std::system_category()));
#else
    fs::rename(temporary, root_ / "enabled.txt");
#endif
}
packs::BuildPlan filtered_plan(const packs::BuildPlan &plan, const ModCatalog &catalog) {
    auto result = plan;
    result.packs.clear();
    // Renderer caches must distinguish both selection and order.
    for (const auto &entry : catalog.list())
        if (entry.kind == Kind::graphics)
            result.fingerprint +=
                ":" + std::to_string(entry.id.size()) + ":" + entry.id + (entry.enabled ? ":1" : ":0");
    std::set<size_t> seen;
    for (const auto &entry : catalog.list())
        for (size_t i = 0; i < plan.packs.size(); ++i) {
            if (entry.id != pack_id(i, plan.packs[i]) || !seen.insert(i).second)
                continue;
            auto pack = plan.packs[i];
            if (!entry.enabled && plan.initial_memory.empty()) {
                pack.shaders.clear();
                pack.textures.clear();
            }
            result.packs.push_back(std::move(pack));
        }
    for (size_t i = 0; i < plan.packs.size(); ++i)
        if (!seen.count(i))
            result.packs.push_back(plan.packs[i]);
    return result;
}
} // namespace mods
