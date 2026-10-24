"""Synthetic host coverage; no game files or console connection required."""

import hashlib
import io
import json
import os
import shlex
import shutil
import subprocess
import tarfile
import zipfile
from email.message import Message
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

import pytest
from tools.builder import botw_builder as builder


@pytest.fixture(autouse=True)
def private_ninja_lock(monkeypatch, tmp_path):
    # Keep the cross-process ninja lock away from real builders running on this machine.
    monkeypatch.setattr(builder.tempfile, "gettempdir", lambda: str(tmp_path))


@pytest.fixture
def dump(tmp_path):
    for folder in ("code", "content", "meta"):
        (tmp_path / folder).mkdir()
    (tmp_path / "code/U-King.rpx").write_bytes(b"synthetic RPX fixture")
    return tmp_path


def test_supported_dump_uses_runtime_hash(dump, monkeypatch):
    expected = hashlib.sha256(b"synthetic RPX fixture").hexdigest()
    monkeypatch.setattr(builder, "expected_rpx", lambda: expected)
    assert builder.verify_dump(dump) == dump / "code/U-King.rpx"


@pytest.mark.parametrize("folder", ["code", "content", "meta"])
def test_dump_missing_folder(dump, folder):
    shutil.rmtree(dump / folder)
    with pytest.raises(builder.BuilderError, match=f"missing folders: {folder}"):
        builder.verify_dump(dump)


def test_missing_rpx(dump):
    (dump / "code/U-King.rpx").unlink()
    with pytest.raises(builder.BuilderError, match="Missing .*U-King.rpx"):
        builder.verify_dump(dump)


def test_unsupported_rpx_is_actionable(dump):
    with pytest.raises(builder.BuilderError, match=r"Unsupported U-King.rpx.*1.5.0 \(v208\)"):
        builder.verify_dump(dump)


def test_runtime_hash_is_read_from_header(monkeypatch, tmp_path):
    header = tmp_path / "runtime/src/host/uwp/setup.h"
    header.parent.mkdir(parents=True)
    header.write_text('inline constexpr char expected_rpx[] = "' + "a" * 64 + '";')
    monkeypatch.setattr(builder, "ROOT", tmp_path)
    assert builder.expected_rpx() == "a" * 64
    header.write_text("unexpected format")
    with pytest.raises(builder.BuilderError, match="Cannot read"):
        builder.expected_rpx()


@pytest.mark.parametrize("command", ["doctor", "setup", "build", "install"])
def test_command_parsing(command):
    extra = {
        "doctor": [],
        "setup": [],
        "build": ["--game", "/dump"],
        "install": ["--xbox", "192.168.1.10"],
    }[command]
    args = builder.parser().parse_args([command, *extra])
    assert args.command == command
    assert args.action == getattr(builder, command)
    assert args.data_dir == builder.DEFAULT_DATA


@pytest.mark.parametrize("position", ["before", "after"])
def test_data_dir_on_both_sides_of_command(position):
    tokens = ["--data-dir", "/external/data"]
    tokens = [*tokens, "doctor"] if position == "before" else ["doctor", *tokens]
    assert builder.parser().parse_args(tokens).data_dir == Path("/external/data")


def test_build_defaults_and_reuse_arguments():
    args = builder.parser().parse_args(["build", "--game", "/dump"])
    assert args.preset == "remaster"
    assert args.jobs == 2
    tokens = [
        "build",
        "--game",
        "/dump",
        "--preset",
        "vanilla",
        "--out",
        "/out",
        "--version",
        "2.0.1.0",
        "--jobs",
        "1",
    ]
    for name in builder.REUSE_FLAGS:
        tokens += ["--" + name.replace("_", "-"), "/reuse/" + name]
    args = builder.parser().parse_args(tokens)
    assert args.preset == "vanilla"
    assert args.version == "2.0.1.0"
    for name in builder.REUSE_FLAGS:
        assert getattr(args, name) == Path("/reuse") / name


@pytest.mark.parametrize(
    "tokens",
    [
        [],
        ["build"],
        ["install"],
        ["build", "--game", "/dump", "--preset", "unknown"],
        ["setup", "--accept-license"],
        ["build", "--game", "/dump", "--jobs", "0"],
        ["build", "--game", "/dump", "--version", "1.2.3"],
        ["build", "--game", "/dump", "--version", "1.2.65536.0"],
        ["build", "--game", "/dump", "--version", "0.0.0.0"],
    ],
)
def test_invalid_arguments(tokens):
    with pytest.raises(SystemExit) as error:
        builder.parser().parse_args(tokens)
    assert error.value.code == 2


