// Host file access for game and save files. On UWP the game can live on a USB drive, which an
// app container only reaches through the *FromApp APIs (the plain CRT/Win32 calls get
// ACCESS_DENIED there); elsewhere these are the usual CRT calls.
#pragma once
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace host_file {
#if defined(_WIN32)
inline std::filesystem::path utf8_path(const std::string &text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
}
#else
inline std::filesystem::path utf8_path(const std::string &text) { return text; }
#endif

#if defined(BOTW_PLATFORM_UWP)
using Stat = struct _stat64;

inline int stat_path(const char *path, Stat *out) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExFromAppW(utf8_path(path).c_str(), GetFileExInfoStandard, &data))
        return -1;
    *out = Stat{};
    out->st_mode = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? _S_IFDIR : _S_IFREG;
    out->st_size = (int64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    // FILETIME (100 ns since 1601) to Unix seconds
    uint64_t t = (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    out->st_mtime = out->st_atime = out->st_ctime = t ? __time64_t(t / 10000000 - 11644473600ull) : 0;
    return 0;
}

// fopen modes: r, w, a, with optional + and b.
inline FILE *open_path(const char *path, const char *mode) {
    std::string m = mode;
    bool plus = m.find('+') != std::string::npos;
    DWORD access = GENERIC_READ, disposition = OPEN_EXISTING;
    int flags = _O_BINARY;
    if (m[0] == 'w')
        access = GENERIC_WRITE, disposition = CREATE_ALWAYS;
    else if (m[0] == 'a')
        access = GENERIC_WRITE, disposition = OPEN_ALWAYS, flags |= _O_APPEND;
    if (plus)
        access = GENERIC_READ | GENERIC_WRITE;
    flags |= plus ? _O_RDWR : m[0] == 'r' ? _O_RDONLY : _O_WRONLY;
    HANDLE h = CreateFile2FromAppW(utf8_path(path).c_str(), access, FILE_SHARE_READ, disposition, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
        return nullptr;
    }
    int fd = _open_osfhandle(intptr_t(h), flags);
    if (fd < 0) {
        CloseHandle(h);
        return nullptr;
    }
    FILE *f = _fdopen(fd, mode);
    if (!f)
        _close(fd);
    return f;
}

// Names in a directory (no "." / ".."); false when it can't be listed.
inline bool list_dir(const std::string &dir, std::vector<std::string> &names) {
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileExFromAppW((utf8_path(dir) / L"*").c_str(), FindExInfoBasic, &data,
                                       FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    do {
        std::wstring w = data.cFileName;
        if (w == L"." || w == L"..")
            continue;
        auto u8 = std::filesystem::path(w).u8string();
        names.emplace_back(u8.begin(), u8.end());
    } while (FindNextFileW(h, &data));
    FindClose(h);
    return true;
}
#else
#if defined(_WIN32)
using Stat = struct _stat64;
inline int stat_path(const char *path, Stat *out) { return _wstat64(utf8_path(path).c_str(), out); }
inline FILE *open_path(const char *path, const char *mode) {
    return _wfopen(utf8_path(path).c_str(), utf8_path(mode).c_str());
}
#else
using Stat = struct ::stat;
inline int stat_path(const char *path, Stat *out) { return ::stat(path, out); }
inline FILE *open_path(const char *path, const char *mode) { return fopen(path, mode); }
#endif
inline bool list_dir(const std::string &dir, std::vector<std::string> &names) {
    std::error_code error;
    for (std::filesystem::directory_iterator it(utf8_path(dir), error), end; !error && it != end; it.increment(error)) {
        names.push_back(it->path().filename().string());
    }
    return !error;
}
#endif
}  // namespace host_file
