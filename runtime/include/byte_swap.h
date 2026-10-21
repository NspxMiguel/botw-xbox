#pragma once
#include <stdint.h>
#ifdef _MSC_VER
#include <stdlib.h>
inline uint16_t botw_bswap16(uint16_t v) { return _byteswap_ushort(v); }
inline uint32_t botw_bswap32(uint32_t v) { return _byteswap_ulong(v); }
inline uint64_t botw_bswap64(uint64_t v) { return _byteswap_uint64(v); }
#else
inline uint16_t botw_bswap16(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t botw_bswap32(uint32_t v) { return __builtin_bswap32(v); }
inline uint64_t botw_bswap64(uint64_t v) { return __builtin_bswap64(v); }
#endif
