#!/usr/bin/env python3
"""Pack a staged UWP folder into an .appx on macOS/Linux (no makeappx needed).

    tools/pack_appx.py <staged-dir> <out.appx>

Files are stored uncompressed, so the block map needs only the SHA-256 of each 64 KiB block.
Sign the result afterwards, e.g.:
    osslsigncode sign -pkcs12 test.pfx -pass "" -h sha256 -in out.appx -out signed.appx
The game build stays on the player's machine; this only packs what is already there.
"""
import base64, hashlib, os, struct, sys, zlib
from xml.sax.saxutils import quoteattr

BLOCK = 65536
TYPES = {
    "exe": "application/x-msdownload", "dll": "application/x-msdownload", "png": "image/png",
    "jpg": "image/jpeg", "jpeg": "image/jpeg", "wav": "audio/wav", "json": "application/json", "txt": "text/plain",
}


class Zip64Writer:
    """Stored entries the way makeappx writes them: version 4.5, a ZIP64 data descriptor after each
    file, ZIP64 central records. Windows rejects plain ZIP 2.0 packages (0x8007000B)."""

    def __init__(self, path: str):
        self.f, self.entries = open(path, "wb"), []

    def add(self, arc: str, chunks) -> None:
        name, off, crc, size = arc.encode(), self.f.tell(), 0, 0
        self.f.write(struct.pack("<IHHHHHIIIHH", 0x04034B50, 45, 0x8, 0, 0, 0x21, 0, 0, 0, len(name), 0) + name)
        for chunk in chunks:
            crc, size = zlib.crc32(chunk, crc), size + len(chunk)
            self.f.write(chunk)
        self.f.write(struct.pack("<IIQQ", 0x08074B50, crc, size, size))
        self.entries.append((name, off, crc, size))

    def close(self) -> None:
        cd = self.f.tell()
        for name, off, crc, size in self.entries:
            extra = struct.pack("<HHQQQ", 1, 24, size, size, off)
            self.f.write(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 45, 45, 0x8, 0, 0, 0x21, crc,
                                     0xFFFFFFFF, 0xFFFFFFFF, len(name), len(extra), 0, 0, 0, 0, 0xFFFFFFFF)
                         + name + extra)
        end = self.f.tell()
        n = len(self.entries)
        self.f.write(struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0, n, n, end - cd, cd))
        self.f.write(struct.pack("<IIQI", 0x07064B50, 0, end, 1))
        self.f.write(struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0))
        self.f.close()


def file_chunks(path: str):
    with open(path, "rb") as f:
        while chunk := f.read(BLOCK):
            yield chunk


def main(src: str, out: str) -> None:
    files = []
    for root, _, names in os.walk(src):
        for name in sorted(names):
            path = os.path.join(root, name)
            files.append((os.path.relpath(path, src).replace(os.sep, "/"), path))
    files.sort(key=lambda f: (f[0] != "AppxManifest.xml", f[0]))
    if not files or files[0][0] != "AppxManifest.xml":
        raise SystemExit("AppxManifest.xml missing from the staged folder")

    blockmap = ['<?xml version="1.0" encoding="UTF-8" standalone="no"?>',
                '<BlockMap xmlns="http://schemas.microsoft.com/appx/2010/blockmap" '
                'HashMethod="http://www.w3.org/2001/04/xmlenc#sha256">']
    exts = set()
    z = Zip64Writer(out)
    for arc, path in files:
        size = os.path.getsize(path)
        blocks = []
        with open(path, "rb") as f:
            while chunk := f.read(BLOCK):
                blocks.append(base64.b64encode(hashlib.sha256(chunk).digest()).decode())
        # Local file header: 30 bytes + name, no extra field (sizes live in the data descriptor).
        blockmap.append(f'<File Name={quoteattr(arc.replace("/", chr(92)))} Size="{size}" '
                        f'LfhSize="{30 + len(arc.encode())}">')
        blockmap += [f'<Block Hash="{h}"/>' for h in blocks]
        blockmap.append("</File>")
        z.add(arc, file_chunks(path))
        if "." in arc:
            exts.add(arc.rsplit(".", 1)[-1].lower())
    blockmap.append("</BlockMap>")
    z.add("AppxBlockMap.xml", ["".join(blockmap).encode()])
    ct = ['<?xml version="1.0" encoding="UTF-8"?>',
          '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">']
    ct += [f'<Default Extension="{e}" ContentType="{TYPES.get(e, "application/octet-stream")}"/>'
           for e in sorted(exts) if e != "xml"]
    ct += ['<Override PartName="/AppxManifest.xml" ContentType="application/vnd.ms-appx.manifest+xml"/>',
           '<Override PartName="/AppxBlockMap.xml" ContentType="application/vnd.ms-appx.blockmap+xml"/>',
           '<Override PartName="/AppxSignature.p7x" ContentType="application/vnd.ms-appx.signature"/>',
           '</Types>']
    z.add("[Content_Types].xml", ["".join(ct).encode()])
    z.close()
    print(f"{out}: {len(files)} files, {os.path.getsize(out) / 2**20:.1f} MiB")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
