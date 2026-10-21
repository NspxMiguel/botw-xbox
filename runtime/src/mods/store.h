// SPDX-License-Identifier: MIT
#pragma once
#include "gamebanana.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
namespace mods::store {
enum class Category { all, graphics, performance, language, textures, gameplay, audio, fixes, tools };
enum class SourceId { all, gamebanana, graphics, link, local };
struct Card {
    std::string id, title, author, source, description, version, date;
    std::vector<std::string> images, presets;
    std::vector<OnlineFile> downloads;
    Category category = Category::gameplay;
    uint64_t size = 0, downloads_count = 0, likes = 0;
    std::string installed_id, installed_version, installed_date;
    bool enabled = false, locked = false;
    size_t order = 0, total = 0;
    bool update() const;
};
struct Page {
    std::vector<Card> cards;
    bool more = false, offline = false;
    std::string error;
};
inline constexpr size_t response_byte_limit = 8 * 1024 * 1024;
// Only transport failures permit an offline cache fallback. Cancellation propagates.
struct NetworkError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
using Fetch = std::function<std::string(const std::string &)>;
Category category(std::string text);
std::string plain_text(std::string_view text, size_t limit = 1200);
bool matches(const Card &, std::string query, Category);
std::vector<Card> parse_github_tree(std::string_view json);
void parse_rules(Card &, std::string_view rules);
Card parse_gamebanana_detail(std::string_view json);
std::vector<Card> parse_gamebanana_cards(std::string_view json);
std::vector<OnlineFile> parse_release(std::string_view json);
// Cache stores timestamp + response atomically; all methods are worker-thread only.
class Cache {
    std::filesystem::path root_;

  public:
    explicit Cache(std::filesystem::path root) : root_(std::move(root)) {}
    std::string get(const std::string &url, const Fetch &, bool &offline, bool refresh = false) const;
};
class Source {
  public:
    virtual ~Source() = default;
    virtual Page search(std::string query, Category category, unsigned page) = 0;
    virtual Card files(Card card) = 0;
};
std::unique_ptr<Source> make_source(SourceId, Fetch, std::filesystem::path mods_root,
                                    std::vector<std::filesystem::path> removable = {}, bool refresh = false);
void finish_install(const std::filesystem::path &directory, const std::function<void()> &commit);
void save_receipt(const std::filesystem::path &directory, const Card &);
void read_receipt(const std::filesystem::path &directory, Card &);
void remember_search(const std::filesystem::path &root, std::string query);
std::vector<std::string> recent_searches(const std::filesystem::path &root);
} // namespace mods::store
