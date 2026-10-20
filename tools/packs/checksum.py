"""Cemu moduleMatches CRC, computed before relocation from the player's RPX.

Format reference: Cemu RPLLoader_BeginCemuhookCRC (see README). Metadata is
hashed in little endian, as on Cemu hosts; section payloads remain big endian.
"""

import struct
import zlib

from .rules import PackError


def module_checksum(raw: bytes) -> int:
    if raw[:6] != b"\x7fELF\x01\x02" or len(raw) < 52:
        raise PackError("expected a big-endian ELF32 RPX")
    offset = struct.unpack_from(">I", raw, 32)[0]
    stride, count, names = struct.unpack_from(">HHH", raw, 46)
    if stride < 40 or offset + stride * count > len(raw):
        raise PackError("invalid RPX section table")
    headers = [
        struct.unpack_from(">10I", raw, offset + stride * i) for i in range(count)
    ]
    sym = next((i for i in reversed(range(count)) if headers[i][1] == 2), -1)
    strings = next((i for i in range(count) if headers[i][1] == 3 and i != names), -1)
    crc = zlib.crc32(b"\x7fRPX")
    crc = zlib.crc32(struct.pack("<4i", count, sym, strings, names), crc)
    for name, kind, flags, address, start, size, _, _, align, _ in headers:
        payload = None
        if kind != 8:
            if start + size > len(raw):
                raise PackError("RPX section outside file")
            payload = raw[start : start + size]
            if flags & 0x08000000:
                if len(payload) < 4:
                    raise PackError("truncated compressed section")
                expected = struct.unpack_from(">I", payload)[0]
                if expected > 256 * 1024 * 1024:
                    raise PackError("compressed section too large")
                decoder = zlib.decompressobj()
                payload = decoder.decompress(payload[4:], expected + 1)
                if len(payload) != expected or not decoder.eof:
                    raise PackError("invalid compressed section size")
                size = expected
        crc = zlib.crc32(
            struct.pack("<6I", name, kind, flags, address, size, align), crc
        )
        if payload:
            crc = zlib.crc32(payload, crc)
    return crc
