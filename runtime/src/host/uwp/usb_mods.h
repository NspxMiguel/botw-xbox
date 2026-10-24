// SPDX-License-Identifier: MIT
#pragma once
#include "../../mods/archive_install.h"
#include "../../mods/local.h"
#include "../../mods/store.h"
#include "mod_network.h"
#include <fstream>
#include <set>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.Storage.h>

namespace uwp::usb_mods {
using namespace winrt;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;
struct Candidate {
    mods::store::Card card;
    StorageFolder folder{nullptr};
    StorageFile archive{nullptr};
};
inline StorageFolder root(StorageFolder drive, std::string_view name) {
    if (name.empty())
        return drive;
    auto item = drive.TryGetItemAsync(to_hstring(name)).get();
    return item ? item.try_as<StorageFolder>() : nullptr;
}
inline std::vector<Candidate> scan(Transfer *transfer = nullptr) {
    std::vector<Candidate> result;
    std::set<std::string> seen;
    for (auto drive : KnownFolders::RemovableDevices().GetFoldersAsync().get()) {
        if (transfer)
            transfer->check();
        try {
            for (auto name : mods::local::roots) {
                if (transfer)
                    transfer->check();
                auto parent = root(drive, name);
                if (!parent)
                    continue;
                for (auto item : parent.GetItemsAsync().get()) {
                    if (transfer)
                        transfer->check();
                    auto filename = to_string(item.Name());
                    if (!mods::local::component(filename))
                        continue;
                    Candidate candidate;
                    candidate.folder = item.try_as<StorageFolder>();
                    candidate.archive = item.try_as<StorageFile>();
                    bool content = false, graphics = false;
                    if (candidate.folder)
                        for (auto child : candidate.folder.GetItemsAsync().get()) {
                            auto child_name = to_string(child.Name());
                            content |= child.IsOfType(StorageItemTypes::Folder) &&
                                       mods::local::content_folder(child_name);
                            graphics |= child.IsOfType(StorageItemTypes::File) &&
                                        mods::local::equal_name(child_name, "rules.txt");
                        }
                    if (candidate.folder && !content && !graphics)
                        continue;
                    if (candidate.archive) {
                        auto extension = std::filesystem::path(item.Name().c_str()).extension();
                        auto ext = to_string(extension.wstring());
                        if (!mods::local::equal_name(ext, ".zip") && !mods::local::equal_name(ext, ".7z") &&
                            !mods::local::equal_name(ext, ".rar"))
                            continue;
                    }
                    auto &card = candidate.card;
                    // An opaque session identity, never passed to CRT or persisted as a game path.
                    card.id = to_string(item.Path());
                    if (!seen.insert(card.id).second)
                        continue;
                    card.title = filename;
                    card.source = "USB";
                    card.category =
                        graphics ? mods::store::Category::graphics : mods::store::category(filename);
                    if (candidate.archive) {
                        card.size = candidate.archive.GetBasicPropertiesAsync().get().Size();
                        card.downloads = {{filename, card.id, "", card.size}};
                    }
                    result.push_back(std::move(candidate));
                }
            }
        } catch (hresult_error const &) {
            if (transfer)
                transfer->check();
            // One disconnected/inaccessible drive must not hide the other drives.
        }
    }
    return result;
}
struct File {
    StorageFile source{nullptr};
    std::filesystem::path relative;
    uint64_t size = 0;
};
inline void enumerate(StorageFolder folder, std::filesystem::path relative, std::vector<File> &files,
                      uint64_t &total, Transfer &transfer, unsigned depth = 0) {
    transfer.check();
    if (depth > 64)
        throw std::runtime_error("Mod directory nesting limit");
    auto listing = folder.GetItemsAsync();
    transfer.set_pending(listing.as<winrt::Windows::Foundation::IAsyncInfo>());
    for (auto item : listing.get()) {
        transfer.check();
        auto name = to_string(item.Name());
        if (name.starts_with('.') || (relative.empty() && mods::local::launcher_file(name)))
            continue;
        // The catalog and overlay use these canonical wrapper names on all hosts.
        auto canonical =
            mods::local::import_name(name, relative.empty(), item.IsOfType(StorageItemTypes::Folder));
        auto path = relative / std::filesystem::path(to_hstring(canonical).c_str());
        if (auto child = item.try_as<StorageFolder>()) {
            enumerate(child, path, files, total, transfer, depth + 1);
        } else if (auto file = item.try_as<StorageFile>()) {
            auto size = file.GetBasicPropertiesAsync().get().Size();
            if (files.size() >= 100000 || size > UINT64_MAX - total)
                throw std::runtime_error("Mod file limit");
            files.push_back({file, path, size});
            total += size;
        }
    }
}
// USB reads stay brokered; only the app-owned staging destination uses CRT.
inline void copy(File const &file, std::filesystem::path const &destination, Transfer &transfer,
                 uint64_t &done, uint64_t total, const std::function<void(uint64_t, uint64_t)> &progress) {
    transfer.check();
    auto open = file.source.OpenReadAsync();
    transfer.set_pending(open.as<winrt::Windows::Foundation::IAsyncInfo>());
    auto stream = open.get();
    if (stream.Size() != file.size)
        throw std::runtime_error("USB file changed");
    std::filesystem::create_directories(destination.parent_path());
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    uint64_t copied = 0;
    while (copied < file.size) {
        transfer.check();
        auto count = uint32_t(std::min<uint64_t>(1024 * 1024, file.size - copied));
        auto read = stream.ReadAsync(Buffer(count), count, InputStreamOptions::Partial);
        transfer.set_pending(read.as<winrt::Windows::Foundation::IAsyncInfo>());
        auto buffer = read.get();
        if (!buffer.Length())
            throw std::runtime_error("USB drive removed or short read");
        auto bytes = buffer.data();
        output.write(reinterpret_cast<const char *>(bytes), buffer.Length());
        copied += buffer.Length();
        done += buffer.Length();
        progress(done, total);
    }
    output.close();
    stream.Close();
    transfer.set_pending(nullptr);
    transfer.check();
}
} // namespace uwp::usb_mods
