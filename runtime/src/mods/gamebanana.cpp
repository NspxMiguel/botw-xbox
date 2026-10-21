// SPDX-License-Identifier: MIT
#include "gamebanana.h"
#include "json_min.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace mods {
namespace {
uint64_t integer(const json::Value &v) {
    double n = v.number();
    if (n < 0 || n > 9007199254740991. || std::floor(n) != n)
        throw std::invalid_argument("Invalid API number");
    return static_cast<uint64_t>(n);
}
void require_array(const json::Value &v) {
    if (!std::holds_alternative<json::Value::Array>(v.data))
        throw std::invalid_argument("Invalid API response");
}
bool zip_extension(std::string_view name) {
    if (name.size() < 4)
        return false;
    std::string ext(name.substr(name.size() - 4));
    for (auto &c : ext)
        if (c >= 'A' && c <= 'Z')
            c += 32;
    return ext == ".zip";
}
} // namespace
bool OnlineFile::supported() const { return zip_extension(name); }
bool https_zip_url(std::string_view url) {
    if (!url.starts_with("https://") || url.size() > 4096 || url.find_first_of("\r\n\t \\#") != url.npos)
        return false;
    auto end = url.find('?');
    auto path = url.substr(0, end);
    auto slash = path.find('/', 8);
    return slash != path.npos && slash > 8 && zip_extension(path);
}
std::string percent_encode(std::string_view text) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~')
            out += char(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    return out;
}
std::string gamebanana_url(unsigned page, std::string_view search) {
    auto base = std::string("https://gamebanana.com/apiv11/");
    if (search.empty())
        return base + "Mod/Index?_nPerpage=15&_nPage=" + std::to_string(std::max(1u, page)) +
               "&_aFilters[Generic_Game]=5866&_sSort=Generic_MostDownloaded";
    return base + "Util/Search/Results?_sModelName=Mod&_sOrder=best_match&_idGameRow=5866&_sSearchString=" +
           percent_encode(search) + "&_nPage=" + std::to_string(std::max(1u, page));
}
std::string gamebanana_files_url(uint64_t id) {
    return "https://gamebanana.com/apiv11/Mod/" + std::to_string(id) + "/DownloadPage";
}
OnlinePage parse_gamebanana(std::string_view text) {
    auto root = json::parse(text);
    require_array(root["_aRecords"]);
    OnlinePage out;
    out.count = integer(root["_aMetadata"]["_nRecordCount"]);
    out.complete = root["_aMetadata"]["_bIsComplete"].boolean();
    for (auto &v : root["_aRecords"].array()) {
        try {  // one malformed record is skipped, not the whole page
            OnlineMod m{integer(v["_idRow"]), v["_sName"].string(), v["_aRootCategory"]["_sName"].string(),
                        v["_aSubmitter"]["_sName"].string()};
            if (m.id && !m.name.empty())
                out.records.push_back(std::move(m));
        } catch (const std::exception &) {
        }
    }
    return out;
}
std::vector<OnlineFile> parse_gamebanana_files(std::string_view text) {
    auto root = json::parse(text);
    require_array(root["_aFiles"]);
    std::vector<OnlineFile> out;
    for (auto &v : root["_aFiles"].array()) {
        try {
            OnlineFile f{v["_sFile"].string(), v["_sDownloadUrl"].string(), v["_sMd5Checksum"].string(),
                         integer(v["_nFilesize"])};
            if (!f.name.empty() && f.url.starts_with("https://"))
                out.push_back(std::move(f));
        } catch (const std::exception &) {
        }
    }
    return out;
}
} // namespace mods
