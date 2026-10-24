# Xbox prompt content-mod research

Checked 2026-10-24 for the Wii U static-recompilation launcher.

## Decision

English Xbox prompt mods **exist**. However, no published download was verified
as a turnkey match for **both** the current Xbox installer and
`preferences.layout == 1`. Do not add a featured/setup recommendation yet.
The Portuguese translation offer remains unchanged and consent remains required.

The closest candidate is [Ultimate Xbox UI](https://gamebanana.com/mods/373804).
Its content ZIPs can be installed by our overlay, but its Western input scheme
swaps X/Y relative to our labels mode. The compatible older **PE** scheme is
available as BNP, RAR or multi-layout 7z, rather than a directly installable,
unambiguous ZIP. This is a compatibility conclusion, not a claim that English
Xbox artwork is unavailable. No mod archives or game files were downloaded.

## Search and evidence

- GameBanana game **5866**, [Controls category](https://gamebanana.com/mods/cats/2119),
  and its game-filtered API search for `xbox`: 14 records on one complete page.
  Reviewed all Xbox UI candidates listed below, including obsolete entries.
- Web queries: `Cemu BOTW Xbox button UI mod English western icons`,
  `site:gamebanana.com/mods "Xbox One" "Wild"`,
  `site:gamebanana.com "Xbox" "USen"`, and Xbox/USen/EUen variants.
- Cemu community: [complete UI pack author's release discussion](https://www.reddit.com/r/cemu/comments/g8jods/),
  [2023 comparison](https://www.reddit.com/r/cemu/comments/13dn6lg/), and
  [2022 installation/conflict discussion](https://www.reddit.com/r/cemu/comments/tzk3kw/).
  These supplied leads; archive paths and terms below come from publisher metadata.
- GitHub searches for `botw xbox button prompts`, `Xbox ControllerLayout` in
  `cemu-project/cemu_graphic_packs`, and BotW Xbox UI repositories. Reviewed the
  community pack's source and [open map-prompt issue #716](https://github.com/cemu-project/cemu_graphic_packs/issues/716).
  Switch/TotK results were excluded from Wii U compatibility claims.

For each GameBanana mod, read `https://gamebanana.com/apiv11/Mod/<id>/ProfilePage`
and/or `DownloadPage`. Archive inventories came from the **JSON metadata** at
`https://gamebanana.com/apiv13/File/<file-id>` (`_aArchiveFileTree`), as used by
the site's File Contents viewer. These are filenames, not extracted archive
members. The older v11 File endpoint and guessed `/Contents` endpoint were not
usable. Metadata snapshots and selection-probe output are local-only under
`build/research-70/` and `build/70-xbox-prompts-mod/`.

All inventories below are outer archive inventories. A `Bootup.pack` entry does
**not** prove which nested layout/texture files it contains. No `.sblarc` or
`.bflim` payload was opened. Publisher support claims are distinguished from
unverified region/version coverage.

## Permissions

The reviewed GameBanana profiles all display **CC BY-NC-ND 4.0** and a separate
checklist permitting download/install, requiring permission for redistribution
and derivative distribution, and prohibiting commercial use. For this store,
treat these as **author-hosted download/install only**: credit and link to the
publisher; do not mirror, bundle, modify or republish their archives/art.
The checklist and license wording should both be preserved when seeking any
broader permission. These labels do not establish ownership of embedded game art.
See, for example, [Ultimate's profile metadata](https://gamebanana.com/apiv11/Mod/373804/ProfilePage)
and [2023 edition's profile metadata](https://gamebanana.com/apiv11/Mod/414301/ProfilePage).

## Candidate inventory

Paths in the table are relative to a pack's `content/`, unless stated otherwise.
“Unknown” means the publisher did not provide a verified support matrix.

| Mod / publisher                                                                                                         | Published files and packaging                                                                                                                                                                                                             | Version, region and language evidence                                                                                                                                            | Current store result                                                                                                                       |
| ----------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| [Ultimate Xbox UI, 373804](https://gamebanana.com/mods/373804), **Proofeek**, mod 2.3a                                  | Four separate graphic-pack ZIPs (ST/WE/ALT/BX), a BNP, UKMM ZIP, `packs.json`. A standard `content/` pack; details below.                                                                                                                 | Fonts for EU/US/JP; shared Bootup. No explicit Wii U game-version or per-language test matrix. USen/EUen coverage is suggested by regional fonts, not proven by filenames alone. | Metadata and ZIP transport work. Default selection is BNP; Western's X/Y mapping does not match labels mode.                               |
| [Xbox One UI Complete – BNP Version, 33532](https://gamebanana.com/mods/33532), **NoPyro**, original **spauldhaliwal**  | `xboneui_us.7z` (405477), `xboneui_eu.7z` (405478): each has three `content/` packs, PE/SL/WL, `Font/Font_US.sbfarc` or `Font_EU.sbfarc`, `Pack/Bootup.pack`, `rules.txt`. Also separate PE/SL/WL BNPs.                                   | Explicit US/EU packaging; latest supported game update unspecified. Individual text languages not enumerated.                                                                    | 7z disabled in default UWP; BNP merge/options unsupported. Generic root search would choose a first pack, without region/layout selection. |
| [Xbox UI Fixed (Complete), 33527](https://gamebanana.com/mods/33527), **Vojajin**, originals **spauldhaliwal / lorul1** | Standard, PE, Western RARs (486029/30/31). Western inventory: `Xbox UI western layout/Botw Xbox UI/content/{Font/Font_US.sbfarc,Pack/Bootup.pack}` plus `rules.txt`. Standard Cemu content-pack wrapper.                                  | Author reports Cemu **1.21.1b**, which is an emulator version, not a game version. Inspected Western has US font; EU/JP and other variants' contents unverified.                 | RAR unavailable in default UWP. Current selection picks Standard even for labels mode.                                                     |
| [Xbox One UI Complete (2023 edition), 414301](https://gamebanana.com/mods/414301), **i2m**, original **spauldhaliwal**  | `xbox_ui_layout_us.bnp` (898018), `xbox_ui_layout_eur.bnp` (898024). US inventory has a top-level placeholder text file and PE/Standard/Western options containing font/Bootup replacements and merge logs. **Not** a ready content pack. | Explicit US/EUR. Cleaned from the old 1.4.1 mod using newer BCML; that history is not proof of validation against Wii U 1.5.0/v208.                                              | BNP unsupported. Treating it as ZIP would not apply the options/merge logs.                                                                |
| [Xbox Controller UI Set 0.5 (BETA), 33524](https://gamebanana.com/mods/33524), **lorul1**                               | `controller_layouts.zip` (477896), `Controller Layouts/BreathOfTheWild_Xbox Controllers/content/{Font/Font_US.sbfarc,Pack/Bootup.pack}` and `rules.txt`. Standard content pack.                                                           | Author: Cemu, USA tested; non-USA untested. Exact game version unknown.                                                                                                          | ZIP/root supported, but requires custom guest A/B/X/Y → physical Y/B/A/X mapping, incompatible with both launcher modes.                   |
| [Xbox One UI Complete 1.4.1, 33533](https://gamebanana.com/mods/33533), **spauldhaliwal**, obsolete                     | Standard/Western/PE ZIPs. PE (369743) contains loose `Bootup.pack`, `Font_EU.sbfarc`, `Font_US.sbfarc`, `readme.txt`, **no `content/`**. Installation replaces original files manually.                                                   | Made for **1.4.1**; author mentions community reports for **1.5.0**, not author testing. US/EUR instructions. Language coverage not enumerated.                                  | ZIP transport works, root validation rejects it. Do not overwrite the player's dump or silently repack it.                                 |
| [Xbox UI (US Region) for Updated Linkle Mod, 49803](https://gamebanana.com/mods/49803), **mikewander**                  | `updated_linkle_xbox_ui_usa.7z` (421541), `BreathOfTheWild_!ALinkleXboxUIUSA/content/{Font/Font_US.sbfarc,Pack/Bootup.pack}` and `rules.txt`.                                                                                             | US only, explicitly excludes EU, requires updated Linkle mod. Exact game version unknown.                                                                                        | 7z unavailable; dependent mod, unsuitable for general setup.                                                                               |
| [PT-BR translation, 717162](https://gamebanana.com/mods/717162), **gurincrivel**, mod 2.0.0; existing baseline          | Four Nintendo/Xbox and official/JP ZIPs. Xbox official (1847227) has `content/Font/Font_{EU,JP,US}.sbfarc`, `Pack/Bootup.pack`, `Bootup_{EUen,JPja,USen}.pack`, RST and `System/Version.txt`.                                             | Portuguese text; regional archive names are replacement destinations, not English translations.                                                                                  | Existing explicit-consent offer; correct official Xbox ZIP selected for labels mode.                                                       |

Inventory evidence: [Ultimate Western](https://gamebanana.com/apiv13/File/980133),
[Ultimate Standard](https://gamebanana.com/apiv13/File/980135),
[NoPyro US](https://gamebanana.com/apiv13/File/405477),
[NoPyro EU](https://gamebanana.com/apiv13/File/405478),
[Vojajin Western](https://gamebanana.com/apiv13/File/486031),
[i2m US](https://gamebanana.com/apiv13/File/898018),
[lorul1 Beta](https://gamebanana.com/apiv13/File/477896),
[obsolete PE](https://gamebanana.com/apiv13/File/369743),
[Linkle](https://gamebanana.com/apiv13/File/421541),
[PT-BR Xbox](https://gamebanana.com/apiv13/File/1847227).

[NoPyro's Switch port, 33340](https://gamebanana.com/mods/33340) and
[Velkyn's UKMM Xbox mod, 670522](https://gamebanana.com/mods/670522) belong to game
**6386**, not 5866. Velkyn publishes Western, X/Y-swap and Standard ZIPs, but
Switch/UKMM archives cannot establish Wii U byte-order, format or region support.
They use the same displayed download/install-only checklist described above.

### Closest candidate: exact fit checks

Ultimate Western file **980133**, `ultimatexboxui_we__23a.zip`, is 29,990,170
bytes. Its outer folder is `BreathOfTheWild_!UltimateXboxUI_WE` and contains:

```text
content/Font/Font_EU.sbfarc
content/Font/Font_JP.sbfarc
content/Font/Font_US.sbfarc
content/Pack/Bootup.pack
content/System/Resource/ResourceSizeTable.product.srsizetable
rules.txt
```

This fits `install_zip`'s content-first root discovery (one wrapper level).
It would become an ordinary file-replacement mod, not a locked rules-only
import. The metadata cannot reveal its nested layout files or verify every
A/B/X/Y, Menu/View and LB/RB/LT/RT glyph. Regional fonts alone do not certify
French, German, Spanish, Italian, Russian or Japanese tutorial coverage.

The author's [Western input-settings image](https://images.gamebanana.com/img/ss/mods/63fdff683538d.jpg)
explicitly maps guest **A→A, B→B, X→Y, Y→X** on an Xbox controller.
Our `Controller::update` in `runtime/src/host/uwp/controller.cpp` uses
**A→A, B→B, X→X, Y→Y** for labels mode. Thus Western would tell players the
wrong X/Y buttons. Standard, Alternative and Baxmann also require different
mappings; the published list has no PE/identity graphic-pack ZIP. Changing
controller semantics as a side effect of accepting a cosmetic mod would require
a separately designed input preference and regression tests.

### GitHub/Cemu alternative

The community [Xbox Controller Button Layout](https://github.com/cemu-project/cemu_graphic_packs/tree/master/src/BreathOfTheWild/Mods/ControllerLayout)
was added by **Crementif** in commit
[57300c7](https://github.com/cemu-project/cemu_graphic_packs/commit/57300c7).
It contains `rules.txt`, `patch_XboxImageReplacement.asm` and
`patch_XboxTextReplacement.asm`: **no `content/` or replacement artwork**.
The patches target v208 (`moduleMatches = 0x6267BFD0`); rules list JP/US/EU title
IDs. Its purpose is swapping ABXY symbols using guest patches, not a complete
Xbox glyph set for shoulders/triggers/Menu/View. The repository's
[license is CC0](https://github.com/cemu-project/cemu_graphic_packs/blob/master/LICENSE.md).
Downloading a PPC patch cannot activate it in this static recompilation; it
requires the existing build-time pack pipeline. The open map-prompt bug also
needs console validation. It is not an interchangeable content-mod solution.

The 2020 Cemu community release by **DatGuyPigglet** describes separate EU/US
and PE/Standard/Western variants. Its author-linked
[Drive ZIP](https://drive.google.com/file/d/1-7wFJA-qqTMKcJvUb0tbf7iWaebQ_D8H/view)
still has readable metadata: **Complete Controller UI Pack v1.2.1.zip**, MIME
`application/x-zip-compressed`, **411,374,926 bytes**. Drive metadata was read
through the connected tool; archive contents were not downloaded. The author
describes Cemu graphic-pack installation, but exact `content/` paths, nested
files, game update and language coverage remain unverified. No redistribution
license is stated in the release post; only the author's download invitation
is established. It has no discovered GameBanana Mod ID: the existing source
cannot feature/resolve it, and an ordinary Drive viewer URL is not a direct ZIP
download. Supporting Drive delivery plus choosing one region/layout out of a
multi-pack archive would need additional work. Do not substitute anonymous
mirrors. GitHub's
[nx-optimizer discussion](https://github.com/MaxLastBreath/nx-optimizer/discussions/266)
likewise offers no Wii U content pack and explicitly declines distributing
game-derived UI assets.

## Existing source and file selection

`runtime/src/mods/gamebanana.cpp` already fetches game-5866 search results and
`Mod/<id>/DownloadPage`; `store.cpp` fetches profile metadata. No new transport
is necessary for these IDs. Metadata availability is distinct from installability:
`OnlineFile::supported()` accepts ZIP, the default Xbox build has no 7z/RAR
backend, and neither installer performs BNP/UKMM option resolution or merges.

`preferred_file` scores filenames containing `xbox` for layout 1 and filenames
without it for layout 0, preferring non-`jp` and then the first tie. It does not
understand ST/WE/PE, game regions, archive support or UKMM. A local C++ probe
called the **actual** parser/selector on publisher JSON, without HTTP downloads:

| Mod    | Current selection for layout 1     | Why unsuitable                                                                                         |
| ------ | ---------------------------------- | ------------------------------------------------------------------------------------------------------ |
| 373804 | `ultimatexboxui__23a.bnp`          | First matching filename; unsupported format. Selecting the WE ZIP manually still has the X/Y mismatch. |
| 33532  | `xbox_one_ui_complete_pe.bnp`      | Correct PE controls, unsupported BNP.                                                                  |
| 33527  | `xbox_ui_standard_layout.rar`      | Wrong layout and unsupported RAR.                                                                      |
| 414301 | `xbox_ui_layout_us.bnp`            | Unsupported BNP; no region selection.                                                                  |
| 33524  | `controller_layouts.zip`           | Required custom controls do not match labels mode.                                                     |
| 33533  | `botw_xbox_ui_standard_layout.zip` | Wrong scheme and no content root.                                                                      |
| 49803  | `updated_linkle_xbox_ui_usa.7z`    | Unsupported archive, US-only dependency.                                                               |
| 717162 | `ptbr_oficial_xbox_western.zip`    | Existing Portuguese offer, not an English replacement.                                                 |

No featured IDs, selector behavior, setup code or store tests were changed.
If an author later publishes a compatible PE/identity ZIP, require explicit
per-mod format/layout selection rather than extending the filename heuristic
blindly. Preserve the Portuguese branch; gate the new offer on non-Portuguese
console language and labels mode, suppress installed/previously answered offers,
default to Not now, and persist either answer **before** invoking the normal
install path. Test both consent outcomes, B/cancel, missing metadata, offline
fallback, repeat setup and API file reordering using synthetic fixtures.

## Original replacement plan

Only **our own artwork drawn from scratch** may be shipped. Create an original
ABXY set, LB/RB/LT/RT, Menu/View, stick-click and directional glyphs, including
pressed/disabled/tutorial states. Keep editable vector sources, export scripts
and an explicit artwork license. Do not trace or extract Nintendo/Microsoft
artwork, copy another mod's atlas, distribute game fonts, or ship a modified
Bootup archive containing unrelated game assets.

The following is a format/work plan, not a verified manifest of any mod's nested
files. Independent format references include
[Switch-Toolbox's supported formats](https://github.com/KillzXGaming/Switch-Toolbox)
and its [BFLYT/BFLAN editing guide](https://github.com/KillzXGaming/Switch-Toolbox/wiki/BFLYT-Editing).

1. Inventory the player's own **Wii U 1.5.0/v208** dump locally. Identify prompt
   references under `Layout/`, `Font/` and `Pack/Bootup*.pack`; record paths and
   hashes privately. Do not assume a loose `content/Layout/*.sblarc` covers
   Bootup-packed or text-inline prompts.
2. Handle **SARC** archives and **Yaz0** compression (`.sblarc` compressed layout
   archives; `.sbfarc` compressed font archives). Repack with Wii U byte order,
   alignment and hashes intact. Nested layouts use **BFLYT** panes/materials/
   texture references and **BFLAN** animations; image atlases use **BFLIM** with
   Wii U tiling/swizzle and format metadata. Switch BNTX assets are not a drop-in
   Wii U substitute.
3. Text-inline button glyphs can require **BFFNT** font atlas/character-map/width
   updates, independently of layout textures. Preserve unrelated font glyphs
   in the player's local output; ship only original glyph art and transformation
   code. Tutorials may reference **MSBT** messages inside regional Bootup packs.
   Measure resized resources and update the **RST** (`.srsizetable`) as needed.
4. Implement a local, hash/version-checked transformation using the player's dump
   plus original art. Distribute the art, code and declarative edits, **never**
   complete copied game archives, fonts or binary deltas carrying extracted art.
   Generate the private result as a standard `content/` replacement tree and use
   the existing overlay. Keep unrelated content byte-for-byte where possible.
5. Define mappings explicitly: labels mode is guest A/B/X/Y → physical A/B/X/Y;
   positions mode swaps A/B and X/Y. Test USen and EUen first, then enumerate
   each supported language and region. Resolve translation/HD-font load-order
   conflicts without replacing the player's language unexpectedly.

Console acceptance must cover action/cancel, jump/attack, sprint, map pins,
inventory, loading hints, tutorials, shoulder/trigger prompts and Menu/View,
with both layouts, both English regions, every claimed language, and enabled
translation/HD-font combinations. Check cold restart, resource sizing and
memory. Host parser tests cannot certify game glyph rendering. A future
optional download must still require an explicit Yes; never fetch the archive
during metadata discovery or on Not now/B.
