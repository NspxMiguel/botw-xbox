// SPDX-License-Identifier: MIT
#pragma once
#include "../../packs/sha256.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace uwp::setup {
inline constexpr char expected_rpx[] = "ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30";
inline constexpr uint64_t margin = 2ull * 1024 * 1024 * 1024;
enum class Step { welcome, guide, detect, verify, wrong_version, storage, copying, automatic, home };
enum class Mode { ask, copy, external, off };
inline std::string mode_name(Mode m) {
    switch (m) {
    case Mode::copy:
        return "copy";
    case Mode::external:
        return "external";
    case Mode::off:
        return "off";
    default:
        return "ask";
    }
}
inline Mode parse_mode(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    if (s == "copy")
        return Mode::copy;
    if (s == "external")
        return Mode::external;
    if (s == "off")
        return Mode::off;
    return Mode::ask;
}
struct Space {
    uint64_t free = 0, capacity = 0;
    bool known = false;
    bool fits(uint64_t bytes) const { return known && bytes <= free && free - bytes >= margin; }
};
inline std::wstring gigabytes(uint64_t bytes, bool pt) {
    std::wostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(1) << double(bytes) / 1e9;
    auto s = out.str();
    if (s.ends_with(L".0"))
        s.resize(s.size() - 2);
    if (pt)
        std::replace(s.begin(), s.end(), L'.', L',');
    return s + L" GB";
}
inline std::optional<unsigned> title_version(std::string_view xml) {
    auto start = xml.find("<title_version");
    if (start == xml.npos)
        return {};
    start = xml.find('>', start);
    if (start == xml.npos)
        return {};
    auto end = xml.find('<', ++start);
    if (end == xml.npos)
        return {};
    auto value = xml.substr(start, end - start);
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\n' || value.front() == '\r' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\n' || value.back() == '\r' || value.back() == '\t'))
        value.remove_suffix(1);
    if (value.starts_with("0x") || value.starts_with("0X"))
        value.remove_prefix(2);
    unsigned n = 0;
    auto result = std::from_chars(value.data(), value.data() + value.size(), n, 16);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || n > 65535)
        return {};
    return n;
}
inline std::optional<uint64_t> title_id(std::string_view xml) {
    auto start = xml.find("<title_id");
    if (start == xml.npos)
        return {};
    start = xml.find('>', start);
    if (start == xml.npos)
        return {};
    auto end = xml.find('<', ++start);
    if (end == xml.npos)
        return {};
    auto value = xml.substr(start, end - start);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    if (value.starts_with("0x"))
        value.remove_prefix(2);
    if (value.size() != 16)
        return {};
    uint64_t id = 0;
    auto result = std::from_chars(value.data(), value.data() + value.size(), id, 16);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
        return {};
    return id;
}
struct State {
    Step step = Step::welcome;
    unsigned page = 0;
    bool verified = false, drive_removed = false;
    Mode mode = Mode::ask;
    void next() {
        if (step == Step::welcome)
            step = Step::guide;
        else if (step == Step::guide) {
            if (page < 3)
                ++page;
            else
                step = Step::detect;
        } else if (step == Step::storage && verified)
            step = Step::automatic;
        else if (step == Step::automatic && verified)
            step = Step::home;
    }
    void back() {
        if (step == Step::guide) {
            if (page)
                --page;
            else
                step = Step::welcome;
        } else if (step == Step::detect) {
            step = Step::guide;
            page = 3;
        } else if (step == Step::verify || step == Step::wrong_version || step == Step::storage) {
            step = Step::detect;
            verified = false;
        } else if (step == Step::automatic)
            step = Step::storage;
    }
    void found() {
        verified = false;
        drive_removed = false;
        step = Step::verify;
    }
    void hash(std::string_view value) {
        verified = value == expected_rpx;
        step = verified ? Step::storage : Step::wrong_version;
    }
    void removed() {
        verified = false;
        drive_removed = true;
        step = Step::detect;
    }
    int storage_focus(Space internal, uint64_t size) const {
        return internal.fits(size) && mode == Mode::copy ? 1 : 0;
    }
};
enum class Format { folder, wua, wud, wux, nus, zip, seven_zip };
inline std::optional<Format> file_format(std::string extension) {
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (extension == ".wua")
        return Format::wua;
    if (extension == ".wud")
        return Format::wud;
    if (extension == ".wux")
        return Format::wux;
    if (extension == ".zip")
        return Format::zip;
    if (extension == ".7z")
        return Format::seven_zip;
    return {};
}
struct Found {
    std::filesystem::path path;
    Format format;
};
// Fixture/desktop discovery. Xbox uses the same depth and predicates with brokered StorageFolder.
inline std::vector<Found> detect(const std::filesystem::path &root) {
    namespace fs = std::filesystem;
    std::vector<Found> result;
    std::function<void(const fs::path &, unsigned)> visit = [&](const auto &p, unsigned depth) {
        std::error_code ec;
        if (fs::is_regular_file(p / "code/U-King.rpx", ec) && fs::is_directory(p / "content", ec) &&
            fs::is_directory(p / "meta", ec)) {
            result.push_back({p, Format::folder});
            return;
        }
        if (fs::is_regular_file(p / "title.tmd", ec) && fs::is_regular_file(p / "title.tik", ec)) {
            result.push_back({p, Format::nus});
            return;
        }
        for (auto &entry : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
            if (entry.is_symlink(ec))
                continue;
            if (entry.is_directory(ec)) {
                if (depth < 2)
                    visit(entry.path(), depth + 1);
            } else if (entry.is_regular_file(ec))
                if (auto format = file_format(entry.path().extension().string()))
                    result.push_back({entry.path(), *format});
        }
    };
    visit(root, 0);
    return result;
}
// Edge plus directional repeat; A/B must never repeat across a screen transition.
struct Repeat {
    uint32_t held = 0;
    double next = 0;
    uint32_t update(uint32_t buttons, uint32_t directions, double now) {
        uint32_t pressed = buttons & ~held;
        if ((buttons & directions) != (held & directions))
            next = now + .4;
        else if ((buttons & directions) && now >= next) {
            pressed |= buttons & directions;
            next = now + .12;
        }
        held = buttons;
        return pressed;
    }
};
} // namespace uwp::setup