def test_license_is_never_implicitly_accepted(monkeypatch):
    monkeypatch.setattr(builder.sys, "stdin", SimpleNamespace(isatty=lambda: False))
    with pytest.raises(builder.BuilderError, match="not accepted"):
        builder.accept_microsoft_license(SimpleNamespace(accept_microsoft_license=False))


@pytest.mark.parametrize("answer,accepted", [("yes", True), ("no", False), ("", False)])
def test_interactive_microsoft_license(monkeypatch, capsys, answer, accepted):
    monkeypatch.setattr(builder.sys, "stdin", SimpleNamespace(isatty=lambda: True))
    monkeypatch.setattr("builtins.input", lambda: answer)
    if accepted:
        builder.accept_microsoft_license(SimpleNamespace(accept_microsoft_license=False))
    else:
        with pytest.raises(builder.BuilderError):
            builder.accept_microsoft_license(SimpleNamespace(accept_microsoft_license=False))
    assert "https://go.microsoft.com/fwlink/?LinkId=2086102" in capsys.readouterr().out


def test_explicit_license_flag_does_not_prompt(monkeypatch):
    monkeypatch.setattr("builtins.input", Mock(side_effect=AssertionError("unexpected prompt")))
    builder.accept_microsoft_license(SimpleNamespace(accept_microsoft_license=True))


@pytest.mark.parametrize("git_marker", ["directory", "worktree"])
def test_output_cannot_be_in_any_repository(tmp_path, git_marker):
    if git_marker == "directory":
        (tmp_path / ".git").mkdir()
    else:
        (tmp_path / ".git").write_text("gitdir: /elsewhere")
    with pytest.raises(builder.BuilderError, match="outside every repository"):
        builder.external_path(tmp_path / "new/output")


def test_low_space_stops_before_recompilation(tmp_path, monkeypatch):
    monkeypatch.setattr(builder, "run", Mock())
    monkeypatch.setattr(builder.shutil, "disk_usage", lambda _: SimpleNamespace(free=2 * 1024**3))
    verify = Mock(side_effect=AssertionError("must stop before dump/build"))
    monkeypatch.setattr(builder, "verify_dump", verify)
    args = builder.parser().parse_args(
        [
            "build",
            "--game",
            "/dump",
            "--out",
            str(tmp_path / "out"),
            "--data-dir",
            str(tmp_path / "data"),
        ]
    )
    with pytest.raises(builder.BuilderError, match="Build stopped: need at least 6 GiB"):
        builder.build(args)
    verify.assert_not_called()
    assert not (tmp_path / "out").exists()


def test_archive_traversal_rejected(tmp_path):
    archive = tmp_path / "bad.zip"
    with zipfile.ZipFile(archive, "w") as out:
        out.writestr("../escaped.txt", "synthetic")
    with pytest.raises(builder.BuilderError, match="Unsafe archive"):
        builder.extract(archive, tmp_path / "unpack")
    assert not (tmp_path / "escaped.txt").exists()


def test_tar_link_rejected(tmp_path):
    archive = tmp_path / "bad.tar.gz"
    with tarfile.open(archive, "w:gz") as out:
        entry = tarfile.TarInfo("link")
        entry.type = tarfile.SYMTYPE
        entry.linkname = "/outside"
        out.addfile(entry)
    with pytest.raises(builder.BuilderError, match="link/special"):
        builder.extract(archive, tmp_path / "unpack")


def test_download_hash_mismatch_leaves_no_file(tmp_path, monkeypatch):
    monkeypatch.setattr(
        builder.urllib.request, "urlopen", lambda *a, **k: io.BytesIO(b"wrong data")
    )
    archive = tmp_path / "data.zip"
    with pytest.raises(builder.BuilderError, match="SHA-256 mismatch"):
        builder.download("https://example.invalid/data.zip", archive, "0" * 64)
    assert not archive.exists()
    assert not archive.with_suffix(".zip.partial").exists()


