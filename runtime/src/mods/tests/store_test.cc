// SPDX-License-Identifier: MIT
#include "../catalog.h"
#include "../store.h"
#include <cassert>
#include <fstream>
#include <iostream>
namespace fs = std::filesystem;
std::string fixture(const char *name) {
    std::ifstream in(fs::path(FIXTURE_ROOT) / name);
    return {std::istreambuf_iterator<char>(in), {}};
}
int main() {
    using namespace mods::store;
    auto gb = parse_gamebanana_cards(fixture("gamebanana.json"));
    assert(gb.size() == 1);
    assert(gb[0].category == Category::language);
    assert(gb[0].likes == 30);
    assert(gb[0].images[0].ends_with("preview.jpg"));
    assert(matches(gb[0], "VOICE", Category::language));
    assert(!matches(gb[0], "VOICE", Category::graphics));
    assert(category("FPS++") == Category::performance);
    assert(category("Shadow resolution") == Category::graphics);
    assert(category("Texture packs") == Category::textures);
    assert(category("Bug fixes") == Category::fixes);
    Card localized;
    localized.title = "Tradução em português";
    assert(matches(localized, "TRADUCAO", Category::all));
    assert(matches(localized, "PORTUGUÊS", Category::all));
    auto modern = parse_github_tree(fixture("github-modern.json"));
    assert(modern.size() == 2 && modern[0].title == "Graphics");
    auto tree = parse_github_tree(fixture("github-tree.json"));
    assert(tree.size() == 2);
    assert(tree[0].title == "FPS++");
    parse_rules(tree[1], fixture("rules.txt"));
    assert(tree[1].title == "Resolution");
    assert(tree[1].presets.size() == 2);
    assert(!tree[1].description.empty());
    assert(parse_release(fixture("release.json")).size() == 1);
    assert(plain_text("<p>Hello &amp; <b>world</b></p>") == "Hello & world");
    Card card;
    card.version = "1.10";
    card.installed_version = "1.9";
    card.installed_id = "file:test";
    assert(card.update());
    card.version = "1.8";
    assert(!card.update());
    card.date = "2026-10-21";
    card.installed_date = "2026-10-20";
    assert(card.update());
    auto root = fs::temp_directory_path() / "w15-store-fixture";
    fs::remove_all(root);
    fs::create_directories(root / "mods" / "sample" / "content");
    const std::string profile = R"({"_idRow":717162,"_sName":"Port de Tradução PT-BR",
        "_aSubmitter":{"_sName":"gurincrivel"},"_aCategory":{"_sName":"Translations"},
        "_sText":"Community translation","_sVersion":"1.0","_tsDateUpdated":1234})";
    const std::string files = R"({"_aFiles":[
        {"_sFile":"ptbr_jp_xbox_western.zip","_sDownloadUrl":"https://example.test/1847226"},
        {"_sFile":"ptbr_jp.zip","_sDownloadUrl":"https://example.test/1847225"},
        {"_sFile":"ptbr_oficial_xbox_western.zip","_sDownloadUrl":"https://example.test/1847227"},
        {"_sFile":"ptbr_oficial_116d4.zip","_sDownloadUrl":"https://example.test/1847223"}]})";
    unsigned featured_calls = 0, listing_calls = 0;
    auto transport = [&](const std::string &url) {
        if (url.ends_with("/717162/ProfilePage")) {
            ++featured_calls;
            return profile;
        }
        if (url == mods::gamebanana_files_url(717162)) {
            ++featured_calls;
            return files;
        }
        ++listing_calls;
        return fixture("gamebanana.json");
    };
    auto banana = make_source(SourceId::gamebanana, transport, root / "featured");
    auto explore = banana->search("", Category::all, 1);
    assert(explore.cards.size() == 2 && explore.cards[0].id == ptbr_translation_id);
    assert(explore.cards[0].featured && !explore.cards[1].featured);
    assert(explore.cards[0].author == "gurincrivel" && explore.cards[0].source == "GameBanana");
    assert(explore.cards[0].category == Category::language && explore.cards[1].id == "42");
    assert(featured_calls == 2 && listing_calls == 1);
    banana->search("", Category::all, 1);
    assert(featured_calls == 2 && listing_calls == 1); // Both cards and metadata use the response cache.
    assert(banana->search("", Category::graphics, 1).cards.empty());
    assert(banana->search("voice", Category::all, 1).cards.size() == 1);
    assert(banana->search("", Category::all, 2).cards.size() == 1);
    assert(featured_calls == 2); // Search and later pages never fetch recommendations.
    auto duplicate = make_source(
        SourceId::gamebanana,
        [&](const std::string &url) {
            if (url.find("/Index?") == url.npos)
                return transport(url);
            return std::string(R"({"_aMetadata":{"_bIsComplete":true},"_aRecords":[
            {"_idRow":717162,"_sName":"Duplicate listing"}]})");
        },
        root / "duplicate");
    auto deduplicated = duplicate->search("", Category::all, 1);
    assert(deduplicated.cards.size() == 1 && deduplicated.cards[0].featured);
    for (bool fail_files : {false, true}) {
        auto failing = make_source(
            SourceId::gamebanana,
            [&](const std::string &url) {
                if (url.ends_with(fail_files ? "/DownloadPage" : "/ProfilePage"))
                    throw NetworkError("Missing featured entry");
                return transport(url);
            },
            root / (fail_files ? "failed-files" : "failed-profile"));
        auto fallback = failing->search("", Category::all, 1);
        assert(fallback.cards.size() == 1 && fallback.cards[0].id == "42");
        assert(fallback.error.empty() && !fallback.offline);
    }
    auto index_unavailable = make_source(
        SourceId::gamebanana,
        [&](const std::string &url) {
            if (url.find("/Index?") != url.npos)
                throw NetworkError("Catalog offline");
            return transport(url);
        },
        root / "featured-only");
    auto featured_only = index_unavailable->search("", Category::all, 1);
    assert(featured_only.cards.size() == 1 && featured_only.cards[0].featured &&
           !featured_only.error.empty());
    auto translation = explore.cards[0];
    assert(preferred_file(translation, 1) == 2);
    assert(preferred_file(translation, 0) == 3);
    translation.downloads.erase(translation.downloads.begin() + 2, translation.downloads.end());
    assert(preferred_file(translation, 1) == 0 && preferred_file(translation, 0) == 1);
    translation.downloads = {{"PTBR_XBOX.ZIP", "", "", 0}, {"PTBR.ZIP", "", "", 0}};
    assert(preferred_file(translation, 1) == 0 && preferred_file(translation, 0) == 1);
    translation.downloads.resize(1);
    assert(preferred_file(translation, 0) == 0);
    translation.downloads.clear();
    assert(preferred_file(translation, 1) == 0);
    for (bool portuguese : {false, true})
        for (bool asked : {false, true})
            assert(should_offer_translation(portuguese, asked, {}) == (portuguese && !asked));
    assert(!should_offer_translation(true, false, {translation}));
    Card manual;
    for (auto folder :
         {"PTBR_Oficial_xbox_western", "ptbr_oficial_116d4", "ptbr_jp", "ptbr_jp_xbox_western"}) {
        manual.installed_id = std::string("file:") + folder;
        assert(!should_offer_translation(true, false, {manual}));
    }
    manual.installed_id = "file:unrelated_translation";
    assert(should_offer_translation(true, false, {manual}));
    std::cout << "featured order/cache/failure, layout preselection and translation offer gating passed\n";
    Cache cache(root / "cache");
    bool offline = false;
    int calls = 0;
    auto fetch = [&](const std::string &) {
        ++calls;
        return fixture("gamebanana.json");
    };
    assert(!cache.get("fixture", fetch, offline).empty());
    cache.get("fixture", fetch, offline);
    assert(calls == 1 && !offline);
    assert(!cache.get(
                     "fixture", [](auto &) -> std::string { throw NetworkError("offline"); }, offline, true)
                .empty());
    assert(offline);
    // A captive portal must never poison the fresh cache or offline fallback.
    int bad_calls = 0;
    auto portal = [&](const std::string &) {
        ++bad_calls;
        return std::string("<html>Sign in</html>");
    };
    for (int i = 0; i < 2; ++i) {
        bool failed = false;
        try {
            cache.get("portal", portal, offline);
        } catch (...) {
            failed = true;
        }
        assert(failed);
    }
    assert(bad_calls == 2);
    cache.get("portal", fetch, offline);
    assert(calls == 2);
    offline = false;
    bool invalid_response = false;
    try {
        cache.get("portal", portal, offline, true);
    } catch (const std::exception &) {
        invalid_response = true;
    }
    assert(invalid_response && !offline);
    assert(cache.get("portal", fetch, offline) == fixture("gamebanana.json"));
    // A non-transport exception (including platform cancellation) must escape a warm cache.
    struct Cancelled {};
    bool cancelled = false;
    try {
        cache.get("fixture", [](auto &) -> std::string { throw Cancelled{}; }, offline, true);
    } catch (const Cancelled &) {
        cancelled = true;
    }
    assert(cancelled && !offline);
    bool local_error = false;
    try {
        cache.get(
            "fixture", [](auto &) -> std::string { throw std::runtime_error("local failure"); }, offline,
            true);
    } catch (const std::runtime_error &) {
        local_error = true;
    }
    assert(local_error && !offline);
    std::string boundary(response_byte_limit, ' ');
    boundary.replace(0, 2, "{}");
    assert(cache.get("limit", [&](auto &) { return boundary; }, offline).size() == response_byte_limit);
    boundary += ' ';
    bool oversized = false;
    try {
        cache.get("limit", [&](auto &) { return boundary; }, offline, true);
    } catch (const std::runtime_error &) {
        oversized = true;
    }
    assert(oversized && !offline);
    assert(cache.get("limit", fetch, offline).size() == response_byte_limit);
    bool unavailable = false;
    try {
        cache.get("uncached", [](auto &) -> std::string { throw NetworkError("offline"); }, offline);
    } catch (const NetworkError &) {
        unavailable = true;
    }
    assert(unavailable && !offline);
    auto old_copy = root / "mods" / "sample";
    auto new_copy = root / "mods" / "new";
    mods::ModCatalog installed;
    installed.load(root / "mods", {});
    installed.setEnabled("file:sample", true);
    installed.save();
    std::ifstream selection(root / "mods" / "enabled.txt");
    std::string original{std::istreambuf_iterator<char>(selection), {}};
    fs::create_directories(new_copy / "content");
    // A directory at the staging filename forces a real catalog save failure.
    fs::create_directory(root / "mods" / "enabled.txt.tmp");
    bool rolled_back = false;
    try {
        finish_install(new_copy, [&] {
            installed.load(root / "mods", {});
            assert(installed.setEnabled("file:sample", false));
            assert(installed.setEnabled("file:new", true));
            assert(installed.move("file:new", 0));
            installed.save();
        });
    } catch (...) {
        rolled_back = true;
    }
    assert(rolled_back && !fs::exists(new_copy) && fs::exists(old_copy));
    std::ifstream after(root / "mods" / "enabled.txt");
    assert(std::string(std::istreambuf_iterator<char>(after), {}) == original);
    fs::remove_all(root / "mods" / "enabled.txt.tmp");
    fs::create_directories(new_copy);
    finish_install(new_copy, [] {});
    assert(fs::exists(new_copy));
    fs::remove_all(new_copy);
    save_receipt(root / "mods" / "sample", gb[0]);
    Card receipt;
    read_receipt(root / "mods" / "sample", receipt);
    assert(receipt.id == "42" && receipt.version == "1.10");
    remember_search(root, "voice");
    remember_search(root, "fps");
    remember_search(root, "voice");
    auto searches = recent_searches(root);
    assert(searches.size() == 2 && searches[0] == "voice");
    auto local = make_source(SourceId::local, fetch, root, {root});
    assert(local->search("sample", Category::all, 1).cards.size() == 1);
    unsigned rules_calls = 0, github_calls = 0;
    auto github = make_source(
        SourceId::graphics,
        [&](const std::string &url) {
            ++github_calls;
            if (url.find("trees/") != url.npos)
                return fixture("github-tree.json");
            if (url.find("releases/") != url.npos)
                return fixture("release.json");
            ++rules_calls;
            return fixture("rules.txt");
        },
        root / "remote");
    assert(github->search("", Category::all, 1).cards.size() == 2);
    assert(github_calls == 2 && rules_calls == 0);
    auto lazy = github->search("", Category::all, 1).cards[1];
    assert(lazy.presets.empty() && lazy.title == "Resolution");
    lazy = github->files(lazy);
    assert(rules_calls == 1 && lazy.presets.size() == 2);
    auto tagged = make_source(
        SourceId::link,
        [&](const std::string &url) {
            assert(url == "https://api.github.com/repos/fixture/repo/releases/tags/feature%2Frelease");
            return fixture("release.json");
        },
        root);
    assert(tagged->search("https://github.com/fixture/repo/releases/tag/feature/release", Category::all, 1)
               .cards.size() == 1);
    auto link =
        make_source(SourceId::link, [&](const std::string &) { return fixture("release.json"); }, root);
    auto linked = link->search("https://github.com/fixture/repo/releases/latest", Category::all, 1);
    assert(linked.cards.size() == 1 && linked.cards[0].downloads.size() == 1);
    bool rejected = false;
    try {
        link->search("file:///private/key", Category::all, 1);
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
    fs::remove_all(root);
    std::cout << "store tests passed\n";
}
