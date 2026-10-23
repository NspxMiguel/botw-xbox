# Boot crash after AXInit: inherited update files

The USA v208 executable can have the correct SHA-256 while the merged content is
corrupt. An earlier extraction overwrote inherited base files with unrelated update bytes.
A valid executable hash therefore does not guarantee that the content folder is intact.
Use files already extracted and decrypted from your own Wii U; the launcher accepts the resulting
folders or decrypted archives and overlays the update on the base without parsing console filesystems.

In the affected v208 extraction, 11,081 linked files were overwritten. For example:

| File | Size | Damaged header | Intact base header |
| --- | ---: | --- | --- |
| `content/Sound/DummySound.bfsar` | 2,108 | `Yaz0 000e78b0` | `FSAR feff0040` |
| `content/System/RegionLangMask.txt` | 288 | `Yaz0 001df6e0` | Text with `USen USes USfr` |

## Guest fault and HLE boundary

The following descriptions are inferred from the generated code, rather than
recovered symbol names:

- `03c16038` opens the sound archive through `03c15b9c`.
- `03c15fa8` reads 256 header bytes through `03c15db8` / `FSReadFile`.
- `03c15f54` checks the FSAR signature/version via `03c15f14`; a bad signature
  leaves the stored header uninitialized.
- `03c14a28` initializes the archive reader from that header.
- `03c14a00` requests section `0x2002` from `03c149d0`. The lookup returns zero,
  and the load at `03c14a18` dereferences address `00000004`, with LR `03c14a14`.

LLDB on the unchanged upstream executable confirmed that the sound header read
at guest LR `03c15e48` requests size 1, count 256, handle 1, destination
`0e14f700`. `FSReadFile` correctly returns 256 and copies the damaged file's
literal Yaz0 bytes into guest memory. This is an extraction error, not a null
heap allocation or an HLE decompression requirement. Cafe FS should return the
file's actual bytes; adding automatic decompression would hide the wrong data.

## Repair and regression checks

Rebuild the merged game in a clean directory from intact, already extracted and decrypted base
and update folders, then overlay the update. Do not reuse a corrupted merged folder: stale files
can survive an overlay.
No game content, keys, or generated guest code belong in Git.

Alternatively, restore only the linked paths from an intact extraction of the
same base title. The local investigation used this method on a separate copy and
verified all restored paths against the base with rsync checksums.

With only those data files restored, the unchanged upstream runtime passed sound
initialization and reached playable control in the Shrine of Resurrection. The
168-second run rendered around 29 fps at scale 1 / 30 fps, with sound and no fatal
error. This also rules out the Xbox port as the cause of the original crash.
The `xbox/bootfix` Release build (`ninja -j4`) then loaded that autosave and reached
playable control as well. Its 112-second run rendered around 30 fps and was closed
after capturing the game window; no fatal error was logged.

```sh
cmake -S runtime/src/host/uwp/tests -B build-setup-tests -G Ninja
cmake --build build-setup-tests --parallel 2
ctest --test-dir build-setup-tests --output-on-failure
```

The retained fixtures cover decrypted archive integrity, base/update precedence, DLC mapping,
version checks and folder diagnostics. They contain only synthetic data.
