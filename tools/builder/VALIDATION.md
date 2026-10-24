# Builder validation — 2026-10-23

A real end-to-end macOS arm64 build from an actual v208 dump succeeded on
2026-10-23: recompile, CMake, Ninja, staging and signing in **1196.90 s** total
(about 20 min, `-j2`), producing a signed **452,217,106-byte** APPX whose signature
verified with osslsigncode. Intermediates were removed afterwards. The sections
below record the earlier checks, including the first attempt that stopped at the
disk-space guard. Installing that package on an Xbox is still to be verified.

## Files

- `tools/builder/botw_builder.py`: Python 3.11+ stdlib entry point for doctor, setup,
  build and Device Portal installation; explicit Microsoft license consent, pinned
  downloads, dump hashing, private build directories, signing and timing.
- `tools/builder/tests/test_builder.py`: synthetic dump/CLI tests, archive integrity,
  consent, space/repository guards, multipart/CSRF/auth handling, simulated build
  orchestration, Ninja waiting, CMake tool discovery/overrides, real synthetic APPX
  signing and certificate reuse.
- `tools/builder/ruff.toml`, `tools/builder/.editorconfig`: formatter settings.
- `cmake/toolchains/uwp-clang-cl.cmake`: overridable cached tool/root paths, tool
  discovery, versioned/flat SDK layouts and existing maintainer defaults.
- `CMakeLists.txt`, `tools/shaders/CMakeLists.txt`: package discovery instead of
  required Homebrew absolute include/library paths.
- `BUILDING.md`: English and PT-BR player guide, linked from both README languages.
- `.gitignore`: defensive exclusions for builder data, outputs and signing material.

Existing CMake files were formatted with cmake-format in a separate mechanical
commit before the implementation commit. No project CLAUDE.md was created.

## Commands and results

All commands ran from `/Users/miguel/.local/share/nxbox/botw-wiiu/builder-wt` on
macOS arm64. The entry point used uv's Python 3.12.14 unless specified otherwise.

```sh
uv run tools/builder/botw_builder.py doctor
```

Exit 0; total **0.01 s** on the final run:

```text
OK clang-cl: /opt/homebrew/opt/llvm/bin/clang-cl
OK lld-link: /opt/homebrew/bin/lld-link
OK llvm-lib: /opt/homebrew/opt/llvm/bin/llvm-lib
OK llvm-rc: /opt/homebrew/opt/llvm/bin/llvm-rc
OK llvm-mt: /opt/homebrew/opt/llvm/bin/llvm-mt
OK llvm-mc: /opt/homebrew/opt/llvm/bin/llvm-mc
OK cmake: /opt/homebrew/bin/cmake
OK ninja: /opt/homebrew/bin/ninja
OK git: /opt/homebrew/bin/git
OK uv: /opt/homebrew/bin/uv
OK openssl: /opt/homebrew/bin/openssl
OK signer: /opt/homebrew/bin/osslsigncode
Install prerequisites:
brew install llvm lld cmake ninja osslsigncode openssl git uv
Doctor passed. Next: setup (toolchain data is checked separately).
```

```sh
uv run tools/builder/botw_builder.py setup \
  --data-dir /Users/miguel/.local/share/nxbox/builder-test-data \
  --xwin-dir /Users/miguel/.local/share/nxbox/xwin/sdk \
  --vcpkg-dir /Users/miguel/.local/share/nxbox/vcpkg-uwp \
  --ffx-source /Users/miguel/.local/share/nxbox/deps/fidelityfx-1.1.4 \
  --ffx-shaders /Users/miguel/.local/share/nxbox/deps/ffx-shaders-1.1.4 \
  --graphic-packs /Users/miguel/.local/share/nxbox/deps/cemu_graphic_packs/src/BreathOfTheWild
```

Exit 0; total **0.00 s** at displayed precision. Microsoft CRT/SDK, vcpkg,
FidelityFX shader headers, FidelityFX source and graphic packs each took **0.00 s**
when reusing the specified local directories. Dependency markers and the FidelityFX
backend source hash passed. The only persistent test data is
`/Users/miguel/.local/share/nxbox/builder-test-data/setup.json` (toolchain paths).
No Microsoft license flag was passed and no consent was given on the player's behalf.

The published vcpkg ZIP was streamed through Python's `urllib.request.urlopen` and
`hashlib.sha256` without writing a copy to disk: **194,538,629 bytes**, **4.25 s**,
SHA-256 **3b304cbb65d29c08f66ca2de3328855a7b4f7a745187bde6e52a1c8358e0ac6a**.
That exact hash is pinned in the builder.

The pinned shader tarball was downloaded, hashed and extracted in a temporary
folder: **0.94 s**, SHA-256
`7b57fd0a3f0e4e0fe24f3bb881f21480754842ff887b96ec9398b857f950e3de`.
The pinned macOS arm64 xwin tarball was downloaded, hashed, extracted and its
`--help` run: **1.04 s**, SHA-256
`f3c43c6cd73aa641e8979c67fe724b67bf81723719d6786adbcdca8e5a1975f9`.
The CLI exposes the required acceptance, SDK version, architecture, cache, unpack
and splat options. These temporary downloads/extractions were removed. This did
not download Microsoft's CRT/SDK.

