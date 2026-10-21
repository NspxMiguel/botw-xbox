// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace mods {
struct OnlineMod {
    uint64_t id = 0;
    std::string name, category, author;
};
struct OnlinePage {
    uint64_t count = 0;
    bool complete = false;
    std::vector<OnlineMod> records;
};
struct OnlineFile {
    std::string name, url, md5;
    uint64_t size = 0;
    bool supported() const;
};
std::string percent_encode(std::string_view text);
std::string gamebanana_url(unsigned page, std::string_view search = {});
std::string gamebanana_files_url(uint64_t id);
OnlinePage parse_gamebanana(std::string_view json);
std::vector<OnlineFile> parse_gamebanana_files(std::string_view json);
bool https_zip_url(std::string_view url);
} // namespace mods
