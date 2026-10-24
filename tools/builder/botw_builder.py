#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Build and sign a private Xbox package from the player's own decrypted Wii U dump."""

from __future__ import annotations

import argparse
import base64
import calendar
import contextlib
import getpass
import hashlib
import http.client
import json
import os
import platform
import re
import shutil
import ssl
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.parse
import urllib.request
import uuid
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DATA = Path.home() / ".botw-builder"
RELEASE = "https://github.com/NspxMiguel/botw-xbox/releases/download/deps-uwp"
VCPKG_SHA256 = "3b304cbb65d29c08f66ca2de3328855a7b4f7a745187bde6e52a1c8358e0ac6a"
FFX_SHA256 = "7b57fd0a3f0e4e0fe24f3bb881f21480754842ff887b96ec9398b857f950e3de"
PACKS_COMMIT = "975f3759d7837da5a374150347d3202360b13ca3"
# Last published xwin release with binaries for both macOS architectures.
XWIN_VERSION = "0.6.6-rc.2"
XWIN_HASHES = {
    "aarch64-apple-darwin": "f3c43c6cd73aa641e8979c67fe724b67bf81723719d6786adbcdca8e5a1975f9",
    "x86_64-apple-darwin": "541fed25bf09ad3c1be29e4cd9b867192ec90c99a5b37e314df8dd2cf41645b0",
    "aarch64-unknown-linux-musl": "8c4bdc2c711f1e18f18ccbfd96edd3bb26dd914a867f0cff5ca53121c156b927",
    "x86_64-unknown-linux-musl": "577525d2ff6a135d4ae882c8c9f6f38c67b1d3d9fad8c8b41d7e55199741326a",
    "x86_64-pc-windows-msvc": "9720c28a52ab10ecb672dd6aacf781efe1cce9e27910f8a9b4c12f7dcf7c0bf6",
}
MICROSOFT_NOTICE = (
    "Microsoft CRT and Windows SDK are licensed by Microsoft, not this project.\n"
    "Microsoft license notice (also shown by xwin):\n"
    "Do you accept the license at https://go.microsoft.com/fwlink/?LinkId=2086102 (yes | no)?"
)
REUSE_FLAGS = ("xwin_dir", "vcpkg_dir", "ffx_source", "ffx_shaders", "graphic_packs")
TOOLS = (
    "clang-cl",
    "lld-link",
    "llvm-lib",
    "llvm-rc",
    "llvm-mt",
    "llvm-mc",
    "cmake",
    "ninja",
    "git",
    "uv",
    "openssl",
)
MIN_FREE = 6 * 1024**3


class BuilderError(Exception):
    """An actionable builder failure."""


@contextlib.contextmanager
def step(label):
    started = time.monotonic()
    print(f"\n== {label} ==", flush=True)
    try:
        yield
    finally:
        print(f"{label}: {time.monotonic() - started:.2f} s", flush=True)


def run(command, **kwargs):
    command = [str(part) for part in command]
    print("+ " + subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, check=True, **kwargs)


def external_path(path):
    path = Path(path).expanduser().resolve()
    for parent in (path, *path.parents):
        if (parent / ".git").exists() or (parent / ".hg").exists():
            raise BuilderError(f"Keep builder data and output outside every repository: {path}")
    return path


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def expected_rpx():
    source = (ROOT / "runtime/src/host/uwp/setup.h").read_text(encoding="utf-8")
    match = re.search(r'expected_rpx\[\]\s*=\s*"([a-f0-9]{64})"', source)
    if not match:
        raise BuilderError("Cannot read the supported RPX hash from runtime/src/host/uwp/setup.h")
    return match[1]


def verify_dump(game):
    game = Path(game).expanduser().resolve()
    missing = [name for name in ("code", "content", "meta") if not (game / name).is_dir()]
    if missing:
        raise BuilderError(
            f"Decrypted dump is missing folders: {', '.join(missing)}. Select the merged code/content/meta parent folder."
        )
    rpx = game / "code/U-King.rpx"
    if not rpx.is_file():
        raise BuilderError(
            f"Missing {rpx}. Supply your own decrypted Wii U dump with update 1.5.0 (v208) merged."
        )
    actual, expected = sha256(rpx), expected_rpx()
    if actual != expected:
        raise BuilderError(
            f"Unsupported U-King.rpx (SHA-256 {actual}; expected {expected}). Merge update 1.5.0 (v208) into your own decrypted Wii U dump."
        )
    print(f"Verified Wii U v208: {rpx}")
    return rpx