@pytest.mark.parametrize(
    "status,body,expected",
    [
        (204, "", (True, 0, "")),
        (200, "", (True, 0, "")),
        (200, '{"Code":0,"Reason":"Succeeded"}', (True, 0, "Succeeded")),
        (202, '{"Code":3805991009,"CodeText":"Installing"}', (False, 3805991009, "Installing")),
        (400, '{"Code":42,"Reason":"Failed"}', (False, 42, "Failed")),
    ],
)
def test_portal_install_states(status, body, expected):
    assert builder.portal_state(status, body) == expected


@pytest.mark.parametrize(
    "host", ["https://192.168.1.10", "user@host", "host/path", "host?query=yes"]
)
def test_portal_rejects_urls_and_embedded_credentials(host):
    with pytest.raises(builder.BuilderError, match="--xbox"):
        builder.DevicePortal(host)


def test_portal_streams_multipart_with_csrf(tmp_path, monkeypatch):
    package, cer = tmp_path / "BotWRecomp.appx", tmp_path / "BotWRecomp.cer"
    package.write_bytes(b"synthetic package")
    cer.write_bytes(b"synthetic certificate")
    headers = Message()
    headers.add_header("Set-Cookie", "CSRF-Token=token-value; Path=/")
    response = SimpleNamespace(headers=headers, status=200, read=lambda: b"{}")
    connections = []

    def connect(*args, **kwargs):
        connection = Mock()
        connection.getresponse.return_value = response
        connections.append(connection)
        return connection

    monkeypatch.setattr(builder.http.client, "HTTPSConnection", connect)
    portal = builder.DevicePortal("192.168.1.10", "user", "private-password")
    portal.request("GET", "/api/os/machinename")
    portal.request("POST", "/api/app/packagemanager/package", [package, cer])
    connection = connections[-1]
    headers = dict(call.args for call in connection.putheader.call_args_list)
    assert headers["X-CSRF-Token"] == "token-value"
    assert headers["Cookie"] == "CSRF-Token=token-value"
    body = b"".join(call.args[0] for call in connection.send.call_args_list)
    assert len(body) == int(headers["Content-Length"])
    assert body.index(b"synthetic package") < body.index(b"synthetic certificate")
    assert b"private-password" not in body


@pytest.mark.parametrize("status,match", [(302, "redirected"), (401, "refused")])
def test_portal_does_not_follow_auth_redirects(monkeypatch, status, match):
    response = SimpleNamespace(headers=Message(), status=status, read=lambda: b"")
    connection = Mock()
    connection.getresponse.return_value = response
    monkeypatch.setattr(builder.http.client, "HTTPSConnection", lambda *a, **k: connection)
    with pytest.raises(builder.BuilderError, match=match):
        builder.DevicePortal("192.168.1.10").request("GET", "/api/os/machinename")
    connection.close.assert_called_once()


@pytest.mark.parametrize("preset", ["remaster", "vanilla"])
def test_build_pipeline_keeps_private_work_outside_repo_and_cleans(tmp_path, monkeypatch, preset):
    """Exercise orchestration without compiling game code or allocating a real build."""
    data, out = tmp_path / "private data", tmp_path / "private output"
    data.mkdir()
    (data / "setup.json").write_text(
        json.dumps({name: "/reuse/" + name for name in builder.REUSE_FLAGS})
    )
    for name in ("check_space", "doctor", "validate_dependencies"):
        monkeypatch.setattr(builder, name, lambda *a: None)
    monkeypatch.setattr(builder, "verify_dump", lambda _: tmp_path / "dump/code/U-King.rpx")
    monkeypatch.setattr(builder, "ninja_running", lambda: False)
    monkeypatch.setattr(builder, "find_tool", lambda name: name)
    cer = data / "public.cer"
    cer.write_bytes(b"public certificate")
    monkeypatch.setattr(builder, "certificate", lambda _: (data / "private.pfx", cer))
    commands = []

    def run(command, **kwargs):
        command = [str(x) for x in command]
        commands.append(command)
        if "--packs" in command:
            from tools.packs.build import load_manifest

            assert load_manifest(command[command.index("--packs") + 1])["packs"] == []
        if "--install" in command:
            stage = Path(command[-1])
            stage.mkdir()
            shutil.copyfile(
                builder.ROOT / "runtime/src/host/uwp/Package.appxmanifest",
                stage / "AppxManifest.xml",
            )

    def sign(unsigned, signed, pfx, public):
        assert 'Version="2.0.0.0"' in (signed.parent / "stage/AppxManifest.xml").read_text()
        signed.write_bytes(b"synthetic signed package")

    monkeypatch.setattr(builder, "run", run)
    monkeypatch.setattr(builder, "sign_package", sign)
    args = builder.parser().parse_args(
        [
            "build",
            "--game",
            "/dump",
            "--preset",
            preset,
            "--data-dir",
            str(data),
            "--out",
            str(out),
            "--version",
            "2.0.0.0",
        ]
    )
    builder.build(args)
    assert sorted(p.name for p in out.iterdir()) == ["BotWRecomp.appx", "BotWRecomp.cer"]
    assert json.loads((data / "last-build.json").read_text())["appx"] == str(
        out / "BotWRecomp.appx"
    )
    recompile = commands[0]
    assert str(builder.ROOT / "tools/packs/recompile.py") in recompile
    assert recompile[1:4] == ["-X", "utf8", "-I"]
    if preset == "remaster":
        assert recompile[recompile.index("--console") + 1] == "series_x"
    configure = next(c for c in commands if "-G" in c)
    assert "-DBOTW_BUILD_GAME=ON" in configure
    assert any(c.startswith("-DBOTW_GENERATED_DIR=" + out.as_posix()) for c in configure)
    for variable, tool in builder.CMAKE_TOOLS.items():
        assert f"-D{variable}={tool}" in configure
    assert not list(out.glob("builder-work-*"))


