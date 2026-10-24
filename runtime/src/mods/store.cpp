// SPDX-License-Identifier: MIT
#include "store.h"
#include "../packs/sha256.h"
#include "json_min.h"
#include "local.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
namespace mods::store {
namespace fs = std::filesystem;
namespace {
std::string lower(std::string s) {
    for (auto &c : s)
        if (c >= 'A' && c <= 'Z')
            c += 32;
    return s;
}
std::string search_key(std::string s) {
    s = lower(std::move(s));
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        auto c = static_cast<unsigned char>(s[i]);
        if (c == 0xc3 && i + 1 < s.size()) {
            auto v = static_cast<unsigned char>(s[i + 1]);
            char folded = 0;
            if ((v >= 0x80 && v <= 0x85) || (v >= 0xa0 && v <= 0xa5))
                folded = 'a';
            else if (v == 0x87 || v == 0xa7)
                folded = 'c';
            else if ((v >= 0x88 && v <= 0x8b) || (v >= 0xa8 && v <= 0xab))
                folded = 'e';
            else if ((v >= 0x8c && v <= 0x8f) || (v >= 0xac && v <= 0xaf))
                folded = 'i';
            else if (v == 0x91 || v == 0xb1)
                folded = 'n';
            else if ((v >= 0x92 && v <= 0x96) || (v >= 0xb2 && v <= 0xb6))
                folded = 'o';
            else if ((v >= 0x99 && v <= 0x9c) || (v >= 0xb9 && v <= 0xbc))
                folded = 'u';
            else if (v == 0x9d || v == 0xbd || v == 0xbf)
                folded = 'y';
            if (folded) {
                out += folded;
                ++i;
                continue;
            }
        }
        out += char(c);
    }
    return out;
}
uint64_t number(const json::Value &v) {
    auto n = v.number();
    return n > 0 && n < 9e15 ? uint64_t(n) : 0;
}
std::string read(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
std::string path_string(const fs::path &p) {
    auto s = p.generic_u8string();
    return {s.begin(), s.end()};
}
bool newer_version(std::string a, std::string b) {
    // Numeric dotted releases: 1.10 must sort after 1.9. Opaque versions need a newer date.
    auto parts = [](std::string s) {
        std::vector<unsigned long> out;
        if (s.starts_with('v'))
            s.erase(0, 1);
        std::istringstream in(s);
        std::string p;
        while (std::getline(in, p, '.')) {
            if (p.empty() || p.find_first_not_of("0123456789") != p.npos || p.size() > 8)
                return std::vector<unsigned long>{};
            out.push_back(std::stoul(p));
        }
        return out;
    };
    auto x = parts(a), y = parts(b);
    if (x.empty() || y.empty())
        return false;
    x.resize(std::max(x.size(), y.size()));
    y.resize(x.size());
    return x > y;
}
constexpr auto release_url = "https://api.github.com/repos/cemu-project/cemu_graphic_packs/releases/latest";
constexpr auto tree_url =
    "https://api.github.com/repos/cemu-project/cemu_graphic_packs/git/trees/master?recursive=1";
std::string rules_url(const std::string &path, const std::string &ref) {
    return "https://raw.githubusercontent.com/cemu-project/cemu_graphic_packs/" +
           (ref.empty() ? std::string("master") : percent_encode(ref)) + "/" + path + "/rules.txt";
}
class Remote : public Source {
    SourceId source_;
    Fetch fetch_;
    Cache cache_;
    bool offline_ = false, refresh_ = false;
    std::string get(const std::string &url) { return cache_.get(url, fetch_, offline_, refresh_); }

  public:
    Remote(SourceId s, Fetch f, fs::path root, bool refresh = false)
        : source_(s), fetch_(std::move(f)), cache_(root / "cache" / "responses"), refresh_(refresh) {}
    Page search(std::string query, Category cat, unsigned page) override {
        offline_ = false;
        Page out;
        if (source_ == SourceId::gamebanana) {
            if (page == 1 && query.empty())
                out = featured_cards(*this, cat);
            try {
                auto response = get(gamebanana_url(page, query));
                auto parsed = parse_gamebanana(response);
                for (auto &c : parse_gamebanana_cards(response)) {
                    if (query.empty() && std::any_of(out.cards.begin(), out.cards.end(),
                                                     [&](auto &featured) { return featured.id == c.id; }))
                        continue;
                    out.cards.push_back(std::move(c));
                }
                out.more = !parsed.complete && page * 15 < parsed.count;
            } catch (const std::exception &) {
                if (out.cards.empty())
                    throw;
                out.error = "Catalog unavailable";
            }
        } else {
            if (page > 1)
                return out;
            auto release = json::parse(get(release_url));
            auto version = release["tag_name"].string(), date = release["published_at"].string();
            auto tree = std::string(tree_url);
            if (!version.empty())
                tree = "https://api.github.com/repos/cemu-project/cemu_graphic_packs/git/trees/" +
                       percent_encode(version) + "?recursive=1";
            out.cards = parse_github_tree(get(tree));
            for (auto &c : out.cards) {
                c.version = version;
                c.date = date;
            }
        }
        std::erase_if(out.cards, [&](const Card &c) { return !matches(c, query, cat); });
        out.offline = offline_;
        return out;
    }
    Card files(Card c) override {
        if (source_ == SourceId::gamebanana) {
            auto info =
                parse_gamebanana_detail(get("https://gamebanana.com/apiv11/Mod/" + c.id + "/ProfilePage"));
            c.description = info.description;
            if (!info.title.empty())
                c.title = info.title;
            if (!info.author.empty())
                c.author = info.author;
            if (!info.title.empty())
                c.category = info.category;
            c.downloads_count = info.downloads_count;
            c.likes = info.likes;
            c.version = info.version;
            c.date = info.date;
            if (!info.images.empty())
                c.images = std::move(info.images);
            c.downloads = parse_gamebanana_files(get(gamebanana_files_url(std::stoull(c.id))));
        } else {
            auto response = get(release_url);
            auto release = json::parse(response);
            c.version = release["tag_name"].string();
            c.date = release["published_at"].string();
            parse_rules(c, get(rules_url(c.id, c.version)));
            c.downloads = parse_release(response);
        }
        return c;
    }
};
class Link : public Source {
    Fetch fetch_;
    fs::path root_;

  public:
    Link(Fetch f, fs::path root) : fetch_(std::move(f)), root_(std::move(root)) {}
    Page search(std::string url, Category, unsigned) override {
        if (url.empty())
            return {};
        if (!url.starts_with("https://") || url.size() > 4096 || url.find_first_of("\r\n\t \\") != url.npos)
            throw std::invalid_argument("Invalid HTTPS link");
        Card c;
        c.id = url;
        auto path = url.substr(0, url.find_first_of("?#"));
        c.title = path.substr(path.find_last_of('/') + 1);
        c.source = "Link";
        c.downloads = {{c.title, url, "", 0}};
        if (url.starts_with("https://gamebanana.com/mods/")) {
            c.id = url.substr(28);
            c.id = c.id.substr(0, c.id.find_first_of("/?#"));
            if (c.id.empty() || c.id.find_first_not_of("0123456789") != c.id.npos)
                throw std::invalid_argument("Invalid mod ID");
            c.source = "GameBanana";
            c = Remote(SourceId::gamebanana, fetch_, root_).files(c);
        } else if (url.starts_with("https://github.com/")) {
            auto path = url.substr(19);
            auto slash = path.find('/');
            auto end = path.find('/', slash + 1);
            if (slash == path.npos)
                throw std::invalid_argument("Invalid GitHub repository");
            auto repo = path.substr(0, end);
            if (path.find("/releases/download/") == path.npos) {
                bool offline = false;
                Cache cache(root_ / "cache" / "responses");
                auto tag = path.find("/releases/tag/");
                auto endpoint = tag == path.npos ? "latest" : "tags/" + percent_encode(path.substr(tag + 14));
                c.downloads = parse_release(cache.get(
                    "https://api.github.com/repos/" + repo + "/releases/" + endpoint, fetch_, offline));
            }
        }
        Page p;
        p.cards.push_back(std::move(c));
        return p;
    }
    Card files(Card c) override { return c; }
};
class Local : public Source {
    std::vector<fs::path> roots_;

  public:
    explicit Local(std::vector<fs::path> roots) : roots_(std::move(roots)) {}
    Page search(std::string query, Category cat, unsigned page) override {
        Page out;
        if (page > 1)
            return out;
        for (const auto &drive : roots_) {
            // Portable adapter only. UWP uses brokered discovery instead of USB CRT paths.
            for (auto directory_name : local::roots) {
                auto root = drive / directory_name;
                std::error_code ec;
                if (fs::is_symlink(root, ec))
                    continue;
                for (auto &e : fs::directory_iterator(root, ec)) {
                    auto name = path_string(e.path().filename());
                    if (!local::component(name) || e.is_symlink(ec))
                        continue;
                    bool directory = false, graphics = false;
                    if (e.is_directory(ec))
                        for (auto &child : fs::directory_iterator(e.path(), ec)) {
                            if (child.is_symlink(ec))
                                continue;
                            auto child_name = path_string(child.path().filename());
                            directory |= child.is_directory(ec) && local::content_folder(child_name);
                            graphics |=
                                child.is_regular_file(ec) && local::equal_name(child_name, "rules.txt");
                        }
                    directory |= graphics;
                    auto ext = lower(path_string(e.path().extension()));
                    if (!directory &&
                        (!e.is_regular_file(ec) || (ext != ".zip" && ext != ".7z" && ext != ".rar")))
                        continue;
                    Card c;
                    c.id = path_string(e.path());
                    if (std::any_of(out.cards.begin(), out.cards.end(),
                                    [&](auto &seen) { return seen.id == c.id; }))
                        continue;
                    c.title = name;
                    c.source = "USB";
                    c.category = graphics ? Category::graphics : category(c.title);
                    if (!directory) {
                        c.size = e.file_size(ec);
                        c.downloads = {{c.title, c.id, "", c.size}};
                    }
                    if (graphics)
                        for (auto &child : fs::directory_iterator(e.path(), ec))
                            if (!child.is_symlink(ec) &&
                                local::equal_name(path_string(child.path().filename()), "rules.txt"))
                                parse_rules(c, read(child.path()));
                    if (matches(c, query, cat))
                        out.cards.push_back(std::move(c));
                }
            }
        }
        return out;
    }
    Card files(Card c) override { return c; }
};
} // namespace
bool Card::update() const {
    return !installed_id.empty() &&
           (newer_version(version, installed_version) ||
            (!date.empty() && !installed_date.empty() &&
             (date.size() == installed_date.size() ? date > installed_date
                                                   : date.size() > installed_date.size())));
}
Category category(std::string s) {
    s = search_key(s);
    auto has = [&](const char *v) { return s.find(v) != s.npos; };
    if (has("translat") || has("language") || has("dubb") || has("voice") || has("localiz") || has("tradu") ||
        has("dubl"))
        return Category::language;
    if (has("fps") || has("performance") || has("speed"))
        return Category::performance;
    if (has("texture") || has("skin"))
        return Category::textures;
    if (has("audio") || has("sound") || has("music"))
        return Category::audio;
    if (has("fix") || has("bug"))
        return Category::fixes;
    if (has("tool") || has("utilit"))
        return Category::tools;
    if (has("enhancement") || has("clarity") || has("graphic") || has("resolution") || has("shadow") ||
        has("lod") || has("anti") || has("visual") || has("draw distance"))
        return Category::graphics;
    return Category::gameplay;
}
std::string plain_text(std::string_view text, size_t limit) {
    std::string out;
    bool tag = false, space = false;
    for (char c : text) {
        if (c == '<') {
            tag = true;
            space = true;
            continue;
        }
        if (c == '>') {
            tag = false;
            continue;
        }
        if (tag)
            continue;
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            space = true;
            continue;
        }
        if (space && !out.empty())
            out += ' ';
        space = false;
        out += c;
        if (out.size() >= limit)
            break;
    }
    for (auto pair : {std::pair{"&amp;", "&"},
                      {"&quot;", "\""},
                      {"&#39;", "'"},
                      {"&nbsp;", " "},
                      {"&lt;", "<"},
                      {"&gt;", ">"}}) {
        size_t p = 0;
        while ((p = out.find(pair.first, p)) != out.npos) {
            out.replace(p, std::char_traits<char>::length(pair.first), pair.second);
            p += std::char_traits<char>::length(pair.second);
        }
    }
    // Never cut a UTF-8 sequence in half.
    if (out.size() >= limit) {
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xc0) == 0x80)
            out.pop_back();
        if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xc0)
            out.pop_back();
    }
    return out;
}
bool matches(const Card &c, std::string q, Category cat) {
    return (cat == Category::all || cat == c.category) &&
           search_key(c.title + " " + c.author + " " + c.description).find(search_key(q)) !=
               std::string::npos;
}
std::vector<Card> parse_gamebanana_cards(std::string_view text) {
    std::vector<Card> out;
    auto root = json::parse(text);
    for (auto &v : root["_aRecords"].array()) {
        Card c;
        c.id = std::to_string(number(v["_idRow"]));
        c.title = plain_text(v["_sName"].string(), 160);
        c.source = "GameBanana";
        c.author = plain_text(v["_aSubmitter"]["_sName"].string(), 120);
        c.category = category(v["_aRootCategory"]["_sName"].string() + " " +
                              v["_aCategory"]["_sName"].string() + " " + c.title);
        c.size = number(v["_nFilesize"]);
        c.description = plain_text(v["_sDescription"].string());
        c.downloads_count = number(v["_nDownloadCount"]);
        c.likes = number(v["_nLikeCount"]);
        c.version = v["_sVersion"].string();
        if (number(v["_tsDateUpdated"]))
            c.date = std::to_string(number(v["_tsDateUpdated"]));
        auto &media = v["_aPreviewMedia"]["_aImages"];
        for (auto &im : media.array()) {
            // The 220 px thumbnail is WebP, which the console's WIC cannot decode; the original is JPG/PNG.
            auto file = im["_sFile220"].string();
            if (file.empty() || file.ends_with(".webp"))
                file = im["_sFile"].string();
            auto url = im["_sBaseUrl"].string() + "/" + file;
            if (url.starts_with("https://")) {
                if (c.images.size() < 8)
                    c.images.push_back(url);
            }
        }
        if (c.id != "0" && !c.title.empty())
            out.push_back(std::move(c));
    }
    return out;
}
Card parse_gamebanana_detail(std::string_view text) {
    auto root = json::parse(text);
    Card c;
    c.id = std::to_string(number(root["_idRow"]));
    c.title = plain_text(root["_sName"].string(), 160);
    c.author = plain_text(root["_aSubmitter"]["_sName"].string(), 120);
    c.source = "GameBanana";
    c.category = category(root["_aRootCategory"]["_sName"].string() + " " +
                          root["_aCategory"]["_sName"].string() + " " + c.title);
    c.downloads_count = number(root["_nDownloadCount"]);
    c.likes = number(root["_nLikeCount"]);
    c.description = plain_text(root["_sText"].string());
    c.version = root["_sVersion"].string();
    if (number(root["_tsDateUpdated"]))
        c.date = std::to_string(number(root["_tsDateUpdated"]));
    for (auto &im : root["_aPreviewMedia"]["_aImages"].array()) {
        auto file = im["_sFile"].string();
        if (file.empty() || file.ends_with(".webp"))
            file = im["_sFile220"].string();
        auto url = im["_sBaseUrl"].string() + "/" + file;
        if (url.starts_with("https://")) {
            if (c.images.size() < 8)
                c.images.push_back(url);
        }
    }
    return c;
}
std::vector<Card> parse_github_tree(std::string_view text) {
    auto root = json::parse(text);
    std::vector<Card> out;
    for (auto &v : root["tree"].array()) {
        auto path = v["path"].string();
        if (!path.ends_with("/rules.txt"))
            continue;
        auto name = path.substr(0, path.size() - 10);
        auto base = name.substr(name.find_last_of('/') + 1);
        auto modern = name.find("BreathOfTheWild/");
        if (!base.starts_with("BreathOfTheWild_") && modern == name.npos)
            continue;
        Card c;
        c.id = name;
        c.title = modern == name.npos ? base.substr(16) : name.substr(modern + 16);
        std::replace(c.title.begin(), c.title.end(), '_', ' ');
        std::replace(c.title.begin(), c.title.end(), '/', ' ');
        c.author = "Cemu community";
        c.source = "Graphic packs";
        c.category = category(c.title);
        out.push_back(std::move(c));
    }
    return out;
}
void parse_rules(Card &c, std::string_view rules) {
    c.presets.clear();
    std::istringstream in{std::string(rules)};
    std::string line, section;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.starts_with('['))
            section = line;
        auto eq = line.find('=');
        if (eq == line.npos)
            continue;
        auto key = line.substr(0, eq), value = line.substr(eq + 1);
        auto trim = [](std::string s) {
            auto b = s.find_first_not_of(" \t\"");
            if (b == s.npos)
                return std::string{};
            return s.substr(b, s.find_last_not_of(" \t\"") - b + 1);
        };
        key = trim(key);
        value = trim(value);
        if (section == "[Definition]") {
            if (key == "name")
                c.title = value;
            if (key == "description") {
                std::replace(value.begin(), value.end(), '|', ' ');
                c.description = plain_text(value);
            }
        }
        if (section == "[Preset]" && key == "name") {
            if (c.presets.size() < 64)
                c.presets.push_back(plain_text(value, 160));
        }
    }
    c.category = category(c.title + " " + c.id);
}
std::vector<OnlineFile> parse_release(std::string_view text) {
    auto root = json::parse(text);
    std::vector<OnlineFile> out;
    for (auto &v : root["assets"].array()) {
        OnlineFile f{v["name"].string(), v["browser_download_url"].string(), "", number(v["size"])};
        if (f.url.starts_with("https://") &&
            (f.supported() || f.name.ends_with(".7z") || f.name.ends_with(".rar")))
            out.push_back(std::move(f));
    }
    return out;
}
std::string Cache::get(const std::string &url, const Fetch &fetch, bool &offline, bool refresh) const {
    auto hash = packs::sha256_bytes(reinterpret_cast<const uint8_t *>(url.data()), url.size());
    auto path = root_ / hash;
    auto cached = read(path);
    auto newline = cached.find('\n');
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    long long stamp = 0;
    if (newline != cached.npos) {
        try {
            stamp = std::stoll(cached.substr(0, newline));
        } catch (...) {
        }
    }
    auto validate = [&](const std::string &body) {
        if (url.starts_with("https://raw.githubusercontent.com/")) {
            if (body.find("[Definition]") == body.npos || body.find_first_not_of(" \r\n\t") == body.npos ||
                body[body.find_first_not_of(" \r\n\t")] != '[')
                throw std::invalid_argument("Invalid graphic pack rules");
            Card parsed;
            parse_rules(parsed, body);
        } else {
            (void)json::parse(body);
        }
    };
    if (stamp > 0) {
        try {
            validate(cached.substr(newline + 1));
        } catch (...) {
            stamp = 0;
        }
    }
    if (!refresh && stamp > 0 && now >= stamp && now - stamp < 3600)
        return cached.substr(newline + 1);
    std::string body;
    try {
        body = fetch(url);
    } catch (const NetworkError &) {
        if (stamp > 0) {
            offline = true;
            return cached.substr(newline + 1);
        }
        throw;
    }
    if (body.size() > response_byte_limit)
        throw std::runtime_error("Response too large");
    validate(body);
    fs::create_directories(root_);
    auto tmp = path;
    tmp += ".tmp-" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream out(tmp, std::ios::binary);
        out << now << '\n' << body;
        out.close();
        if (!out)
            throw std::runtime_error("Cache write failed");
    }
