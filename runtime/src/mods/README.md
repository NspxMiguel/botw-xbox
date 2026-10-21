# Portable mod catalog

`catalog.h` manages one mods root. No game assets are opened: discovery uses only
folder/file names beneath immediate child folders with `content/` or `aoc/`
(case insensitive). Symlinks are ignored. Translation takes precedence over dubbing
when a folder contains both. Discovered replacement folders start disabled;
compiled graphics packs start enabled.

```cpp
mods::ModCatalog catalog;
catalog.load(mods_root, packs::compiled_plan());
// catalog.list(), setEnabled(id, enabled), move(id, zero_based_index)
catalog.save();
auto plan = mods::filtered_plan(packs::compiled_plan(), catalog);
```

Entries run from low to high priority. `move` uses the final zero-based index and
rejects invalid indices/IDs. `setEnabled` returns false for unknown or locked IDs.
IDs distinguish compiled packs (including duplicate names) from replacement folders.
A different compiled selection can change pack IDs; unknown saved IDs are ignored.

`save()` replaces `enabled.txt` using a temporary sibling and atomic replacement.
Filesystem errors are reported as exceptions. Ordinary enabled folder paths are
written verbatim with a `./` prefix (and `/.` suffix for trailing whitespace).
The existing VFS reader therefore applies the selection without any format changes.
Lines such as `# w8-mod "file:Translation" 0` retain the order of disabled entries;
compiled graphics selection uses the same comment records. VFS ignores these
comments. Legacy path-only lists remain supported, including repeated paths (last
occurrence wins). This catalog manages immediate child folders only; external or
plain-tree paths in manually authored legacy lists are outside its discovery scope.

When the plan contains any `initial_memory`, every compiled pack stays enabled and
locked with `rebuild to change`: those writes cannot be attributed to individual
packs. `filtered_plan` never changes memory writes, executable identity, or compiled
control fields. It orders graphics entries according to the catalog, clears disabled
packs' shaders/textures, and preserves unmatched packs. The fingerprint includes
the graphics selection/order to separate renderer caches. The returned plan must
outlive any renderer pointers into it. No runtime PPC patching or compilation occurs.

The adapter is deliberately not called from `main.cpp` or `host/uwp/runtime.cpp`.
Their owners must load the saved catalog, retain the filtered plan, and pass it to
the renderer/installer. File replacement auto-launch continues to read `enabled.txt`
through VFS independently.

Run authored synthetic fixtures on macOS:

```sh
cmake -S runtime/src/mods/tests -B /tmp/botw-mods-tests -G Ninja
cmake --build /tmp/botw-mods-tests
ctest --test-dir /tmp/botw-mods-tests --output-on-failure
```

## Launcher store (W15)

The silver launcher store uses `store.h` / `store.cpp`. A source implements
`search(query, category, page)` and `files(card)`, with an injected HTTP transport.
GameBanana (game 5866), Cemu community graphic packs, HTTPS links and local/USB
folders share cards, categories, version/date comparisons and installation receipts.
Both legacy `BreathOfTheWild_*` directories and modern
`src/BreathOfTheWild/<pack>` identities are supported. Graphics rules and presets
are pinned to the release tag; `rules.txt`'s `version` is a **format** version,
not a mod update version.

The UWP adapter in `host/uwp/store_app.inl` owns asynchronous refreshes. HTTP,
cache I/O, image decode, discovery and extraction run on workers; only the current
request generation publishes UI state. Input continues during catalog and detail
requests. A cancelled download removes its temporary file. Updates extract and
validate the new mod before disabling/removing the previous copy, preserving its
selection and load order. Downloaded graphics packs use the existing
`.imported-graphics` catalog marker and remain locked pending recompilation;
this static recompilation does not execute downloaded PPC patches.

All data is player-side under `LocalState/mods`: timestamped responses in
`cache/responses`, bounded image downloads in `cache/thumbs`, six recent searches
in `cache/searches`, `.store-receipt` metadata inside installed directories and
transient downloads in `.downloads`. Responses are fresh for one hour. A failed
refresh can use stale responses offline; explicit retry bypasses freshness.
Decoded store images are scaled to at most 640×360, with an 18-entry session cap. No mod or game assets are shipped.

Controller: LB/RB select tabs (images on details), LT/RT select categories,
D-pad/left stick traverse cards and controls with Repeat, Y opens CoreText search,
A opens/selects, X toggles installed entries and B returns. The source chips and
category rail are also focusable. Detail actions include file selection, removal
confirmation and load-order changes. Changes are saved immediately for next boot.
The initial remote catalog is bounded to four GameBanana pages per search;
installed identities outside those pages are checked separately for updates.

### Archive backend limitation

ZIP works in the default Xbox build. `archive_install.cpp` also provides a
transactional 7z/RAR reader behind `BOTW_MOD_LIBARCHIVE=ON`, using a **linked,
packaged** libarchive, never a downloaded binary or process. The official vcpkg
libarchive port currently excludes UWP, so this option defaults off: Xbox displays
the existing extract-on-PC guidance for 7z/RAR. Enabling it for Xbox requires a
separately validated sandbox-compatible library and package dependency closure.
The portable backend is tested against authored 7z and stored-RAR archives,
including traversal, symlink rejection and rollback. Password-protected archives
are rejected. Full Xbox 7z/RAR installation is not complete.

The portable test project also covers saved synthetic JSON/rules fixtures,
accent-insensitive Portuguese search, categories, numeric release ordering,
date updates, receipts, cache fallback, selected-pack extraction and controller
navigation. Archive tests are included when libarchive development files are
available (`-DLibArchive_INCLUDE_DIR=... -DLibArchive_LIBRARY=...` if necessary).
No test performs network requests. `docs/XBOX-PORT.md` is unchanged.