def test_real_synthetic_appx_signing_and_certificate_reuse(tmp_path):
    """Verify the packer/signer together, using only our own synthetic payload."""
    if not builder.find_tool("openssl") or not builder.find_signer():
        pytest.skip("OpenSSL and an APPX signer required")
    stage = tmp_path / "stage"
    stage.mkdir()
    shutil.copyfile(
        builder.ROOT / "runtime/src/host/uwp/Package.appxmanifest", stage / "AppxManifest.xml"
    )
    (stage / "BotWRecomp.exe").write_bytes(b"synthetic test payload; no game code")
    unsigned, signed = tmp_path / "unsigned.appx", tmp_path / "signed.appx"
    builder.run([builder.sys.executable, builder.ROOT / "tools/pack_appx.py", stage, unsigned])
    pfx, cer = builder.certificate(tmp_path / "data")
    before = pfx.read_bytes()
    builder.sign_package(unsigned, signed, pfx, cer)
    assert builder.certificate(tmp_path / "data") == (pfx, cer)
    assert pfx.read_bytes() == before
    if os.name != "nt":
        assert pfx.stat().st_mode & 0o777 == 0o600
    with zipfile.ZipFile(signed) as package:
        assert package.read("AppxSignature.p7x").startswith(b"PKCX")
    subject = subprocess.check_output(
        [
            builder.find_tool("openssl"),
            "x509",
            "-inform",
            "DER",
            "-in",
            cer,
            "-noout",
            "-subject",
            "-nameopt",
            "RFC2253",
        ],
        text=True,
    )
    assert subject.strip() == "subject=" + builder.manifest_identity()["Publisher"]


