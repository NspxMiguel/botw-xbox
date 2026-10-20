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