Pins originate from the [dependency release](https://github.com/NspxMiguel/botw-xbox/releases/tag/deps-uwp),
[xwin 0.6.6-rc.2 release](https://github.com/Jake-Shadle/xwin/releases/tag/0.6.6-rc.2)
and the official Cemu packs checkout at commit
`975f3759d7837da5a374150347d3202360b13ca3`. The signer minimum accounts for
[osslsigncode's APPX support and directory-ordering fix](https://github.com/mtrojnar/osslsigncode/blob/master/NEWS.md).

```sh
uv run --no-project python - <<'PY'
from tools.builder.botw_builder import verify_dump, step
with step('Verify actual player dump (read only)'):
    verify_dump('/Users/miguel/.local/share/nxbox/botw-wiiu-game')
PY
```

Exit 0; **0.01 s**. Output: `Verified Wii U v208:` followed by the supplied
`code/U-King.rpx` path. No output source or game copy was made.

```sh
uv run tools/builder/botw_builder.py build \
  --game /Users/miguel/.local/share/nxbox/botw-wiiu-game \
  --data-dir /Users/miguel/.local/share/nxbox/builder-test-data \
  --out /Users/miguel/.local/share/nxbox/builder-test \
  --xwin-dir /Users/miguel/.local/share/nxbox/xwin/sdk \
  --vcpkg-dir /Users/miguel/.local/share/nxbox/vcpkg-uwp \
  --ffx-source /Users/miguel/.local/share/nxbox/deps/fidelityfx-1.1.4 \
  --ffx-shaders /Users/miguel/.local/share/nxbox/deps/ffx-shaders-1.1.4 \
  --graphic-packs /Users/miguel/.local/share/nxbox/deps/cemu_graphic_packs/src/BreathOfTheWild
```

Exit **1**, as required by the task's disk limit. Preflight and total: **0.00 s**
at displayed precision. Its `df -h` check showed **2.5 GiB** available; the byte
check showed **2.45 GiB**:

```text
Build stopped: need at least 6 GiB free; only 2.45 GiB available.
Free 3.55 GiB before retrying (about 15 GB recommended).
```

The output directory was not created. Recompile, CMake configure, Ninja compilation,
Runtime install, gameplay packing and gameplay signing were **not run**. Their
sizes/durations are unavailable. **Gameplay APPX size: unavailable (not produced).**
There were no gameplay build intermediates to delete. No Ninja was started.

```sh
uv run --no-project --with pytest python -m pytest tools/builder/tests tools/packs/tests -q
```

Exit 0: **94 passed, 33 subtests passed in 43.84 s**. Includes existing pack tests.
The synthetic signing test runs the actual packer, OpenSSL and osslsigncode, verifies
its signature with the public certificate, checks the embedded APPX signature and
Publisher subject, and confirms the PFX is reused. Its synthetic artifacts live in
pytest's temporary directory outside the repository; none contains game data.

```sh
uv run --no-project --python 3.11 --with pytest python -m pytest tools/builder/tests -q
```

Exit 0: **55 passed in 0.38 s**, on Python **3.11.16**. Includes synthetic signing
and CMake script-mode discovery/override checks, which do not compile or run Ninja.

```sh
uvx ruff check tools/builder
uvx ruff format --check tools/builder
uvx --from cmakelang cmake-format --check CMakeLists.txt cmake/toolchains/uwp-clang-cl.cmake tools/shaders/CMakeLists.txt
uv run --no-project python .github/workflows/ci-build.py check-contract
git diff --check
```

All passed. Formatter/contract checks are subsecond operations; no runtime/game
build is implied by those checks.

## Remaining validation

- Real macOS gameplay recompile, full CMake configuration/compiler probing, Ninja
  compilation/link, staging, gameplay signing and resulting APPX size/durations
  require at least **3.55 GiB more free space** than at the recorded attempt.
- Fresh Microsoft CRT/SDK download, unpack and preservation of the store CRT were
  not executed: the provided local xwin SDK was reused. Only the pinned xwin Mac
  executable and argument surface were exercised.
- Full vcpkg archive extraction, FidelityFX clone and pinned packs fetch were not
  run through setup; local copies were reused. The published vcpkg ZIP was fully
  hashed, and the shader archive was really downloaded/extracted separately.
- Windows: no winget commands, native tool discovery, xwin download/splat, CMake
  configure/Ninja compile, Windows locking/process detection, SignTool signing,
  osslsigncode signing on Windows, or real Device Portal installation were run.
- Linux: no apt/pacman/dnf commands, native tool discovery, xwin download/splat,
  CMake configure/Ninja compile, Linux locking/process detection, osslsigncode
  build/signing, or real Device Portal installation were run.
- No real Xbox deployment or gameplay test was performed on any OS. HTTP multipart,
  CSRF, authentication failures and install-state handling were tested with mocks.
- Full macOS Metal/shader-tool builds and unrelated native runtime test suites were
  not run; this change's checks cover the builder and existing Python pack suite.