@pytest.mark.parametrize("override", [False, True])
def test_cmake_toolchain_discovery_and_cache_overrides(tmp_path, override):
    """Load the toolchain in script mode: no compiler test, build or Ninja is run."""
    cmake = builder.find_tool("cmake")
    if not cmake:
        pytest.skip("CMake required")
    script = tmp_path / "toolchain.cmake"
    toolchain = (builder.ROOT / "cmake/toolchains/uwp-clang-cl.cmake").as_posix()
    script.write_text(
        f'include("{toolchain}")\n'
        + "\n".join(
            f'message(STATUS "{name}=${{{name}}}")'
            for name in (
                "CMAKE_C_COMPILER",
                "CMAKE_CXX_COMPILER",
                "CMAKE_LINKER",
                "CMAKE_AR",
                "CMAKE_RC_COMPILER",
                "CMAKE_MT",
                "XWIN_ROOT",
                "VCPKG_UWP_ROOT",
                "XWIN_SDK_INCLUDE_DIR",
            )
        )
    )
    command = [cmake]
    if override:
        for name in (
            "CMAKE_C_COMPILER",
            "CMAKE_CXX_COMPILER",
            "CMAKE_LINKER",
            "CMAKE_AR",
            "CMAKE_RC_COMPILER",
            "CMAKE_MT",
        ):
            command += [f"-D{name}={Path(cmake).as_posix()}"]
        for name, folder in (
            ("XWIN_ROOT", "custom xwin"),
            ("VCPKG_UWP_ROOT", "custom vcpkg"),
            ("XWIN_SDK_INCLUDE_DIR", "custom include"),
        ):
            command += [f"-D{name}={(tmp_path / folder).as_posix()}"]
    result = subprocess.run(
        [*command, "-P", str(script)], check=True, capture_output=True, text=True
    )
    if override:
        assert "XWIN_ROOT=" + (tmp_path / "custom xwin").as_posix() in result.stdout
        assert "VCPKG_UWP_ROOT=" + (tmp_path / "custom vcpkg").as_posix() in result.stdout
        assert "XWIN_SDK_INCLUDE_DIR=" + (tmp_path / "custom include").as_posix() in result.stdout
        # Windows paths are case-insensitive and CMake may normalise the extension case.
        assert ("CMAKE_C_COMPILER=" + Path(cmake).as_posix()).lower() in result.stdout.lower()
    else:
        assert "clang-cl" in result.stdout
        assert "lld-link" in result.stdout
        assert "llvm-mt" in result.stdout


def test_archive_single_shader_root_is_normalized(tmp_path, monkeypatch):
    archive = tmp_path / "source.zip"
    with zipfile.ZipFile(archive, "w") as out:
        out.writestr("ffx-shaders-1.1.4/compiled.stamp", "synthetic")
        out.writestr("._ffx-shaders-1.1.4", "synthetic metadata")
    monkeypatch.setattr(
        builder, "download", lambda url, dest, digest: shutil.copyfile(archive, dest)
    )
    data = tmp_path / "data"
    data.mkdir()
    destination = data / "ffx-shaders-1.1.4"
    builder.fetch_archive(
        data,
        "shaders.zip",
        "https://example.invalid",
        "unused",
        destination,
        marker="compiled.stamp",
    )
    assert (destination / "compiled.stamp").read_text() == "synthetic"
    assert not (destination / "._ffx-shaders-1.1.4").exists()


@pytest.mark.parametrize("version,usable", [("2.5", False), ("2.14", True)])
def test_signer_requires_appx_capable_version(monkeypatch, version, usable):
    monkeypatch.setattr(
        builder, "find_tool", lambda name: "/tools/osslsigncode" if name == "osslsigncode" else None
    )
    monkeypatch.setattr(
        builder.subprocess, "check_output", lambda *a, **k: "osslsigncode " + version
    )
    assert bool(builder.find_signer()) == usable


def test_ninja_waits_for_existing_process(tmp_path, monkeypatch):
    running = iter([True, True, False])
    monkeypatch.setattr(builder, "ninja_running", lambda: next(running))
    sleep = Mock()
    monkeypatch.setattr(builder.time, "sleep", sleep)
    with builder.ninja_slot(tmp_path):
        assert sleep.call_count == 2
    assert all(call.args == (5,) for call in sleep.call_args_list)


def host_os(monkeypatch, name):
    # Replace only the builder's OS view; pathlib/pytest retain the real host OS.
    monkeypatch.setattr(builder, "os", SimpleNamespace(**{**vars(os), "name": name}))


@pytest.mark.parametrize("name", ["clang-cl", "lld-link", "llvm-mc"])
def test_version_suffixed_llvm_tools(tmp_path, monkeypatch, name):
    monkeypatch.setattr(builder.shutil, "which", lambda _: None)
    monkeypatch.setattr(builder, "tool_dirs", list)
    monkeypatch.setattr(builder.os, "get_exec_path", lambda: [str(tmp_path)])
    for suffix in ("9", "18", "20", "oops"):
        tool = tmp_path / f"{name}-{suffix}"
        tool.write_bytes(b"fixture")
        tool.chmod(0o755)
    assert builder.find_tool(name) == str(tmp_path / f"{name}-20")


def test_native_windows_executable_discovery(tmp_path, monkeypatch):
    host_os(monkeypatch, "nt")
    monkeypatch.setattr(builder.shutil, "which", lambda _: None)
    monkeypatch.setattr(builder, "tool_dirs", lambda: [tmp_path / "LLVM tools"])
    executable = tmp_path / "LLVM tools/clang-cl.exe"
    executable.parent.mkdir()
    executable.write_bytes(b"fixture")
    executable.chmod(0o755)
    assert builder.find_tool("clang-cl") == str(executable)