def tool_dirs():
    directories = [
        Path("/opt/homebrew/opt/llvm/bin"),
        Path("/opt/homebrew/opt/lld/bin"),
        Path("/opt/homebrew/opt/openssl@3/bin"),
        Path("/usr/local/opt/llvm/bin"),
        Path("/usr/local/opt/lld/bin"),
        Path("/usr/local/opt/openssl@3/bin"),
        Path("/usr/lib/llvm/bin"),
        Path("/usr/bin"),
    ]
    directories += sorted(Path("/usr/lib").glob("llvm-*/bin"), reverse=True)
    for variable in ("ProgramFiles", "ProgramFiles(x86)"):
        base = Path(os.environ.get(variable, "C:/Program Files"))
        directories += [base / "LLVM/bin", base / "CMake/bin", base / "OpenSSL-Win64/bin"]
        directories += sorted((base / "Windows Kits/10/bin").glob("*/x64"), reverse=True)
    return directories


def find_tool(name):
    found = shutil.which(name)
    if found:
        return found
    for directory in tool_dirs():
        for suffix in (".exe", "") if os.name == "nt" else ("",):
            path = directory / (name + suffix)
            if path.is_file() and os.access(path, os.X_OK):
                return str(path)
    return None


def find_signer():
    osslsigncode = find_tool("osslsigncode")
    if osslsigncode:
        version = subprocess.check_output([osslsigncode, "--version"], text=True)
        match = re.search(r"osslsigncode (\d+)\.(\d+)", version)
        # 2.7 added APPX; 2.9 corrected its central-directory ordering.
        if match and tuple(map(int, match.groups())) >= (2, 9):
            return osslsigncode
    return find_tool("signtool") if os.name == "nt" else None


def install_commands():
    system = platform.system()
    if system == "Darwin":
        return ["brew install llvm lld cmake ninja osslsigncode openssl git uv"]
    if system == "Windows":
        return [
            f"winget install --exact --id {package}"
            for package in (
                "LLVM.LLVM",
                "Kitware.CMake",
                "Ninja-build.Ninja",
                "Git.Git",
                "astral-sh.uv",
                "ShiningLight.OpenSSL.Dev",
            )
        ] + [
            "Install osslsigncode with APPX support, or install SignTool: winget install --exact --id Microsoft.WindowsSDK.10.0.26100"
        ]
    if system == "Linux":
        distro = platform.freedesktop_os_release()
        identifiers = (distro.get("ID", "") + " " + distro.get("ID_LIKE", "")).split()
        if "arch" in identifiers:
            commands = [
                "sudo pacman -S --needed clang llvm lld cmake ninja osslsigncode openssl git python uv"
            ]
        elif any(value in identifiers for value in ("fedora", "rhel", "centos")):
            commands = [
                "sudo dnf install clang clang-tools-extra llvm lld cmake ninja-build osslsigncode openssl git python3"
            ]
        else:
            commands = [
                "sudo apt update",
                "sudo apt install clang llvm lld cmake ninja-build osslsigncode openssl git python3",
            ]
        if not find_tool("uv") and "arch" not in identifiers:
            commands += ["curl -LsSf https://astral.sh/uv/install.sh | sh"]
        return commands
    raise BuilderError(f"Unsupported OS: {system}. Use macOS, Linux or Windows.")


def doctor(_args=None):
    print(
        f"BotW Builder: {platform.system()} {platform.machine()}, Python {platform.python_version()}"
    )
    missing = []
    for name in TOOLS:
        path = find_tool(name)
        print(f"{'OK' if path else 'MISSING'} {name}: {path or 'not found'}")
        if not path:
            missing.append(name)
    signer = find_signer()
    print(
        f"{'OK' if signer else 'MISSING'} signer: {signer or 'osslsigncode >= 2.9 or SignTool required'}"
    )
    if not signer:
        missing.append("signer")
    if find_tool("cmake"):
        version = subprocess.check_output([find_tool("cmake"), "--version"], text=True)
        match = re.search(r"version (\d+)\.(\d+)", version)
        if not match or tuple(map(int, match.groups())) < (3, 24):
            missing.append("CMake >= 3.24")
            print("MISSING CMake >= 3.24")
    print("Install prerequisites:\n" + "\n".join(install_commands()))
    if not signer and platform.system() == "Linux":
        print(
            "If your distribution ships osslsigncode < 2.9, install its C/C++ compiler, "
            "pkg-config and OpenSSL development headers, then run:\n"
            "git clone --depth 1 --branch 2.14 https://github.com/mtrojnar/osslsigncode.git /tmp/botw-osslsigncode\n"
            "cmake -S /tmp/botw-osslsigncode -B /tmp/botw-osslsigncode/build -DCMAKE_BUILD_TYPE=Release\n"
            "cmake --build /tmp/botw-osslsigncode/build\n"
            "sudo cmake --install /tmp/botw-osslsigncode/build"
        )
    if missing:
        raise BuilderError("Missing prerequisites: " + ", ".join(missing))
    print("Doctor passed. Next: setup (toolchain data is checked separately).")


