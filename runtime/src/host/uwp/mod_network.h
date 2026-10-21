// SPDX-License-Identifier: MIT
#pragma once
#include "../../mods/gamebanana.h"
#include "../../mods/store.h"
#include "request_deadline.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Cryptography.Core.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>
namespace uwp {
// Cancellation may come from the UI while transport runs on a worker apartment.
struct Transfer {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    winrt::Windows::Foundation::IAsyncInfo pending{nullptr};
    Transfer() = default;
    Transfer &operator=(Transfer &&) {
        cancelled = false;
        set_pending(nullptr);
        return *this;
    }
    void set_pending(winrt::Windows::Foundation::IAsyncInfo operation) {
        std::lock_guard lock(mutex);
        pending = operation;
        if (cancelled && pending) {
            try {
                pending.Cancel();
            } catch (winrt::hresult_error const &) {
            }
        }
    }
    void cancel() {
        cancelled = true;
        std::lock_guard lock(mutex);
        if (pending) {
            try {
                pending.Cancel();
            } catch (winrt::hresult_error const &) {
            }
        }
    }
    void check() const {
        if (cancelled)
            throw winrt::hresult_canceled();
    }
};
using StoreRequests = RequestGroup<Transfer>;
inline winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> fetch_json(std::string url,
                                                                              Transfer &transfer) {
    using namespace winrt;
    using namespace winrt::Windows::Web::Http;
    using namespace winrt::Windows::Storage::Streams;
    transfer.check();
    HttpClient client;
    client.DefaultRequestHeaders().UserAgent().ParseAdd(L"NXbox-ModStore/1.0");
    auto request = client.GetAsync(winrt::Windows::Foundation::Uri(to_hstring(url)),
                                   HttpCompletionOption::ResponseHeadersRead);
    transfer.set_pending(request.as<winrt::Windows::Foundation::IAsyncInfo>());
    auto response = co_await request;
    if (response.StatusCode() != HttpStatusCode::Ok ||
        response.RequestMessage().RequestUri().SchemeName() != L"https")
        throw mods::store::NetworkError("HTTP request failed");
    auto open = response.Content().ReadAsInputStreamAsync();
    transfer.set_pending(open.as<winrt::Windows::Foundation::IAsyncInfo>());
    auto stream = co_await open;
    std::string text;
    for (;;) {
        transfer.check();
        auto read = stream.ReadAsync(Buffer(65536), 65536, InputStreamOptions::Partial);
        transfer.set_pending(read.as<winrt::Windows::Foundation::IAsyncInfo>());
        auto buffer = co_await read;
        if (!buffer.Length())
            break;
        if (buffer.Length() > mods::store::response_byte_limit - text.size())
            throw std::runtime_error("API response too large");
        std::vector<uint8_t> bytes(buffer.Length());
        DataReader::FromBuffer(buffer).ReadBytes(bytes);
        text.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    transfer.set_pending(nullptr);
    co_return to_hstring(text);
}
inline winrt::Windows::Foundation::IAsyncAction
download_mod(mods::OnlineFile file, std::filesystem::path path, Transfer &transfer,
             std::function<void(uint64_t, uint64_t)> progress,
             uint64_t limit = uint64_t(4) * 1024 * 1024 * 1024) {
    using namespace winrt;
    using namespace winrt::Windows::Web::Http;
    using namespace winrt::Windows::Storage::Streams;
    using namespace winrt::Windows::Security::Cryptography;
    using namespace winrt::Windows::Security::Cryptography::Core;
    transfer.check();
    if (!file.url.starts_with("https://"))
        throw std::runtime_error("HTTPS required");
    HttpClient client;
    client.DefaultRequestHeaders().UserAgent().ParseAdd(L"NXbox-ModStore/1.0");
    auto request = client.GetAsync(winrt::Windows::Foundation::Uri(to_hstring(file.url)),
                                   HttpCompletionOption::ResponseHeadersRead);
    transfer.set_pending(request.as<winrt::Windows::Foundation::IAsyncInfo>());
    auto response = co_await request;
    if (response.StatusCode() != HttpStatusCode::Ok ||
        response.RequestMessage().RequestUri().SchemeName() != L"https")
        throw std::runtime_error("HTTP download failed");
    auto length = response.Content().Headers().ContentLength();
    uint64_t expected = length ? length.Value() : 0;
    if (expected > limit || file.size > limit || (file.size && expected && file.size != expected))
        throw std::runtime_error("Invalid download size");
    auto hash = HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Md5()).CreateHash();
    auto open = response.Content().ReadAsInputStreamAsync();
    transfer.set_pending(open.as<winrt::Windows::Foundation::IAsyncInfo>());
    auto stream = co_await open;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("Cannot create download");
    uint64_t size = 0;
    for (;;) {
        transfer.check();
        auto read = stream.ReadAsync(Buffer(65536), 65536, InputStreamOptions::Partial);
        transfer.set_pending(read.as<winrt::Windows::Foundation::IAsyncInfo>());
        auto buffer = co_await read;
        if (!buffer.Length())
            break;
        size += buffer.Length();
        if (size > limit || (expected && size > expected) || (file.size && size > file.size))
            throw std::runtime_error("Download too large");
        hash.Append(buffer);
        std::vector<uint8_t> bytes(buffer.Length());
        DataReader::FromBuffer(buffer).ReadBytes(bytes);
        output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        if (!output)
            throw std::runtime_error("Download write failed");
        progress(size, file.size ? file.size : expected);
    }
    transfer.set_pending(nullptr);
    output.close();
    transfer.check();
    if (!output || !size || (expected && expected != size) || (file.size && file.size != size))
        throw std::runtime_error("Incomplete download");
    if (!file.md5.empty()) {
        auto actual = to_string(CryptographicBuffer::EncodeToHexString(hash.GetValueAndReset()));
        for (auto &c : file.md5)
            if (c >= 'A' && c <= 'F')
                c += 32;
        if (actual != file.md5)
            throw std::runtime_error("Download checksum mismatch");
    }
}
} // namespace uwp