def test_windows_prefers_native_signtool(monkeypatch):
    host_os(monkeypatch, "nt")
    monkeypatch.setattr(builder, "find_tool", lambda name: f"C:/Program Files/tools/{name}.exe")
    check = Mock(side_effect=AssertionError("must not probe osslsigncode"))
    monkeypatch.setattr(builder.subprocess, "check_output", check)
    assert builder.find_signer() == "C:/Program Files/tools/signtool.exe"


def test_broken_signer_is_reported_as_missing(monkeypatch):
    monkeypatch.setattr(
        builder, "find_tool", lambda n: "osslsigncode" if n == "osslsigncode" else None
    )
    monkeypatch.setattr(
        builder.subprocess, "check_output", Mock(side_effect=OSError("missing DLL"))
    )
    assert builder.find_signer() is None


def test_signtool_keeps_spaces_and_empty_password_in_separate_arguments(tmp_path, monkeypatch):
    signer = "C:/Program Files/Windows Kits/10/bin/x64/signtool.exe"
    monkeypatch.setattr(builder, "find_signer", lambda: signer)
    unsigned, signed = tmp_path / "unsigned file.appx", tmp_path / "signed file.appx"
    pfx, cer = tmp_path / "private key.pfx", tmp_path / "public key.cer"
    with zipfile.ZipFile(unsigned, "w") as package:
        package.writestr("AppxSignature.p7x", b"PKCXmock signature")
    command = Mock()
    monkeypatch.setattr(builder, "run", command)
    builder.sign_package(unsigned, signed, pfx, cer)
    command.assert_called_once_with([signer, "sign", "/fd", "SHA256", "/f", pfx, "/p", "", signed])
    assert signed.read_bytes() == unsigned.read_bytes()


@pytest.mark.parametrize(
    "system,machine,os_name,triple",
    [
        ("Windows", "AMD64", "nt", "x86_64-pc-windows-msvc"),
        ("Linux", "x86_64", "posix", "x86_64-unknown-linux-musl"),
        ("Linux", "aarch64", "posix", "aarch64-unknown-linux-musl"),
    ],
)
def test_xwin_platform_download_and_store_crt(
    tmp_path, monkeypatch, system, machine, os_name, triple
):
    host_os(monkeypatch, os_name)
    monkeypatch.setattr(builder.platform, "system", lambda: system)
    monkeypatch.setattr(builder.platform, "machine", lambda: machine)
    fetched, commands = [], []

    def fetch(data, name, url, digest, destination):
        fetched.append((name, url, digest))
        destination.mkdir()
        (destination / ("xwin.exe" if os_name == "nt" else "xwin")).write_bytes(b"fixture")

    def run(command, **kwargs):
        commands.append(command)
        if "unpack" in command:
            store = Path(command[command.index("--cache-dir") + 1]) / "unpack/crt/lib/x64/store"
            store.mkdir(parents=True)
            (store / "msvcprt.lib").write_bytes(b"synthetic store CRT")
        else:
            sdk = Path(command[command.index("--output") + 1])
            # Match real xwin: prep_splat clears the output CRT, and splat
            # moves files out of the unpack cache. Neither location is safe.
            if sdk.exists():
                shutil.rmtree(sdk)
            sdk.mkdir()
            shutil.rmtree(Path(command[command.index("--cache-dir") + 1]) / "unpack")

    monkeypatch.setattr(builder, "fetch_archive", fetch)
    monkeypatch.setattr(builder, "run", run)
    sdk = builder.fetch_xwin(tmp_path, SimpleNamespace(accept_microsoft_license=True))
    name, url, digest = fetched[0]
    assert name == f"xwin-{builder.XWIN_VERSION}-{triple}.tar.gz"
    assert url.endswith("/" + name) and digest == builder.XWIN_HASHES[triple]
    assert (sdk / "crt/lib/x86_64/store/msvcprt.lib").read_bytes() == b"synthetic store CRT"
    assert ("--disable-symlinks" in commands[-1]) == (os_name == "nt")
    assert all("--accept-license" in command for command in commands)


