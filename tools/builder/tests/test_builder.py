"""Synthetic host coverage; no game files or console connection required."""

import hashlib
import io
import json
import os
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
    data, out = tmp_path / "data", tmp_path / "out"
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
    assert "tools/packs/recompile.py" in " ".join(recompile)
    if preset == "remaster":
        assert recompile[recompile.index("--console") + 1] == "series_x"
    configure = next(c for c in commands if "-G" in c)
    assert "-DBOTW_BUILD_GAME=ON" in configure
    assert any(c.startswith("-DBOTW_GENERATED_DIR=" + str(out)) for c in configure)
    assert not list(out.glob("builder-work-*"))


def test_real_synthetic_appx_signing_and_certificate_reuse(tmp_path):
    """Verify the packer/signer together, using only our own synthetic payload."""
    if not all(builder.find_tool(name) for name in ("openssl", "osslsigncode")):
        pytest.skip("OpenSSL and osslsigncode required")
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
        command += [
            "-DXWIN_ROOT=/custom/xwin",
            "-DVCPKG_UWP_ROOT=/custom/vcpkg",
            "-DXWIN_SDK_INCLUDE_DIR=/custom/include",
        ]
    result = subprocess.run(
        [*command, "-P", str(script)], check=True, capture_output=True, text=True
    )
    if override:
        assert "XWIN_ROOT=/custom/xwin" in result.stdout
        assert "VCPKG_UWP_ROOT=/custom/vcpkg" in result.stdout
        assert "XWIN_SDK_INCLUDE_DIR=/custom/include" in result.stdout
        assert "CMAKE_C_COMPILER=" + Path(cmake).as_posix() in result.stdout
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
