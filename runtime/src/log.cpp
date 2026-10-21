#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include <mutex>
#ifdef _WIN32
#include <chrono>
#if defined(BOTW_PLATFORM_UWP)
#include <fcntl.h>
#include <io.h>
#include <string>
#include <windows.h>
#include <fileapifromapp.h>
#endif
#endif

int g_log_level = 0;
static FILE* g_log_file = nullptr;
static std::recursive_mutex g_log_lock;  // recursive: the watchdog's stack-dump signal handler logs

void log_set_file(const char* path) {
    std::lock_guard<std::recursive_mutex> lock(g_log_lock);
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = nullptr;
    }
#if defined(BOTW_PLATFORM_UWP)
    // Open LocalState through the app API, including non-ASCII package paths.
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (count > 0) {
        std::wstring wide(size_t(count), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide.data(), count);
        HANDLE handle = CreateFile2FromAppW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            CREATE_ALWAYS, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_WRONLY | _O_TEXT);
            if (fd >= 0) {
                g_log_file = _fdopen(fd, "w");
                if (!g_log_file)
                    _close(fd);
            } else
                CloseHandle(handle);
        }
    }
    if (!g_log_file)
        OutputDebugStringA("BotW: could not open runtime log in LocalState.\n");
#else
    g_log_file = fopen(path, "w");
#endif
    if (!g_log_file)
        fprintf(stderr, "Could not open runtime log: %s\n", path);
}

void log_write(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
#ifdef _WIN32
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    double t = std::chrono::duration<double>(clock::now() - t0).count();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    static double t0 = ts.tv_sec + ts.tv_nsec * 1e-9;
    double t = ts.tv_sec + ts.tv_nsec * 1e-9 - t0;

#endif
    std::lock_guard<std::recursive_mutex> l(g_log_lock);
    fprintf(stderr, "[%8.3f] %s\n", t, buf);
    if (g_log_file) {
        fprintf(g_log_file, "[%8.3f] %s\n", t, buf);
        fflush(g_log_file);
    }
}