def download(url, destination, digest):
    destination = Path(destination)
    if destination.is_file() and sha256(destination) == digest:
        return
    temporary = destination.with_suffix(destination.suffix + ".partial")
    try:
        with urllib.request.urlopen(url, timeout=120) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output, 1024 * 1024)
        if sha256(temporary) != digest:
            raise BuilderError(
                f"SHA-256 mismatch for {url}; release content changed. Refusing extraction."
            )
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def extract(archive, destination):
    """Extract data only; reject traversal, symlinks and special files on Python 3.11 too."""
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)

    def target(name):
        if "\\" in name or ":" in name:
            raise BuilderError(f"Unsafe archive path: {name}")
        path = (destination / name).resolve()
        if not path.is_relative_to(destination):
            raise BuilderError(f"Unsafe archive path: {name}")
        return path

    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as source:
            for entry in source.infolist():
                mode = entry.external_attr >> 16
                if mode & 0o170000 == 0o120000:
                    raise BuilderError(f"Archive symlink rejected: {entry.filename}")
                path = target(entry.filename)
                if entry.is_dir():
                    path.mkdir(parents=True, exist_ok=True)
                else:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    with source.open(entry) as stream, path.open("wb") as output:
                        shutil.copyfileobj(stream, output)
    else:
        with tarfile.open(archive) as source:
            for entry in source:
                path = target(entry.name)
                if entry.isdir():
                    path.mkdir(parents=True, exist_ok=True)
                elif entry.isfile():
                    path.parent.mkdir(parents=True, exist_ok=True)
                    with source.extractfile(entry) as stream, path.open("wb") as output:
                        shutil.copyfileobj(stream, output)
                    path.chmod(entry.mode & 0o777)
                else:
                    raise BuilderError(f"Archive link/special file rejected: {entry.name}")


def fetch_archive(data, name, url, digest, destination, marker=None):
    # A failed extraction is never mistaken for a complete setup on the next run.
    with tempfile.TemporaryDirectory(prefix="download-", dir=data) as directory:
        directory = Path(directory)
        archive = directory / name
        download(url, archive, digest)
        unpacked = directory / "unpacked"
        extract(archive, unpacked)
        if marker and not (unpacked / marker).exists():
            candidates = list(unpacked.glob("*/" + marker))
            if len(candidates) != 1:
                raise BuilderError(f"Archive {name} has no unique {marker} root.")
            unpacked = candidates[0].parent
        unpacked.replace(destination)


def accept_microsoft_license(args):
    print(MICROSOFT_NOTICE, flush=True)
    if args.accept_microsoft_license:
        return
    if not sys.stdin.isatty() or input().strip().lower() != "yes":
        raise BuilderError(
            "Microsoft license not accepted. Read the notice and rerun setup with --accept-microsoft-license, or answer yes interactively."
        )


def fetch_xwin(data, args):
    accept_microsoft_license(args)
    machine = platform.machine().lower()
    arch = (
        "aarch64"
        if machine in ("arm64", "aarch64")
        else "x86_64"
        if machine in ("x86_64", "amd64")
        else machine
    )
    system = {
        "Darwin": "apple-darwin",
        "Linux": "unknown-linux-musl",
        "Windows": "pc-windows-msvc",
    }[platform.system()]
    triple = f"{arch}-{system}"
    if triple not in XWIN_HASHES:
        raise BuilderError(
            f"No pinned xwin binary for {triple}; supply --xwin-dir from a supported host."
        )
    binary_root = data / f"xwin-{XWIN_VERSION}-{triple}"
    if not binary_root.exists():
        name = f"xwin-{XWIN_VERSION}-{triple}.tar.gz"
        fetch_archive(
            data,
            name,
            f"https://github.com/Jake-Shadle/xwin/releases/download/{XWIN_VERSION}/{name}",
            XWIN_HASHES[triple],
            binary_root,
        )
    binaries = list(binary_root.rglob("xwin.exe" if os.name == "nt" else "xwin"))
    binaries = [path for path in binaries if path.is_file()]
    if len(binaries) != 1:
        raise BuilderError(f"Cannot locate xwin executable in {binary_root}")
    destination = data / "xwin"
    with tempfile.TemporaryDirectory(prefix="xwin-setup-", dir=data) as directory:
        directory = Path(directory)
        cache, sdk = directory / "cache", directory / "sdk"
        command = [
            binaries[0],
            "--accept-license",
            "--arch",
            "x86_64",
            "--sdk-version",
            "10.0.26100",
            "--cache-dir",
            cache,
        ]
        # xwin's normal splat prunes the store CRT even though it is in the
        # desktop package. Preserve those Microsoft-supplied UWP libraries first.
        run([*command, "unpack"], cwd=data)
        stores = [
            p.parent
            for p in cache.rglob("*")
            if p.name.lower() == "msvcprt.lib"
            and p.parent.name.lower() == "store"
            and p.parent.parent.name.lower() in ("x64", "x86_64")
        ]
        if len(stores) != 1:
            raise BuilderError(
                "xwin did not unpack one x64 store CRT; supply --xwin-dir with crt/lib/x86_64/store."
            )
        shutil.copytree(stores[0], sdk / "crt/lib/x86_64/store")
        symlink_options = ["--disable-symlinks"] if os.name == "nt" else []
        run([*command, "splat", "--output", sdk, *symlink_options], cwd=data)
        sdk.replace(destination)
    return destination


