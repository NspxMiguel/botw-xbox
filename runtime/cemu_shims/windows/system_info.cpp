#include "util/SystemInfo/SystemInfo.h"
#include <windows.h>

void QueryProcTime(uint64 &now, uint64 &user, uint64 &kernel) {
    FILETIME creation, exit, kernelTime, userTime;
    ULARGE_INTEGER value;
    GetSystemTimeAsFileTime(&creation);
    value.LowPart = creation.dwLowDateTime;
    value.HighPart = creation.dwHighDateTime;
    now = value.QuadPart;
    user = kernel = 0;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernelTime, &userTime)) {
        value.LowPart = userTime.dwLowDateTime;
        value.HighPart = userTime.dwHighDateTime;
        user = value.QuadPart;
        value.LowPart = kernelTime.dwLowDateTime;
        value.HighPart = kernelTime.dwHighDateTime;
        kernel = value.QuadPart;
    }
}
