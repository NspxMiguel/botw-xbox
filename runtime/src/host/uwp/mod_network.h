// SPDX-License-Identifier: MIT
#pragma once
#include "../../mods/gamebanana.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Cryptography.Core.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>
namespace uwp {
// UI-thread owned state. Each awaited operation can be cancelled by the CoreWindow B handler.
struct Transfer {
    bool cancelled = false;
    winrt::Windows::Foundation::IAsyncInfo pending{nullptr};
    void cancel() {
        cancelled = true;
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
inline winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> fetch_json(std::string url,
                                                                              Transfer &transfer) {
    using namespace winrt;
    using namespace winrt::Windows::Web::Http;
    using namespace winrt::Windows::Storage::Streams;
    transfer.check();
    HttpClient client;
    auto request =
        client.GetAsync(winrt::Windows::Foundation::Uri(to_hstring(url)), HttpCompletionOption::ResponseHeadersRead);
    transfer.pending = request.as<winrt::Windows::Foundation::IAsyncInfo>();
    auto response = co_await request;
    if (response.StatusCode() != HttpStatusCode::Ok ||
        response.RequestMessage().RequestUri().SchemeName() != L"https")
        throw std::runtime_error("HTTP request failed");
    auto stream = co_await response.Content().ReadAsInputStreamAsync();
    std::string text;
    for (;;) {
        transfer.check();
        auto read = stream.ReadAsync(Buffer(65536), 65536, InputStreamOptions::Partial);
        transfer.pending = read.as<winrt::Windows::Foundation::IAsyncInfo>();
        auto buffer = co_await read;
        if (!buffer.Length())
            break;
        if (text.size() + buffer.Length() > 8 * 1024 * 1024)
            throw std::runtime_error("API response too large");
        std::vector<uint8_t> bytes(buffer.Length());
        DataReader::FromBuffer(buffer).ReadBytes(bytes);
        text.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    transfer.pending = nullptr;
    co_return to_hstring(text);
}
inline winrt::Windows::Foundation::IAsyncAction
download_mod(mods::OnlineFile file, std::filesystem::path path, Transfer &transfer,
             std::function<void(uint64_t, uint64_t)> progress) {
    using namespace winrt;
    using namespace winrt::Windows::Web::Http;
    using namespace winrt::Windows::Storage::Streams;
    using namespace winrt::Windows::Security::Cryptography;
    using namespace winrt::Windows::Security::Cryptography::Core;
    transfer.check();
    if (!file.url.starts_with("https://"))
        throw std::runtime_error("HTTPS required");
    HttpClient client;
    auto request = client.GetAsync(winrt::Windows::Foundation::Uri(to_hstring(file.url)),
                                   HttpCompletionOption::ResponseHeadersRead);
    transfer.pending = request.as<winrt::Windows::Foundation::IAsyncInfo>();
    auto response = co_await request;
    if (response.StatusCode() != HttpStatusCode::Ok ||
        response.RequestMessage().RequestUri().SchemeName() != L"https")
        throw std::runtime_error("HTTP download failed");
    auto length = response.Content().Headers().ContentLength();
    uint64_t expected = length ? length.Value() : 0;
    constexpr uint64_t limit = uint64_t(4) * 1024 * 1024 * 1024;
    if (expected > limit || file.size > limit || (file.size && expected && file.size != expected))
        throw std::runtime_error("Invalid download size");
    auto hash = HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Md5()).CreateHash();
    auto stream = co_await response.Content().ReadAsInputStreamAsync();
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("Cannot create download");
    uint64_t size = 0;
    for (;;) {
        transfer.check();
        auto read = stream.ReadAsync(Buffer(65536), 65536, InputStreamOptions::Partial);
        transfer.pending = read.as<winrt::Windows::Foundation::IAsyncInfo>();
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
    transfer.pending = nullptr;
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
