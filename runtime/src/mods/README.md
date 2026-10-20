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