def validate_dependencies(paths):
    checks = {
        "xwin_dir": (
            "crt/include",
            "crt/lib/x86_64/store/msvcprt.lib",
            "sdk/lib/um/x86_64/WindowsApp.lib",
        ),
        "vcpkg_dir": ("x64-uwp/include", "x64-uwp/lib", "x64-windows/include/directx-dxc/dxcapi.h"),
        "ffx_source": ("sdk/src/backends/dx12/ffx_dx12.cpp", "LICENSE.txt"),
        "ffx_shaders": ("compiled.stamp",),
        "graphic_packs": ("Mods/FPS++/rules.txt",),
    }
    for name, markers in checks.items():
        for marker in markers:
            if not (Path(paths[name]) / marker).exists():
                raise BuilderError(
                    f"Invalid --{name.replace('_', '-')}: missing {Path(paths[name]) / marker}. Run setup or supply the correct reuse directory."
                )
    backend = Path(paths["ffx_source"]) / checks["ffx_source"][0]
    if sha256(backend) != "3bad3a805daead55b061f8594e7fb789991eb466852757cf508ec425fc7066aa":
        raise BuilderError("FidelityFX source must be unmodified v1.1.4.")


def load_config(data):
    path = data / "setup.json"
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}


def setup(args):
    data = external_path(args.data_dir)
    data.mkdir(parents=True, exist_ok=True)
    paths = load_config(data)
    defaults = {
        "xwin_dir": data / "xwin",
        "vcpkg_dir": data / "vcpkg-uwp",
        "ffx_source": data / "fidelityfx-1.1.4",
        "ffx_shaders": data / "ffx-shaders-1.1.4",
        "graphic_packs": data / "cemu_graphic_packs/src/BreathOfTheWild",
    }
    for name, default in defaults.items():
        paths[name] = str(
            Path(getattr(args, name, None) or paths.get(name, default)).expanduser().resolve()
        )
    with step("Microsoft CRT/SDK"):
        if not getattr(args, "xwin_dir", None) and not Path(paths["xwin_dir"]).exists():
            paths["xwin_dir"] = str(fetch_xwin(data, args))
        else:
            print(
                f"Reusing {paths['xwin_dir']} (no Microsoft download or license acceptance performed)."
            )
    for name, archive, digest in (
        ("vcpkg_dir", "vcpkg-uwp-deps.zip", VCPKG_SHA256),
        ("ffx_shaders", "ffx-shaders-1.1.4.tar.gz", FFX_SHA256),
    ):
        with step(name):
            destination = Path(paths[name])
            if not getattr(args, name, None) and not destination.exists():
                fetch_archive(
                    data,
                    archive,
                    f"{RELEASE}/{archive}",
                    digest,
                    destination,
                    marker="compiled.stamp" if name == "ffx_shaders" else None,
                )
            print(paths[name])
    with step("FidelityFX source (MIT)"):
        destination = Path(paths["ffx_source"])
        if not getattr(args, "ffx_source", None) and not destination.exists():
            with tempfile.TemporaryDirectory(prefix="ffx-clone-", dir=data) as directory:
                checkout = Path(directory) / "source"
                run(
                    [
                        find_tool("git") or "git",
                        "clone",
                        "--depth",
                        "1",
                        "--branch",
                        "v1.1.4",
                        "--no-checkout",
                        "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git",
                        checkout,
                    ]
                )
                checkout_without_personal_instructions(checkout, "HEAD")
                checkout.replace(destination)
        print(destination)
    with step("Graphic packs (CC0)"):
        destination = Path(paths["graphic_packs"])
        if not getattr(args, "graphic_packs", None) and not destination.exists():
            checkout = data / "cemu_graphic_packs"
            if checkout.exists():
                raise BuilderError(
                    f"Incomplete packs checkout: {checkout}; supply --graphic-packs or move it aside."
                )
            with tempfile.TemporaryDirectory(prefix="packs-clone-", dir=data) as directory:
                clone = Path(directory) / "source"
                git = find_tool("git") or "git"
                run([git, "init", clone])
                run(
                    [
                        git,
                        "-C",
                        clone,
                        "remote",
                        "add",
                        "origin",
                        "https://github.com/cemu-project/cemu_graphic_packs.git",
                    ]
                )
                run([git, "-C", clone, "fetch", "--depth", "1", "origin", PACKS_COMMIT])
                checkout_without_personal_instructions(clone, "FETCH_HEAD")
                clone.replace(checkout)
        print(destination)
    validate_dependencies(paths)
    temporary = data / "setup.json.partial"
    temporary.write_text(
        json.dumps({name: paths[name] for name in REUSE_FLAGS}, indent=2) + "\n", encoding="utf-8"
    )
    temporary.replace(data / "setup.json")
    print(f"Setup ready: {data / 'setup.json'}\nNext: build --game /path/to/your/decrypted-dump")


