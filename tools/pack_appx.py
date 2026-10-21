#!/usr/bin/env python3
"""Pack a staged UWP folder into an .appx on macOS/Linux (no makeappx needed).

    tools/pack_appx.py <staged-dir> <out.appx>

Files are stored uncompressed, so the block map needs only the SHA-256 of each 64 KiB block.
Sign the result afterwards, e.g.:
    osslsigncode sign -pkcs12 test.pfx -pass "" -h sha256 -in out.appx -out signed.appx
The game build stays on the player's machine; this only packs what is already there.
"""
import base64, hashlib, os, sys, zipfile
from xml.sax.saxutils import quoteattr

BLOCK = 65536
TYPES = {
    "exe": "application/x-msdownload", "dll": "application/x-msdownload", "png": "image/png",
    "wav": "audio/wav", "json": "application/json", "txt": "text/plain",
}


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
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED, allowZip64=False) as z:
        for arc, path in files:
            size = os.path.getsize(path)
            blocks = []
            with open(path, "rb") as f:
                while chunk := f.read(BLOCK):
                    blocks.append(base64.b64encode(hashlib.sha256(chunk).digest()).decode())
            # Local file header: 30 bytes + name; stored, non-zip64 entries carry no extra field.
            blockmap.append(f'<File Name={quoteattr(arc.replace("/", chr(92)))} Size="{size}" '
                            f'LfhSize="{30 + len(arc.encode())}">')
            blockmap += [f'<Block Hash="{h}"/>' for h in blocks]
            blockmap.append("</File>")
            z.write(path, arc)
            if "." in arc:
                exts.add(arc.rsplit(".", 1)[-1].lower())
        blockmap.append("</BlockMap>")
        z.writestr("AppxBlockMap.xml", "".join(blockmap))
        ct = ['<?xml version="1.0" encoding="UTF-8"?>',
              '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">']
        ct += [f'<Default Extension="{e}" ContentType="{TYPES.get(e, "application/octet-stream")}"/>'
               for e in sorted(exts) if e != "xml"]
        ct += ['<Override PartName="/AppxManifest.xml" ContentType="application/vnd.ms-appx.manifest+xml"/>',
               '<Override PartName="/AppxBlockMap.xml" ContentType="application/vnd.ms-appx.blockmap+xml"/>',
               '<Override PartName="/AppxSignature.p7x" ContentType="application/vnd.ms-appx.signature"/>',
               '</Types>']
        z.writestr("[Content_Types].xml", "".join(ct))
    print(f"{out}: {len(files)} files, {os.path.getsize(out) / 2**20:.1f} MiB")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