def test_dependency_checkout_preserves_lf_despite_git_defaults(tmp_path):
    git = builder.find_tool("git")
    if not git:
        pytest.skip("Git required")
    repository = tmp_path / "source repository"
    checkout = tmp_path / "dependency with spaces"
    builder.run([git, "init", repository])
    source = repository / "source.txt"
    payload = "pinned UTF-8 bytes: \u00e9\nsecond line\n".encode()
    source.write_bytes(payload)
    builder.run([git, "-C", repository, "add", "source.txt"])
    builder.run(
        [
            git,
            "-C",
            repository,
            "-c",
            "user.name=Miguel",
            "-c",
            "user.email=miguel@nspx.dev",
            "-c",
            "commit.gpgsign=false",
            "commit",
            "-m",
            "Add synthetic fixture",
        ]
    )
    builder.run([git, "clone", "--no-checkout", repository, checkout])
    builder.run([git, "-C", checkout, "config", "core.autocrlf", "true"])
    builder.checkout_without_personal_instructions(checkout, "HEAD")
    assert (checkout / "source.txt").read_bytes() == payload
    assert b"\r" not in (checkout / ".git/info/sparse-checkout").read_bytes()


@pytest.mark.parametrize(
    "distro,manager", [("ubuntu", "apt"), ("arch", "pacman"), ("fedora", "dnf")]
)
def test_linux_install_guidance(monkeypatch, distro, manager):
    monkeypatch.setattr(builder.platform, "system", lambda: "Linux")
    monkeypatch.setattr(builder.platform, "freedesktop_os_release", lambda: {"ID": distro})
    monkeypatch.setattr(builder, "find_tool", lambda _: None)
    assert any(manager in command for command in builder.install_commands())


def test_posix_command_display_quotes_shell_metacharacters(monkeypatch):
    host_os(monkeypatch, "posix")
    command = ["/path with spaces/python", "/output/it's $(literal);file"]
    assert shlex.split(builder.command_line(command)) == command


@pytest.mark.parametrize(
    "output,running",
    [('"ninja.exe","1234","Console"\r\n', True), ("INFO: No tasks match.\r\n", False)],
)
def test_windows_ninja_process_detection(monkeypatch, output, running):
    host_os(monkeypatch, "nt")
    check = Mock(return_value=output)
    monkeypatch.setattr(builder.subprocess, "check_output", check)
    assert builder.ninja_running() == running
    assert check.call_args.args[0] == [
        "tasklist",
        "/FI",
        "IMAGENAME eq ninja.exe",
        "/FO",
        "CSV",
        "/NH",
    ]


@pytest.mark.parametrize("powerpc", [True, False])
def test_doctor_requires_working_powerpc_assembler(monkeypatch, powerpc):
    monkeypatch.setattr(builder, "find_tool", lambda name: name)
    monkeypatch.setattr(builder, "find_signer", lambda: "signer")
    monkeypatch.setattr(builder, "install_commands", list)
    monkeypatch.setattr(builder.subprocess, "check_output", lambda *a, **k: "cmake version 3.31")
    probe = Mock(
        return_value=SimpleNamespace(
            returncode=0 if powerpc else 1,
            stdout="encoding: [0x4e,0x80,0x00,0x20]\r\n" if powerpc else "",
        )
    )
    monkeypatch.setattr(builder.subprocess, "run", probe)
    if powerpc:
        builder.doctor()
    else:
        with pytest.raises(builder.BuilderError, match="llvm-mc PowerPC backend"):
            builder.doctor()
    assert probe.call_args.args[0] == [
        "llvm-mc",
        "--triple=powerpc-unknown-linux-gnu",
        "--show-encoding",
    ]
    assert probe.call_args.kwargs["input"] == "blr\n"


def test_default_package_version_increases():
    import time

    from tools.builder.botw_builder import default_package_version, package_version

    a = default_package_version(time.strptime("2026-10-23 09:59", "%Y-%m-%d %H:%M"))
    b = default_package_version(time.strptime("2026-10-23 10:00", "%Y-%m-%d %H:%M"))
    c = default_package_version(time.strptime("2026-10-24 00:00", "%Y-%m-%d %H:%M"))
    assert a == "1.1.295.599" and b == "1.1.295.600" and c == "1.1.296.0"
    assert package_version(
        default_package_version(time.strptime("2200-12-31 23:59", "%Y-%m-%d %H:%M"))
    )