def checkout_without_personal_instructions(checkout, revision):
    git = find_tool("git") or "git"
    run([git, "-C", checkout, "config", "core.sparseCheckout", "true"])
    (checkout / ".git/info/sparse-checkout").write_text(
        "/*\n!CLAUDE.md\n!**/CLAUDE.md\n", encoding="utf-8"
    )
    run([git, "-C", checkout, "checkout", "--detach", revision])


def manifest_identity():
    root = ET.parse(ROOT / "runtime/src/host/uwp/Package.appxmanifest").getroot()
    return root.find("{*}Identity").attrib


def certificate(data):
    identity = manifest_identity()
    publisher = identity["Publisher"]
    if not re.fullmatch(r"CN=[A-Za-z0-9 ._-]+", publisher):
        raise BuilderError(f"Unsupported certificate Publisher: {publisher}")
    directory = data / "certificates"
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    pfx, cer = directory / "BotWRecomp.pfx", directory / "BotWRecomp.cer"
    marker = directory / "publisher.txt"
    if pfx.exists() or cer.exists():
        if not (
            pfx.is_file()
            and cer.is_file()
            and marker.is_file()
            and marker.read_text().strip() == publisher
        ):
            raise BuilderError(
                f"Incomplete certificate or Publisher changed in {directory}; preserve the old certificate and move the directory aside before retrying."
            )
        run(
            [
                find_tool("openssl"),
                "x509",
                "-inform",
                "DER",
                "-in",
                cer,
                "-checkend",
                "86400",
                "-noout",
            ]
        )
        return pfx, cer
    with tempfile.TemporaryDirectory(prefix="certificate-", dir=directory) as temporary:
        temporary = Path(temporary)
        key, pem = temporary / "key.pem", temporary / "cert.pem"
        openssl = find_tool("openssl")
        run(
            [
                openssl,
                "req",
                "-x509",
                "-newkey",
                "rsa:2048",
                "-sha256",
                "-nodes",
                "-days",
                "3650",
                "-subj",
                "/" + publisher,
                "-addext",
                "keyUsage=critical,digitalSignature",
                "-addext",
                "extendedKeyUsage=codeSigning",
                "-addext",
                "basicConstraints=critical,CA:FALSE",
                "-keyout",
                key,
                "-out",
                pem,
            ]
        )
        run(
            [
                openssl,
                "pkcs12",
                "-export",
                "-out",
                temporary / "identity.pfx",
                "-inkey",
                key,
                "-in",
                pem,
                "-passout",
                "pass:",
            ]
        )
        run([openssl, "x509", "-in", pem, "-outform", "DER", "-out", temporary / "public.cer"])
        (temporary / "identity.pfx").chmod(0o600)
        (temporary / "identity.pfx").replace(pfx)
        (temporary / "public.cer").replace(cer)
        marker.write_text(publisher + "\n", encoding="utf-8")
    return pfx, cer


def sign_package(unsigned, signed, pfx, cer):
    signer = find_signer()
    if not signer:
        raise BuilderError("Install osslsigncode >= 2.9 or SignTool; run doctor for commands.")
    if Path(signer).stem.lower() != "signtool":
        run(
            [
                signer,
                "sign",
                "-pkcs12",
                pfx,
                "-pass",
                "",
                "-h",
                "sha256",
                "-in",
                unsigned,
                "-out",
                signed,
            ]
        )
        with tempfile.TemporaryDirectory(prefix="verify-", dir=signed.parent) as directory:
            pem = Path(directory) / "cert.pem"
            run([find_tool("openssl"), "x509", "-inform", "DER", "-in", cer, "-out", pem])
            run([signer, "verify", "-CAfile", pem, "-in", signed])
    else:
        shutil.copyfile(unsigned, signed)
        run([signer, "sign", "/fd", "SHA256", "/f", pfx, "/p", "", signed])
        # SignTool's trust check requires importing into a certificate store.
        # Avoid changing machine state; require the embedded APPX signature below.
    with zipfile.ZipFile(signed) as package:
        if not package.read("AppxSignature.p7x").startswith(b"PKCX"):
            raise BuilderError("Signed APPX has no valid signature container.")


def check_space(path):
    ancestor = path
    while not ancestor.exists():
        ancestor = ancestor.parent
    if os.name != "nt":
        run(["df", "-h", ancestor])
    free = shutil.disk_usage(ancestor).free
    print(f"Free disk space: {free / 1024**3:.2f} GiB", flush=True)
    if free < MIN_FREE:
        raise BuilderError(
            f"Build stopped: need at least 6 GiB free; only {free / 1024**3:.2f} GiB available. Free {(MIN_FREE - free) / 1024**3:.2f} GiB before retrying (about 15 GB recommended)."
        )


