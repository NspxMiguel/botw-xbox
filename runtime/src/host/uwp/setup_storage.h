// SPDX-License-Identifier: MIT
#pragma once
#include "../../log.h"
#include "diagnostics.h"
#include "formats/layout.h"
#include "setup.h"
#include <atomic>
#include <cstdio>
#include <fstream>
#include <memory>
#include <windows.h>
// windowsstoragecom.h drags in desktop-only shell headers under the app partition, so declare the one
// app-callable interface used here (values and layout from the 10.0.26100 SDK header).
enum HANDLE_OPTIONS : unsigned { HO_NONE = 0 };
enum HANDLE_ACCESS_OPTIONS : unsigned { HAO_READ = 0x120089 };
enum HANDLE_SHARING_OPTIONS : unsigned { HSO_SHARE_READ = 0x1 };
enum HANDLE_CREATION_OPTIONS : unsigned { HCO_OPEN_EXISTING = 0x3 };
struct IOplockBreakingHandler;
struct __declspec(uuid("DF19938F-5462-48A0-BE65-D2A3271A08D6")) IStorageFolderHandleAccess : ::IUnknown {
    virtual HRESULT __stdcall Create(LPCWSTR fileName, HANDLE_CREATION_OPTIONS creationOptions,
                                     HANDLE_ACCESS_OPTIONS accessOptions,
                                     HANDLE_SHARING_OPTIONS sharingOptions, HANDLE_OPTIONS options,
                                     IOplockBreakingHandler *oplockBreakingHandler,
                                     HANDLE *interopHandle) = 0;
};
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.Storage.Search.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
namespace uwp::setup {
using Folder = winrt::Windows::Storage::StorageFolder;
using File = winrt::Windows::Storage::StorageFile;
using namespace formats;
struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("Cancelled") {};
};
struct Progress {
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> done{0}, total{0};
    void check() const {
        if (cancel)
            throw Cancelled();
    }
};
inline Reader open_reader(File file) {
    uint64_t size = file.GetBasicPropertiesAsync().get().Size();
    // Real broker-granted handles avoid an RPC for every megabyte of a disc image.
    HANDLE handle = INVALID_HANDLE_VALUE;
    try {
        auto parent = file.GetParentAsync().get();
        winrt::com_ptr<::IStorageFolderHandleAccess> access;
        if (parent)
            access = parent.try_as<::IStorageFolderHandleAccess>();
        if (access)
            access->Create(file.Name().c_str(), HCO_OPEN_EXISTING, HAO_READ, HSO_SHARE_READ, HO_NONE, nullptr,
                           &handle);
    } catch (winrt::hresult_error const &) {
    }
    if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
        auto owned = std::shared_ptr<void>(handle, [](void *h) { CloseHandle(h); });
        return {size, [owned, size](uint64_t offset, std::span<uint8_t> out) {
                    require(offset <= size && out.size() <= size - offset, "Read outside file");
                    LARGE_INTEGER pos;
                    pos.QuadPart = LONGLONG(offset);
                    require(SetFilePointerEx(owned.get(), pos, nullptr, FILE_BEGIN) != 0, "File seek failed");
                    while (!out.empty()) {
                        DWORD n = 0;
                        DWORD count = DWORD(std::min<size_t>(out.size(), 1024 * 1024));
                        require(ReadFile(owned.get(), out.data(), count, &n, nullptr) != 0 && n == count,
                                "Drive removed or short read");
                        out = out.subspan(n);
                    }
                }};
    }
    auto stream = file.OpenReadAsync().get();
    return {size, [stream, size](uint64_t offset, std::span<uint8_t> out) {
                require(offset <= size && out.size() <= size - offset, "Read outside file");
                winrt::Windows::Storage::Streams::DataReader reader(stream.GetInputStreamAt(offset));
                require(out.size() <= UINT32_MAX &&
                            reader.LoadAsync(uint32_t(out.size())).get() == out.size(),
                        "Drive removed or short read");
                reader.ReadBytes(winrt::array_view<uint8_t>(out.data(), out.data() + out.size()));
            }};
}
inline std::string hash_file(File file, Progress &progress) {
    auto reader = open_reader(file);
    progress.total = reader.size;
    progress.done = 0;
    packs::Sha256 hash;
    stream_range(reader, 0, reader.size, [&](auto bytes) {
        progress.check();
        hash.update(bytes.data(), bytes.size());
        progress.done += bytes.size();
    });
    return hash.finish();
}
inline Bytes small_file(Folder root, const wchar_t *name, size_t limit = 1024 * 1024) {
    auto reader = open_reader(root.GetFileAsync(name).get());
    require(reader.size <= limit, "Metadata too large");
    return reader.bytes(0, size_t(reader.size));
}
inline std::string read_text(Folder root, const wchar_t *name) {
    try {
        auto bytes = small_file(root, name);
        size_t offset = bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf ? 3 : 0;
        return {bytes.begin() + offset, bytes.end()};
    } catch (winrt::hresult_error const &) {
        return {};
    }
}
inline void write_text(Folder root, const wchar_t *name, std::string_view text) {
    auto file =
        root.CreateFileAsync(name, winrt::Windows::Storage::CreationCollisionOption::ReplaceExisting).get();
    auto stream = file.OpenAsync(winrt::Windows::Storage::FileAccessMode::ReadWrite).get();
    stream.Size(0);
    winrt::Windows::Storage::Streams::DataWriter writer(stream);
    // Persist raw UTF-8, without an encoding marker in mode tokens or relative folder names.
    auto bytes = reinterpret_cast<const uint8_t *>(text.data());
    writer.WriteBytes(winrt::array_view<const uint8_t>(bytes, bytes + text.size()));
    require(writer.StoreAsync().get() == text.size(), "Short settings write");
    require(writer.FlushAsync().get(), "Settings flush failed");
    writer.DetachStream();
    stream.Close();
}
inline Space space(Folder root) {
    Space result;
    ULARGE_INTEGER free{}, capacity{}, unused{};
    if (GetDiskFreeSpaceExW(root.Path().c_str(), &free, &capacity, &unused))
        return {free.QuadPart, capacity.QuadPart, true};
    try {
        std::vector<winrt::hstring> keys{L"System.FreeSpace", L"System.Capacity"};
        auto values =
            root.Properties()
                .RetrievePropertiesAsync(winrt::single_threaded_vector<winrt::hstring>({keys[0], keys[1]}))
                .get();
        if (values.HasKey(keys[0]) && values.HasKey(keys[1])) {
            result = {winrt::unbox_value<uint64_t>(values.Lookup(keys[0])),
                      winrt::unbox_value<uint64_t>(values.Lookup(keys[1])), true};
        }
    } catch (winrt::hresult_error const &) {
    }
    return result;
}
struct FolderLayer {
    Folder folder{nullptr};
    std::string prefix;
};
struct Item {
    Folder folder{nullptr}, drive{nullptr};
    File file{nullptr};
    Format format = Format::folder;
    std::wstring name;
    std::vector<FolderLayer> layers{};
};
inline bool game_folder(Folder root) {
    try {
        root.GetFileAsync(L"code\\U-King.rpx").get();
        root.GetFolderAsync(L"content").get();
        root.GetFolderAsync(L"meta").get();
        return true;
    } catch (winrt::hresult_error const &) {
        return false;
    }
}
// Probe common locations directly as well as enumerating: a broker can expose a
// known child even when listing its parent fails. Depth remains relative to the drive.
template <typename Visit> inline void search_removable(Visit visit, LogMemo *memo = nullptr) {
    try {
        auto drives = winrt::Windows::Storage::KnownFolders::RemovableDevices().GetFoldersAsync().get();
        if (memo) {
            std::string list;
            for (auto drive : drives)
                list += winrt::to_string(drive.Path()) + "|";
            memo->drive_list(list);
        }
        for (auto drive : drives) {
            auto drive_path = winrt::to_string(drive.Path());
            if (!memo || memo->changed("drive " + drive_path, ""))
                LOG("Setup search drive: %s", drive_path.c_str());
            for (auto path : {L"NXbox\\games", L"NXbox", L"games", L"wiiu", L"BotW"}) {
                try {
                    visit(drive.GetFolderAsync(path).get(), drive,
                          std::wstring_view(path) == L"NXbox\\games" ? 2u : 1u);
                } catch (winrt::hresult_error const &) {
                }
            }
            try {
                for (auto folder : drive.GetFoldersAsync().get())
                    if (game_name(winrt::to_string(folder.Name())))
                        visit(folder, drive, 1);
            } catch (winrt::hresult_error const &) {
            }
            visit(drive, drive, 0);
        }
    } catch (winrt::hresult_error const &e) {
        char code[16];
        snprintf(code, sizeof code, "0x%08x", unsigned(e.code().value));
        if (!memo || memo->changed("drive enumeration", code))
            LOG("Setup drive enumeration failed: %s", code);
    }
}
inline Folder remembered_game(LogMemo *memo = nullptr) {
    try {
        auto access = winrt::Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList();
        if (access.ContainsItem(L"game-folder"))
            return access.GetFolderAsync(L"game-folder").get();
    } catch (winrt::hresult_error const &) {
        if (!memo || memo->changed("remembered", ""))
            LOG("Setup remembered folder is unavailable");
    }
    return nullptr;
}
inline std::vector<Item> scan(Folder local, Folder remembered = nullptr, LogMemo *memo = nullptr) {
    std::vector<Item> result;
    struct Title {
        Folder folder{nullptr}, drive{nullptr};
        uint64_t id = 0;
        unsigned version = 0;
        Format format = Format::folder;
    };
    std::vector<Title> titles;
    try {
        auto game = local.GetFolderAsync(L"game").get();
        if (game_folder(game))
            result.push_back({game, local, nullptr, Format::folder, std::wstring(game.Name())});
    } catch (winrt::hresult_error const &) {
    }
    std::set<std::wstring> visited;
    std::function<void(Folder, Folder, unsigned)> walk = [&](Folder root, Folder drive, unsigned depth) {
        if (!visited.insert(std::wstring(root.Path())).second)
            return;
        try {
            if (auto id = title_id(read_text(root, L"code\\app.xml")))
                titles.push_back(
                    {root, drive, *id, title_version(read_text(root, L"code\\app.xml")).value_or(0)});
            if ((std::wstring_view(root.Name()).starts_with(L"BotW-extracted-") ||
                 root.TryGetItemAsync(L"setup-source.txt").get()) &&
                !root.TryGetItemAsync(L"setup-complete.txt").get())
                return;
            if (game_folder(root)) {
                result.push_back({root, drive, nullptr, Format::folder, std::wstring(root.Name())});
                return;
            }
            if (root.TryGetItemAsync(L"title.tmd").get() && root.TryGetItemAsync(L"title.tik").get()) {
                auto ticket = small_file(root, L"title.tik"),
                     tmd = small_file(root, L"title.tmd", 8 * 1024 * 1024);
                titles.push_back(
                    {root, drive, integer(ticket, 0x1dc, 8), unsigned(integer(tmd, 0x1dc, 2)), Format::nus});
                result.push_back({root, drive, nullptr, Format::nus, std::wstring(root.Name())});
                return;
            }
            for (auto file : root.GetFilesAsync().get())
                if (auto format = file_format(winrt::to_string(file.FileType())))
                    result.push_back({root, drive, file, *format, std::wstring(file.Name())});
            // Depth 3 reaches X:\NXbox\games\BotW, where the NXbox FTP server stores uploads.
            if (depth < 3)
                for (auto folder : root.GetFoldersAsync().get())
                    walk(folder, drive, depth + 1);
        } catch (winrt::hresult_error const &) { /* A drive can disappear between any two calls. */
        } catch (std::runtime_error const &) {   /* Malformed metadata is not a game candidate. */
        }
    };
    // Prefer the picker object over the same path exposed by removableStorage:
    // its grant can expose files filtered out of drive enumeration.
    if (!result.empty())
        visited.insert(std::wstring(result.front().folder.Path()));
    if (!remembered)
        remembered = remembered_game(memo);
    if (remembered)
        walk(remembered, remembered, 0);
    search_removable(walk, memo);
    // Pair split Loadiine/Dumpling folders by title family, never by translated display name.
    for (auto &item : result) {
        if ((item.format != Format::folder && item.format != Format::nus) ||
            item.drive.Path() == local.Path() ||
            std::wstring_view(item.folder.Name()).starts_with(L"BotW-extracted-"))
            continue;
        std::optional<uint64_t> id;
        for (auto &title : titles)
            if (title.folder.Path() == item.folder.Path())
                id = title.id;
        if (!id || (*id >> 32) != 0x00050000)
            continue;
        std::optional<Title> update, dlc;
        for (auto &title : titles) {
            if (title.format != item.format || title.drive.Path() != item.drive.Path() ||
                uint32_t(title.id) != uint32_t(*id) ||
                std::wstring_view(title.folder.Name()).starts_with(L"BotW-extracted-"))
                continue;
            if ((title.id >> 32) == 0x0005000e && (!update || title.version > update->version))
                update = title;
            if ((title.id >> 32) == 0x0005000c)
                dlc = title;
        }
        if (update || dlc) {
            item.layers.push_back({item.folder, "base/"});
            if (update)
                item.layers.push_back({update->folder, "update/"});
            if (dlc)
                item.layers.push_back({dlc->folder, "dlc/"});
        }
    }
    std::set<std::wstring> update_paths;
    for (auto &base : result)
        for (auto &layer : base.layers)
            if (layer.prefix == "update/")
                update_paths.insert(std::wstring(layer.folder.Path()));
    result.erase(
        std::remove_if(result.begin(), result.end(),
                       [&](auto &item) { return update_paths.contains(std::wstring(item.folder.Path())); }),
        result.end());
    return result;
}
inline Probe storage_probe(Folder root) {
    auto native = [](std::string path) {
        std::replace(path.begin(), path.end(), '/', '\\');
        return winrt::to_hstring(path);
    };
    return {[root, native](const std::string &s) -> std::optional<uint64_t> {
                try {
                    return root.GetFileAsync(native(s)).get().GetBasicPropertiesAsync().get().Size();
                } catch (...) {
                    return {};
                }
            },
            [root, native](const std::string &s) {
                try {
                    return bool(root.GetFolderAsync(native(s)).get());
                } catch (...) {
                    return false;
                }
            },
            [root, native](const std::string &s) {
                try {
                    return root.GetFolderAsync(native(s)).get().GetItemsAsync(0, 1).get().Size() != 0;
                } catch (...) {
                    return false;
                }
            },
            [root, native](const std::string &s) { return open_reader(root.GetFileAsync(native(s)).get()); }};
}
inline void log_diagnosis(const Diagnosis &d, LogMemo *memo = nullptr) {
    auto issue =
        std::find_if(d.issues.begin(), d.issues.end(), [](auto &i) { return i.kind != IssueKind::optional; });
    auto key = d.folder + (d.source.empty() ? "" : "/") + d.source;
    auto verdict = d.incomplete() ? "incomplete; first issue: " + issue->path : std::string("valid");
    if (!memo || memo->changed(key, verdict))
        LOG("Setup candidate %s: %s", key.c_str(), verdict.c_str());
}
// Log a candidate verdict (e.g. "ignored") only when it is new or differs from the previous scan.
inline void log_verdict(LogMemo *memo, const std::string &key, const std::string &verdict) {
    if (!memo || memo->changed(key, verdict))
        LOG("Setup candidate %s: %s", key.c_str(), verdict.c_str());
}
inline std::vector<Diagnosis> scan_diagnostics(Folder local, DiagnosticCache &cache,
                                               Folder remembered = nullptr, LogMemo *memo = nullptr) {
    std::vector<Diagnosis> result;
    cache.begin();
    std::set<std::wstring> visited;
    std::function<void(Folder, unsigned)> walk = [&](Folder root, unsigned depth) {
        if (!visited.insert(std::wstring(root.Path())).second)
            return;
        try {
            if (root.TryGetItemAsync(L"setup-source.txt").get() &&
                !root.TryGetItemAsync(L"setup-complete.txt").get())
                return;
            auto p = storage_probe(root);
            bool nus = p.file("title.tmd").has_value() && looks_nus(p, winrt::to_string(root.Name()));
            bool unpacked = looks_unpacked(p, winrt::to_string(root.Name()));
            auto path = winrt::to_string(root.Path());
            if (nus || unpacked || game_folder(root)) {
                auto d = diagnose(p, path, nus ? Format::nus : Format::folder);
                if (nus || unpacked || !d.incomplete()) {
                    log_diagnosis(d, memo);
                    result.push_back(std::move(d));
                } else
                    log_verdict(memo, path, "ignored");
            } else
                log_verdict(memo, path, "ignored");
            for (auto file : root.GetFilesAsync().get())
                if (auto format = file_format(winrt::to_string(file.FileType()))) {
                    auto properties = file.GetBasicPropertiesAsync().get();
                    if (auto d = cache.get(p, winrt::to_string(root.Path()), *format,
                                           winrt::to_string(file.Name()), properties.Size(),
                                           properties.DateModified().time_since_epoch().count())) {
                        log_diagnosis(*d, memo);
                        result.push_back(std::move(*d));
                    } else
                        log_verdict(memo, path + "/" + winrt::to_string(file.Name()), "ignored");
                }
            if (!nus && depth < 3)
                for (auto folder : root.GetFoldersAsync().get()) {
                    auto name = folder.Name();
                    if (!unpacked ||
                        (name != L"code" && name != L"content" && name != L"meta" && name != L"aoc"))
                        walk(folder, depth + 1);
                }
        } catch (...) { /* A hot-unplug is retried on the next scan. */
            log_verdict(memo, winrt::to_string(root.Path()), "ignored (unavailable)");
        }
    };
    try {
        auto game = local.GetFolderAsync(L"game").get();
        walk(game, 0);
    } catch (...) {
    }
    if (!remembered)
        remembered = remembered_game(memo);
    if (remembered)
        walk(remembered, 0);
    search_removable([&](Folder root, Folder, unsigned depth) { walk(root, depth); }, memo);
    cache.end();
    return result;
}
inline uint64_t tree_size(Folder root, Progress &progress, unsigned depth = 0) {
    progress.check();
    require(depth < 64, "Folder nesting too deep");
    uint64_t size = 0;
    for (auto file : root.GetFilesAsync().get()) {
        progress.check();
        auto n = file.GetBasicPropertiesAsync().get().Size();
        require(n <= UINT64_MAX - size, "Folder size overflow");
        size += n;
    }
    for (auto folder : root.GetFoldersAsync().get()) {
        auto n = tree_size(folder, progress, depth + 1);
        require(n <= UINT64_MAX - size, "Folder size overflow");
        size += n;
    }
    return size;
}
inline void folder_entries(Folder root, std::string prefix, std::vector<Entry> &entries, Progress &progress,
                           unsigned depth = 0) {
    progress.check();
    require(depth < 64 && entries.size() < 200000, "Folder too large");
    for (auto file : root.GetFilesAsync().get()) {
        auto name = prefix + winrt::to_string(file.Name());
        auto size = file.GetBasicPropertiesAsync().get().Size();
        entries.push_back({name, size, [file, size](const Sink &sink) {
                               auto reader = open_reader(file);
                               stream_range(reader, 0, size, sink);
                           }});
    }
    for (auto folder : root.GetFoldersAsync().get())
        folder_entries(folder, prefix + winrt::to_string(folder.Name()) + "/", entries, progress, depth + 1);
}
inline std::vector<Entry> source_entries(const Item &item, Progress &progress) {
    progress.check();
    std::vector<Entry> entries;
    auto progress_reader = [&](File file) {
        auto reader = open_reader(file);
        return Reader{reader.size, [reader, &progress](uint64_t offset, std::span<uint8_t> out) {
                          progress.check();
                          reader.read(offset, out);
                      }};
    };
    if (item.format == Format::folder) {
        if (item.layers.empty()) {
            folder_entries(item.folder, "", entries, progress);
            return entries;
        }
        for (auto &layer : item.layers)
            folder_entries(layer.folder, layer.prefix, entries, progress);
        return merge_titles(std::move(entries));
    }
    if (item.format == Format::wua)
        return merge_titles(wua(progress_reader(item.file), [&progress](uint64_t done, uint64_t total) {
            progress.check();
            progress.done = done;
            progress.total = total;
        }));
    if (item.format == Format::zip)
        return merge_titles(zip(progress_reader(item.file)));
    if (item.format == Format::seven_zip)
        throw std::runtime_error("Extract .7z on a PC");
    std::optional<Key> common;
    std::vector<Key> keys;
    // Keys never leave their source drive or enter logs/settings.
    std::function<void(Folder, unsigned)> search = [&](Folder folder, unsigned depth) {
        for (auto file : folder.GetFilesAsync().get()) {
            progress.check();
            auto name = winrt::to_string(file.Name());
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (name != "otp.bin" && name != "common.key" && name != "keys.txt" && !name.ends_with(".key"))
                continue;
            auto reader = open_reader(file);
            if (reader.size > 1024 * 1024)
                continue;
            auto bytes = reader.bytes(0, size_t(reader.size));
            if (name == "otp.bin" || name == "common.key") {
                if (auto parsed = parse_common(bytes))
                    common = parsed;
            } else {
                auto parsed = parse_keys(bytes, name != "keys.txt");
                keys.insert(keys.end(), parsed.begin(), parsed.end());
            }
        }
        if (depth < 3)
            for (auto child : folder.GetFoldersAsync().get())
                search(child, depth + 1);
    };
    search(item.drive, 0);
    require(common.has_value(), "Missing or invalid otp.bin / common.key");
    if (item.format == Format::nus) {
        auto decrypt = [&](Folder folder) {
            return nus(small_file(folder, L"title.tmd", 8 * 1024 * 1024), small_file(folder, L"title.tik"),
                       *common, [folder](const std::string &name) {
                           return open_reader(folder.GetFileAsync(winrt::to_hstring(name)).get());
                       });
        };
        if (item.layers.empty())
            return merge_titles(decrypt(item.folder));
        for (auto &layer : item.layers) {
            auto title = decrypt(layer.folder);
            for (auto &entry : title) {
                entry.path = layer.prefix + entry.path;
                entries.push_back(std::move(entry));
            }
        }
        return merge_titles(std::move(entries));
    }
    require(!keys.empty(), "Missing disc key: game.key / keys.txt");
    auto reader = progress_reader(item.file);
    for (auto key : keys) {
        progress.check();
        try {
            return merge_titles(wud(reader, key, *common));
        } catch (std::runtime_error const &error) {
            if (std::string_view(error.what()) != "Wrong disc key")
                throw;
        }
    }
    throw std::runtime_error("No disc key decrypted this image");
}
inline std::string source_identity(const Item &item, Progress &progress) {
    packs::Sha256 hash;
    auto add = [&](const std::string &text) {
        hash.update(reinterpret_cast<const uint8_t *>(text.data()), text.size());
    };
    add(winrt::to_string(item.drive.Name()));
    add(std::to_string(int(item.format)));
    auto file = [&](File value, const std::string &prefix) {
        progress.check();
        auto properties = value.GetBasicPropertiesAsync().get();
        add(prefix + winrt::to_string(value.Name()) + ":" + std::to_string(properties.Size()) + ":" +
            std::to_string(properties.DateModified().time_since_epoch().count()) + "\n");
    };
    std::function<void(Folder, std::string, unsigned)> walk = [&](Folder root, std::string prefix,
                                                                  unsigned depth) {
        require(depth < 64, "Folder too deep");
        std::vector<File> files;
        for (auto value : root.GetFilesAsync().get())
            files.push_back(value);
        std::sort(files.begin(), files.end(), [](const File &a, const File &b) {
            return std::wstring_view(a.Name()) < std::wstring_view(b.Name());
        });
        for (auto value : files)
            file(value, prefix);
        std::vector<Folder> folders;
        for (auto child : root.GetFoldersAsync().get())
            folders.push_back(child);
        std::sort(folders.begin(), folders.end(), [](const Folder &a, const Folder &b) {
            return std::wstring_view(a.Name()) < std::wstring_view(b.Name());
        });
        for (auto child : folders)
            walk(child, prefix + winrt::to_string(child.Name()) + "/", depth + 1);
    };
    if (item.file)
        file(item.file, "");
    else if (item.layers.empty())
        walk(item.folder, winrt::to_string(item.folder.Name()) + "/", 0);
    else
        for (auto &layer : item.layers)
            walk(layer.folder, layer.prefix, 0);
    return hash.finish().substr(0, 16);
}
inline Folder extraction_folder(const Item &item, Folder local, const std::string &identity) {
    auto location_name = winrt::to_hstring("extract-location-" + identity + ".txt");
    auto saved = read_text(local, location_name.c_str());
    if (!saved.empty()) {
        require(safe_path(saved) && saved.find('/') == saved.npos, "Invalid extraction location");
        Folder folder{nullptr};
        try {
            folder = item.drive.GetFolderAsync(winrt::to_hstring(saved)).get();
        } catch (winrt::hresult_error const &) {
        }
        if (folder) {
            require(read_text(folder, L"setup-source.txt") == identity,
                    "Extraction folder ownership changed");
            return folder;
        }
    }
    Folder folder{nullptr};
    try {
        folder =
            item.drive
                .CreateFolderAsync(L"BotW", winrt::Windows::Storage::CreationCollisionOption::FailIfExists)
                .get();
    } catch (winrt::hresult_error const &) {
        folder = item.drive
                     .CreateFolderAsync(winrt::to_hstring("BotW-extracted-" + identity),
                                        winrt::Windows::Storage::CreationCollisionOption::GenerateUniqueName)
                     .get();
    }
    write_text(folder, L"setup-source.txt", identity);
    write_text(local, location_name.c_str(), winrt::to_string(folder.Name()));
    return folder;
}
inline Folder ensure_parent(Folder root, std::string_view path) {
    auto end = path.find_last_of('/');
    if (end == path.npos)
        return root;
    auto parent = path.substr(0, end);
    while (!parent.empty()) {
        auto slash = parent.find('/');
        auto part = parent.substr(0, slash);
        root = root.CreateFolderAsync(winrt::to_hstring(part),
                                      winrt::Windows::Storage::CreationCollisionOption::OpenIfExists)
                   .get();
        if (slash == parent.npos)
            break;
        parent.remove_prefix(slash + 1);
    }
    return root;
}
inline std::string basename(std::string_view path) {
    auto slash = path.find_last_of('/');
    return std::string(slash == path.npos ? path : path.substr(slash + 1));
}
// Write final names through the broker: removable rename and USB temp files are not reliable.
// Resume trusts a completed entry only after rehashing it against its LocalState journal.
inline void extract(const std::vector<Entry> &entries, Folder destination, Folder local,
                    const std::wstring &journal_name, Progress &progress, bool resumable) {
    validate_entries(entries);
    progress.done = 0;
    uint64_t total = 0;
    for (auto &entry : entries)
        total += entry.size;
    progress.total = total;
    std::map<std::string, std::string> completed;
    if (resumable) {
        auto path = std::filesystem::path(local.Path().c_str()) / journal_name;
        std::error_code error;
        auto size = std::filesystem::file_size(path, error);
        require(error || size <= 64 * 1024 * 1024, "Extraction journal too large");
        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line)) {
            std::istringstream row(line);
            std::string name, digest;
            if (!(row >> std::quoted(name) >> digest) || !safe_path(name) || digest.size() != 64)
                continue;
            require(completed.size() <= 200000, "Too many journal entries");
            completed[name] = digest;
        }
    }
    std::ofstream journal;
    if (resumable) {
        auto path = std::filesystem::path(local.Path().c_str()) / journal_name;
        journal.open(path, std::ios::app);
        require(bool(journal), "Unable to open extraction journal");
        // A terminated line also isolates a torn final record after power loss.
        journal << '\n';
    }
    for (auto &entry : entries) {
        progress.check();
        auto parent = ensure_parent(destination, entry.path);
        auto name = winrt::to_hstring(basename(entry.path));
        if (completed.contains(entry.path)) {
            try {
                auto file = parent.GetFileAsync(name).get();
                auto reader = open_reader(file);
                packs::Sha256 hash;
                if (reader.size == entry.size) {
                    stream_range(reader, 0, reader.size, [&](auto bytes) {
                        progress.check();
                        hash.update(bytes.data(), bytes.size());
                    });
                    if (hash.finish() == completed[entry.path]) {
                        progress.done += entry.size;
                        continue;
                    }
                }
            } catch (winrt::hresult_error const &) {
            }
        }
        auto file =
            parent.CreateFileAsync(name, winrt::Windows::Storage::CreationCollisionOption::ReplaceExisting)
                .get();
        auto stream = file.OpenAsync(winrt::Windows::Storage::FileAccessMode::ReadWrite).get();
        stream.Size(0);
        packs::Sha256 hash;
        uint64_t written = 0;
        entry.extract([&](std::span<const uint8_t> bytes) {
            progress.check();
            require(written <= entry.size && bytes.size() <= entry.size - written, "Extraction overflow");
            winrt::Windows::Storage::Streams::DataWriter writer(stream.GetOutputStreamAt(written));
            writer.WriteBytes(winrt::array_view<const uint8_t>(bytes.data(), bytes.data() + bytes.size()));
            require(writer.StoreAsync().get() == bytes.size(), "Drive removed or short write");
            writer.DetachStream();
            hash.update(bytes.data(), bytes.size());
            written += bytes.size();
            progress.done += bytes.size();
        });
        require(written == entry.size, "Short extraction");
        require(stream.FlushAsync().get(), "Flush failed");
        stream.Close();
        if (resumable) {
            journal << std::quoted(entry.path) << ' ' << hash.finish() << '\n';
            journal.flush();
            require(bool(journal), "Unable to persist extraction journal");
        }
    }
}
} // namespace uwp::setup
