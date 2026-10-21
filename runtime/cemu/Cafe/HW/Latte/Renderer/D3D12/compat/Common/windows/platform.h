// BotWRecomp: Windows platform definitions required by the vendored GPU PCH.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

inline uint32_t GetExceptionError()
{
	return static_cast<uint32_t>(GetLastError());
}

#include <shared_mutex>

class SlimRWLock {
  public:
    void LockRead() { mutex_.lock_shared(); }
    void UnlockRead() { mutex_.unlock_shared(); }
    void LockWrite() { mutex_.lock(); }
    void UnlockWrite() { mutex_.unlock(); }

  private:
    std::shared_mutex mutex_;
};