def ninja_running():
    if os.name == "nt":
        result = subprocess.check_output(
            ["tasklist", "/FI", "IMAGENAME eq ninja.exe", "/FO", "CSV", "/NH"], text=True
        )
        return '"ninja.exe"' in result.lower()
    result = subprocess.run(["pgrep", "-x", "ninja"], capture_output=True, check=False)
    if result.returncode not in (0, 1):
        raise BuilderError("Cannot check for other ninja processes.")
    return result.returncode == 0


@contextlib.contextmanager
def ninja_slot(data):
    # Cross-process lock for builders, plus detection of maintainer ninja jobs.
    lock_path = Path(tempfile.gettempdir()) / f"botw-builder-{getpass.getuser()}.ninja.lock"
    with lock_path.open("a+b") as lock:
        if os.name == "nt":
            import msvcrt

            lock.seek(0)
            lock.write(b"0")
            lock.flush()
            lock.seek(0)
            while True:
                try:
                    msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    print("Waiting for another builder's ninja...", flush=True)
                    time.sleep(5)
        else:
            import fcntl

            while True:
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    print("Waiting for another builder's ninja...", flush=True)
                    time.sleep(5)
        try:
            while ninja_running():
                print("Waiting for the machine's existing ninja process...", flush=True)
                time.sleep(5)
            yield
        finally:
            if os.name == "nt":
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock, fcntl.LOCK_UN)


def build(args):
    data = external_path(args.data_dir)
    out = external_path(args.out or data / "output")
    with step("Build preflight"):
        check_space(out)
        rpx = verify_dump(args.game)
        doctor()
        paths = load_config(data)
        for name in REUSE_FLAGS:
            if getattr(args, name, None):
                paths[name] = str(Path(getattr(args, name)).expanduser().resolve())
        if any(name not in paths for name in REUSE_FLAGS):
            raise BuilderError(
                "Toolchain data missing. Run setup first or supply all five reuse flags."
            )
        validate_dependencies(paths)
    data.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    # Game-derived sources and binaries never touch the source checkout.
    with tempfile.TemporaryDirectory(prefix="builder-work-", dir=out) as directory:
        work = Path(directory)
        generated, compiled, stage = work / "generated", work / "build", work / "stage"
        with step(f"Recompile ({args.preset}, Series X)"):
            command = [sys.executable, "-I", ROOT / "tools/packs/recompile.py", rpx, generated]
            if args.preset == "remaster":
                command += [
                    "--preset",
                    "remaster",
                    "--console",
                    "series_x",
                    "--graphic-packs",
                    paths["graphic_packs"],
                ]
            else:
                selection = work / "vanilla.json"
                selection.write_text(
                    '{"version":1,"title_id":"00050000101c9400","packs":[]}\n',
                    encoding="utf-8",
                )
                command += ["--packs", selection]
            run([*command, "--llvm-mc", find_tool("llvm-mc")], cwd=ROOT)
        with ninja_slot(data):
            with step("CMake configure"):
                run(
                    [
                        find_tool("cmake"),
                        "-S",
                        ROOT,
                        "-B",
                        compiled,
                        "-G",
                        "Ninja",
                        f"-DCMAKE_MAKE_PROGRAM={find_tool('ninja')}",
                        f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'cmake/toolchains/uwp-clang-cl.cmake'}",
                        "-DCMAKE_BUILD_TYPE=Release",
                        "-DBOTW_PLATFORM=uwp",
                        "-DBOTW_RENDERER=d3d12",
                        "-DBOTW_BUILD_GAME=ON",
                        f"-DBOTW_GENERATED_DIR={generated}",
                        f"-DXWIN_ROOT={paths['xwin_dir']}",
                        f"-DVCPKG_UWP_ROOT={paths['vcpkg_dir']}",
                        f"-DBOTW_FFX_SOURCE={paths['ffx_source']}",
                        f"-DBOTW_FFX_SHADERS_DIR={paths['ffx_shaders']}",
                        f"-DPython3_EXECUTABLE={sys.executable}",
                    ]
                )
            with step("Ninja compile and link"):
                run([find_tool("ninja"), "-C", compiled, "-j", args.jobs, "BotWRecomp"])
        with step("Stage Runtime"):
            run(
                [
                    find_tool("cmake"),
                    "--install",
                    compiled,
                    "--component",
                    "Runtime",
                    "--prefix",
                    stage,
                ]
            )
            manifest = stage / "AppxManifest.xml"
            text = manifest.read_text(encoding="utf-8")
            text, count = re.subn(
                r'(<Identity\b[^>]*\bVersion=")[^"]+("[^>]*>)',
                lambda m: m[1] + args.version + m[2],
                text,
                count=1,
            )
            if count != 1:
                raise BuilderError("Cannot set staged package version.")
            manifest.write_text(text, encoding="utf-8")
        with step("Pack APPX"):
            unsigned = work / "unsigned.appx"
            run([sys.executable, ROOT / "tools/pack_appx.py", stage, unsigned])
        with step("Certificate and signing"):
            pfx, cer = certificate(data)
            signed = work / "BotWRecomp.appx"
            sign_package(unsigned, signed, pfx, cer)
            output = out / "BotWRecomp.appx"
            public = out / "BotWRecomp.cer"
            shutil.copyfile(cer, public)
            signed.replace(output)
    (data / "last-build.json").write_text(
        json.dumps({"appx": str(output), "cer": str(public)}) + "\n", encoding="utf-8"
    )
    print(
        f"\nSigned package: {output}\nPublic certificate: {public}\nAPPX size: {output.stat().st_size} bytes\nBuild intermediates removed.\nNext: uv run tools/builder/botw_builder.py install --xbox YOUR_XBOX_IP --out {subprocess.list2cmdline([str(out)])}\nIn the Xbox launcher, select your own game data from USB. Do not share the APPX."
    )


