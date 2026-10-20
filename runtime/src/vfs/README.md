# Content replacement overlay (W6)

The overlay is independent of game data and packs' PPC/shader build plans. It
replaces files beneath `/vol/content` and `/vol/aoc` only. It accepts Cemu pack
roots (`rules.txt` beside `content/` and/or `aoc/`) and plain replacement trees.
No translation, voice archive, or other game asset is included.

## Launcher and W5 integration

Before guest FS clients start, call:

```cpp
vfs::content_overlay().configure(game_root, {
    {"Translation", translation_pack_path, true},
    {"Dubbing", dubbing_pack_path, true},
    {"Optional override", optional_pack_path, false},
}, log_callback);
```

`vfs::Mod` is the adapter boundary for the enabled pack selection: `name`,
`path`, `enabled`, and optional `volume`. Entries run from low to high priority;
**later entries win**. Disabled and missing paths are ignored. A plain tree is
implicitly rooted at `content`; explicitly set `volume = "aoc"` for an unwrapped
DLC tree. Wrapped DLC trees may include subdirectories such as `aoc/0010/`.

W5's `tools/packs/rules.py` owns Cemu rules parsing. The current
`runtime/src/packs/packs.h` build plan has no content paths or enabled selection
API, so this layer consumes the launcher's selected metadata instead of parsing
rules again. Pass the same ordered enabled selection to both systems. Cemu's
`[Definition] path` is a display category, not a host filesystem location.
Replacement files do not require PPC recompilation or a generated pack plan.
The overlay does not interpret presets, patch code, or discover enabled state
from `rules.txt`.

If the launcher has not configured the singleton, `FSInit` loads an ordered
`enabled.txt` from each of these roots:

1. `<game>/../mods`
2. `<LocalState>/mods` (`g_user_root/mods` on other hosts)

LocalState entries therefore win over the game's sibling entries. Supplying
`BOTW_MODS_ROOT` selects one root instead. Hosts can also call
`configure_defaults(game_root, local_state, chosen_root, log_callback)` directly.
An absent or empty list enables nothing; scanning arbitrary installed folders
never silently enables them. For example:

```text
# One pack directory per line, lowest priority first.
BrazilianTranslation
BrazilianDubbing
/path/to/another/pack
```

Paths are relative to the list's directory unless absolute. Spaces are literal;
blank lines and lines starting with `#` are ignored. Omit a path to disable it.
Use the structured API for named entries and explicit enabled flags. Explicit
configuration (even an empty list) survives `FSInit`'s default initialization.

## Runtime behavior

Only enabled mod trees are indexed at configuration time. Content lookup is
component-wise ASCII case-insensitive, including base fallback on case-sensitive
hosts. Directory snapshots merge the base and all enabled layers, deduplicating
names without case and reporting the winning file's native stat and size. A
replacement file masks a directory's subtree; a later directory can replace it.
Open handles retain their selected physical files. File replacement logs go
through the supplied callback, once per virtual file on its first lookup,
including stat/list lookups. The FSInit callback uses `LOG`; direct configuration without a callback writes
to stderr.

With no enabled content nodes, the FS hooks use an `active()` guard and the
original native path: no overlay traversal, lookup, logging, or allocation.
Replacement indexing is a startup snapshot; configure again from the launcher
to apply a changed selection, before guest threads run and with all FS handles
closed. Symlink entries inside mod trees are skipped. All content opens resolve through the overlay, including update modes.
Path-based removal, saves, code, and metadata retain their native paths.

## Synthetic tests (no game files or full runtime build)

```sh
cmake -S runtime/src/vfs/tests -B /tmp/botw-vfs-tests
cmake --build /tmp/botw-vfs-tests -j 2
ctest --test-dir /tmp/botw-vfs-tests --output-on-failure
```

The portable overlay test covers precedence, disabled/missing mods, plain and
wrapped packs, DLC, merged and mod-only directories, case matching, missing
files, replacement sizes, file/directory conflicts, logging, and selection roots.
An additional portable test compiles the real `coreinit/fs.cpp` hooks against
synthetic guest memory and checks open/read, path/handle stat, merged directory
entries, end-of-directory, closing, handle reuse, and W1 path guards. Fixtures
include Portuguese game/mod folders, UTF-8 enabled lists and filenames, and
native save directory enumeration. All runtime string paths are UTF-8; the FS
hooks retain W1’s native `path_stat`, `path_open`, and filesystem iterators. Test sources use `.cc` so
the runtime's recursive `.cpp` source glob does not include test entry points.

For ASan/UBSan on macOS:

```sh
cmake -S runtime/src/vfs/tests -B /tmp/botw-vfs-sanitized \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build /tmp/botw-vfs-sanitized -j 2
UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir /tmp/botw-vfs-sanitized --output-on-failure
```