#ifdef _WIN32
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cache replace failed");
#else
    fs::rename(tmp, path);
#endif
    return body;
}
std::unique_ptr<Source> make_source(SourceId id, Fetch fetch, fs::path root, std::vector<fs::path> removable,
                                    bool refresh) {
    if (id == SourceId::link)
        return std::make_unique<Link>(std::move(fetch), std::move(root));
    if (id == SourceId::local)
        return std::make_unique<Local>(std::move(removable));
    return std::make_unique<Remote>(id, std::move(fetch), std::move(root), refresh);
}
Page featured_cards(Source &source, Category cat) {
    Page out;
    for (auto id : featured_gamebanana_ids) {
        try {
            Card card;
            card.id = id;
            card.source = "GameBanana";
            card = source.files(std::move(card));
            if (card.title.empty() || card.author.empty())
                continue;
            card.featured = true;
            if (matches(card, "", cat))
                out.cards.push_back(std::move(card));
        } catch (const std::exception &) {
            // Optional recommendations never fail the catalog. Platform cancellation still propagates.
        }
    }
    return out;
}
size_t preferred_file(const Card &card, int layout) {
    size_t selected = 0;
    int best = -1;
    for (size_t i = 0; i < card.downloads.size(); ++i) {
        auto name = lower(card.downloads[i].name);
        bool xbox = name.find("xbox") != name.npos;
        int score = (xbox == (layout == 1) ? 2 : 0) + (name.find("jp") == name.npos ? 1 : 0);
        if (score > best) {
            best = score;
            selected = i;
        }
    }
    return selected;
}
bool should_offer_translation(bool portuguese, bool asked_before, const std::vector<Card> &installed) {
    if (!portuguese || asked_before)
        return false;
    return std::none_of(installed.begin(), installed.end(), [](const Card &card) {
        if (card.source == "GameBanana" && card.id == ptbr_translation_id)
            return true;
        // Console/USB installs from before store receipts also count, even when disabled.
        auto id = lower(card.installed_id);
        return id == "file:ptbr_oficial_xbox_western" || id == "file:ptbr_oficial_116d4" ||
               id == "file:ptbr_jp" || id == "file:ptbr_jp_xbox_western";
    });
}
void finish_install(const fs::path &directory, const std::function<void()> &commit) {
    try {
        commit();
    } catch (...) {
        std::error_code cleanup;
        fs::remove_all(directory, cleanup);
        throw;
    }
}
void save_receipt(const fs::path &dir, const Card &c) {
    std::ofstream out(dir / ".store-receipt");
    out << std::quoted(c.id) << ' ' << std::quoted(c.source) << ' ' << std::quoted(c.version) << ' '
        << std::quoted(c.date) << ' ' << std::quoted(c.title) << ' ' << std::quoted(c.author) << ' '
        << int(c.category) << ' ' << c.size << ' ' << std::quoted(c.description) << ' ' << c.images.size();
    for (auto &image : c.images)
        out << ' ' << std::quoted(image);
    out.close();
    if (!out)
        throw std::runtime_error("Receipt write failed");
}
void read_receipt(const fs::path &dir, Card &c) {
    std::ifstream in(dir / ".store-receipt");
    Card r;
    if (in >> std::quoted(r.id) >> std::quoted(r.source) >> std::quoted(r.version) >> std::quoted(r.date) >>
        std::quoted(r.title) >> std::quoted(r.author)) {
        c.id = r.id;
        c.source = r.source;
        c.version = c.installed_version = r.version;
        c.date = c.installed_date = r.date;
        c.title = r.title;
        c.author = r.author;
        int cat = 0;
        size_t count = 0;
        if (in >> cat >> c.size >> std::quoted(c.description) >> count) {
            if (cat >= 0 && cat <= 8)
                c.category = Category(cat);
            for (size_t i = 0; i < std::min<size_t>(count, 12); ++i) {
                std::string image;
                if (in >> std::quoted(image))
                    c.images.push_back(image);
            }
        }
    }
}
std::vector<std::string> recent_searches(const fs::path &root) {
    std::ifstream in(root / "cache" / "searches");
    std::vector<std::string> out;
    std::string q;
    while (out.size() < 6 && in >> std::quoted(q))
        out.push_back(q);
    return out;
}
void remember_search(const fs::path &root, std::string q) {
    if (q.empty())
        return;
    auto recent = recent_searches(root);
    std::erase(recent, q);
    recent.insert(recent.begin(), q);
    recent.resize(std::min<size_t>(recent.size(), 6));
    fs::create_directories(root / "cache");
    std::ofstream out(root / "cache" / "searches");
    for (auto &s : recent)
        out << std::quoted(s) << '\n';
}
} // namespace mods::store