class DevicePortal:
    """Minimal streaming Windows Device Portal client; credentials remain in memory."""

    def __init__(self, host, user=None, password=None):
        parsed = urllib.parse.urlsplit("https://" + host)
        if not parsed.hostname or parsed.path or parsed.query or parsed.fragment or parsed.username:
            raise BuilderError("--xbox must be an IP/hostname, optionally with :11443.")
        self.host, self.port = parsed.hostname, parsed.port or 11443
        self.token = None
        self.auth = (
            "Basic " + base64.b64encode(f"{user}:{password or ''}".encode()).decode()
            if user
            else None
        )

    def request(self, method, path, files=None):
        # The Xbox portal always has its own self-signed TLS certificate.
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        connection = http.client.HTTPSConnection(self.host, self.port, context=context, timeout=120)
        headers = {"Authorization": self.auth} if self.auth else {}
        if self.token:
            headers["Cookie"] = f"CSRF-Token={self.token}"
            if method != "GET":
                headers["X-CSRF-Token"] = self.token
        try:
            if files:
                boundary = "botw-" + uuid.uuid4().hex
                parts = []
                for file in files:
                    prefix = (
                        f'--{boundary}\r\nContent-Disposition: form-data; name="{file.name}"; filename="{file.name}"\r\nContent-Type: application/octet-stream\r\n\r\n'
                    ).encode()
                    parts.append((prefix, file))
                tail = f"--{boundary}--\r\n".encode()
                headers["Content-Type"] = f"multipart/form-data; boundary={boundary}"
                headers["Content-Length"] = str(
                    sum(len(prefix) + file.stat().st_size + 2 for prefix, file in parts) + len(tail)
                )
                connection.putrequest(method, path)
                for key, value in headers.items():
                    connection.putheader(key, value)
                connection.endheaders()
                for prefix, file in parts:
                    connection.send(prefix)
                    with file.open("rb") as stream:
                        while chunk := stream.read(1024 * 1024):
                            connection.send(chunk)
                    connection.send(b"\r\n")
                connection.send(tail)
            else:
                connection.request(method, path, headers=headers)
            response = connection.getresponse()
            match = re.search(
                r"CSRF-Token=([^;,\s]+)",
                ";".join(response.headers.get_all("Set-Cookie", [])),
                re.IGNORECASE,
            )
            if match:
                self.token = match[1]
            body = response.read().decode("utf-8", errors="replace")
            if 300 <= response.status < 400:
                raise BuilderError(
                    "Device Portal redirected to setup. Set credentials in Xbox Dev Home > Remote Access."
                )
            if response.status == 401:
                raise BuilderError(
                    "Device Portal refused the credentials. Check Xbox Dev Home > Remote Access."
                )
            return response.status, body
        finally:
            connection.close()

    def install(self, files):
        status, body = self.request("GET", "/api/os/machinename")
        if status != 200:
            raise BuilderError(f"Device Portal connection failed ({status}): {body[:200]}")
        for _ in range(60):
            status, body = self.request(
                "POST",
                "/api/app/packagemanager/package?package=" + urllib.parse.quote(files[0].name),
                files,
            )
            if status in (200, 202):
                break
            if status != 409:
                raise BuilderError(f"Package upload failed ({status}): {body[:300]}")
            print("Console busy; retrying in 5 seconds...", flush=True)
            time.sleep(5)
        else:
            raise BuilderError("Console stayed busy for five minutes.")
        started, idle, progress = time.monotonic(), 0, False
        while time.monotonic() - started < 15 * 60:
            time.sleep(2)
            status, body = self.request("GET", "/api/app/packagemanager/state")
            done, code, message = portal_state(status, body)
            if message:
                print(message, flush=True)
            if done:
                idle += 1
                if idle >= 2 and (progress or time.monotonic() - started > 8):
                    print("Installed. Open BotW Xbox Recomp from Dev Home.")
                    return
            else:
                idle, progress = 0, True
                if code not in (0, 3805991009):
                    raise BuilderError(f"Install failed ({code}): {message}")
        raise BuilderError("Install timed out after 15 minutes.")


def portal_state(status, body):
    if status in (200, 204) and not body.strip():
        return True, 0, ""
    try:
        parsed = json.loads(body)
    except ValueError:
        return status == 200, status, body[:200]
    code = int(parsed.get("Code", 0))
    return status == 200 and code == 0, code, str(parsed.get("Reason", parsed.get("CodeText", "")))


def install(args):
    data = external_path(args.data_dir)
    if args.out:
        out = external_path(args.out)
        files = [out / "BotWRecomp.appx", out / "BotWRecomp.cer"]
    elif (data / "last-build.json").exists():
        last = json.loads((data / "last-build.json").read_text(encoding="utf-8"))
        files = [external_path(last[name]) for name in ("appx", "cer")]
    else:
        files = [data / "output/BotWRecomp.appx", data / "output/BotWRecomp.cer"]
    for path in files:
        if not path.is_file() or path.stat().st_size == 0:
            raise BuilderError(
                f"Missing or empty package file: {path}. Run build first, or pass --out."
            )
    password = args.password
    if args.user and password is None:
        if not sys.stdin.isatty():
            raise BuilderError("Supply --password with --user in noninteractive mode.")
        password = getpass.getpass("Xbox Device Portal password: ")
    with step("Xbox deployment"):
        DevicePortal(args.xbox, args.user, password).install(files)


def package_version(value):
    if (
        not re.fullmatch(r"\d+\.\d+\.\d+\.\d+", value)
        or any(int(n) > 65535 for n in value.split("."))
        or int(value.split(".")[0]) == 0
    ):
        raise argparse.ArgumentTypeError(
            "Version must be four integers from 0 to 65535, with a nonzero major version (e.g. 1.0.0.0)."
        )
    return value


def default_package_version(now=None):
    # The Xbox refuses to install an older or equal version over an existing one, so every
    # rebuild gets a newer one: days since 2026-01-01 and minute of the day (UTC).
    now = now or time.gmtime()
    days = (calendar.timegm(now) - calendar.timegm((2026, 1, 1, 0, 0, 0))) // 86400
    return f"1.1.{int(days)}.{now.tm_hour * 60 + now.tm_min}"


def positive_int(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("Use a positive number of jobs.")
    return number


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    # Common options work both before and after the command.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument(
        "--data-dir",
        type=Path,
        default=argparse.SUPPRESS,
        help="Per-user toolchain/certificates directory (default: ~/.botw-builder)",
    )
    result.add_argument("--data-dir", type=Path, default=DEFAULT_DATA)
    commands = result.add_subparsers(dest="command", required=True)
    commands.add_parser("doctor", parents=[common]).set_defaults(action=doctor)
    for command, action in (("setup", setup), ("build", build)):
        sub = commands.add_parser(command, parents=[common])
        sub.set_defaults(action=action)
        for name in REUSE_FLAGS:
            sub.add_argument(
                "--" + name.replace("_", "-"),
                type=Path,
                help="Reuse an existing local directory without downloading",
            )
        if command == "setup":
            sub.add_argument(
                "--accept-microsoft-license",
                action="store_true",
                help="Explicitly accept Microsoft's CRT/SDK license before downloading",
            )
        else:
            sub.add_argument("--game", required=True, type=Path)
            sub.add_argument("--preset", choices=("remaster", "vanilla"), default="remaster")
            sub.add_argument("--out", type=Path)
            sub.add_argument("--version", type=package_version, default=default_package_version())
            sub.add_argument("--jobs", type=positive_int, default=2)
    sub = commands.add_parser("install", parents=[common])
    sub.set_defaults(action=install)
    sub.add_argument("--xbox", required=True)
    sub.add_argument("--user")
    sub.add_argument("--password")
    sub.add_argument(
        "--out",
        type=Path,
        help="Directory containing BotWRecomp.appx and BotWRecomp.cer (default: last build)",
    )
    return result


def main(argv=None):
    started = time.monotonic()
    try:
        args = parser().parse_args(argv)
        args.action(args)
        return 0
    except (
        BuilderError,
        OSError,
        ValueError,
        subprocess.CalledProcessError,
        zipfile.BadZipFile,
        tarfile.TarError,
        http.client.HTTPException,
    ) as error:
        print(f"botw-builder: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("botw-builder: interrupted; temporary build files removed.", file=sys.stderr)
        return 130
    finally:
        print(f"Total: {time.monotonic() - started:.2f} s", flush=True)


if __name__ == "__main__":
    sys.exit(main())
