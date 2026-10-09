#!/usr/bin/env python3
"""Assemble Channel Card and Berry locally; optionally compare an SVN checkout.

This tool never modifies an SVN working copy or commits/tags a repository.
"""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from urllib.parse import unquote
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
INPUTS = ("channel_card", "berry_compiler", "scripts/channel_release.py",
          "scripts/svn_publish.sh")
CARD_ROOT = {"README.md",
             "channel_MCU.ioc", "startup_stm32h725xx.s", "STM32H725xG_flash.ld", "flash.sh"}
COMPILER_ROOT = {"CMakeLists.txt", "Makefile", "README.md", "berry.c",
                 "berry.linux-arm64"}
CARD_DOCS = {"PROTOCOL.md"}
NOTICES = {"LICENSE", "LICENSE.txt", "PROVENANCE.md", "README.md"}


def run(*args, cwd=ROOT):
    return subprocess.check_output(args, cwd=cwd)


def selected(path):
    """Select build inputs explicitly; unknown files are not exported."""
    parts = PurePosixPath(path).parts
    if len(parts) < 2 or any(p.startswith(".") for p in parts):
        return False
    if any(p in {"tests", "test", "fixtures", "build", "scripts", "__pycache__"}
           for p in parts):
        return False
    if parts[-1].startswith("test_") or PurePosixPath(path).stem.endswith("_test"):
        return False
    product, *tail = parts
    rel = "/".join(tail)
    suffix = PurePosixPath(path).suffix
    if product == "channel_card":
        if rel in CARD_ROOT or rel in CARD_DOCS:
            return True
        if tail[0] == "app" and len(tail) == 2:
            return suffix in {".cpp", ".h"} or tail[-1] == "Makefile"
        if tail[0] == "core" and len(tail) == 3:
            return (tail[1] == "src" and suffix == ".c" or
                    tail[1] == "inc" and suffix == ".h")
        if tail[0] == "drivers":
            return suffix in {".c", ".h"} or tail[-1] in NOTICES
        if tail[0] == "berry_runtime":
            if len(tail) == 2:
                return suffix in {".c", ".h"}
            if tail[1:3] == ["third_party", "berry"]:
                return suffix in {".c", ".h"} or tail[-1] in NOTICES
    if product == "berry_compiler":
        if rel in COMPILER_ROOT:
            return True
        if tail[0] in {"config", "shared"}:
            return suffix in {".c", ".h"}
        if tail[0] == "examples" and len(tail) == 2:
            return suffix == ".be"
        if tail[:2] == ["vendor", "berry"]:
            return suffix in {".c", ".h"} or tail[-1] in NOTICES
    return False


def destination(path):
    return path.removeprefix("channel_card/")


def markdown_links(path):
    # Ignore code examples; only actual Markdown links are package dependencies.
    text = re.sub(r"```.*?```", "", path.read_text(), flags=re.S)
    return re.findall(r"\]\(([^)]+)\)", text)


def validate(stage):
    required = [*CARD_ROOT, *CARD_DOCS,
                "app/Makefile",
                "app/cs4304.cpp",
                "app/audio.cpp",
                "app/channel.cpp",
                "app/filter.cpp",
                "app/oscillator.cpp",
                "app/voice.cpp",
                "app/samples.cpp",
                "app/stream.cpp",
                "app/usb.cpp",
                "app/cs4304.h",
                "app/audio.h",
                "app/channel.h",
                "app/filter.h",
                "app/oscillator.h",
                "app/voice.h",
                "app/samples.h",
                "app/stream.h",
                "app/usb.h",
                "berry_runtime/berry_backend.c",
                "berry_runtime/third_party/berry/LICENSE",
                "berry_runtime/third_party/berry/generate/be_const_strtab.h",
                "berry_compiler/vendor/berry/LICENSE",
                "berry_compiler/vendor/berry/generate/be_const_strtab.h",
                "berry_compiler/examples/channel_envelope.be"]
    required += ["berry_compiler/" + p for p in COMPILER_ROOT]
    for rel in required:
        if not (stage / rel).is_file():
            raise ValueError(f"missing package input: {rel}")
    binary = stage / "berry_compiler/berry.linux-arm64"
    data = binary.read_bytes()
    if data[:6] != b"\x7fELF\x02\x01" or int.from_bytes(data[18:20], "little") != 183:
        raise ValueError("bundled compiler must be a little-endian ELF64 ARM64 executable")
    if not os.access(binary, os.X_OK):
        raise ValueError("bundled ARM64 compiler is not executable")
    if not os.access(stage / "flash.sh", os.X_OK):
        raise ValueError("flash.sh is not executable")
    # The host intentionally has a different berry_conf.h, but shared code and
    # ABI declarations must match the device snapshot byte for byte.
    shared = {
        "vm.c": "shared/src/vm.c", "vm_source.c": "shared/src/vm_source.c",
        "berry_runtime_modtab.c": "shared/src/berry_runtime_modtab.c",
        "vm.h": "shared/include/freshwater/vm.h",
        "vm_channel.h": "shared/include/freshwater/vm_channel.h",
        "vm_source.h": "shared/include/freshwater/vm_source.h",
        "script_runtime.h": "shared/include/script/script_runtime.h",
    }
    for device, host in shared.items():
        if ((stage / "berry_runtime" / device).read_bytes() !=
                (stage / "berry_compiler" / host).read_bytes()):
            raise ValueError(f"compiler/runtime snapshot differs: {device}")
    device_vendor = stage / "berry_runtime/third_party/berry"
    host_vendor = stage / "berry_compiler/vendor/berry"
    for vendor, other in [(device_vendor, host_vendor), (host_vendor, device_vendor)]:
        for p in vendor.rglob("*"):
            if p.is_file():
                peer = other / p.relative_to(vendor)
                if not peer.is_file() or p.read_bytes() != peer.read_bytes():
                    raise ValueError(f"Berry vendor snapshot differs: {p.relative_to(vendor)}")
    # Check maintained guides and references. Upstream READMEs retain upstream
    # links even when their original repository contains additional files.
    docs = [stage / "README.md", stage / "berry_compiler/README.md"]
    docs += [stage / rel for rel in sorted(CARD_DOCS)]
    for p in docs:
        for link in markdown_links(p):
            link = unquote(link.split("#", 1)[0])
            if not link or re.match(r"[a-zA-Z]+:", link):
                continue
            target = (p.parent / link).resolve()
            if not target.is_relative_to(stage.resolve()) or not target.exists():
                raise ValueError(f"broken package link in {p.relative_to(stage)}: {link}")


def assemble(stage, working_tree=False):
    revision = run("git", "rev-parse", "HEAD").decode().strip()
    if working_tree:
        paths = run("git", "ls-files", "-z", "--cached", "--others",
                    "--exclude-standard", "--", "channel_card", "berry_compiler")
        for raw in sorted(set(paths.split(b"\0")) - {b""}):
            rel = os.fsdecode(raw)
            src = ROOT / rel
            if not selected(rel) or not src.exists():
                continue
            if src.is_symlink() or any(p.is_symlink() for p in src.parents if ROOT in p.parents):
                raise ValueError(f"symlink is not a release input: {rel}")
            if not src.is_file():
                raise ValueError(f"not a regular file: {rel}")
            dst = stage / destination(rel)
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
    else:
        status = run("git", "status", "--porcelain", "--untracked-files=all", "--", *INPUTS)
        if status:
            raise ValueError("release inputs are uncommitted; use --working-tree only for a local preview")
        archive = run("git", "archive", revision, "channel_card", "berry_compiler")
        with tarfile.open(fileobj=io.BytesIO(archive)) as source:
            for member in source:
                if member.isdir() or not selected(member.name):
                    continue
                if not member.isfile():
                    raise ValueError(f"not a regular release file: {member.name}")
                dst = stage / destination(member.name)
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_bytes(source.extractfile(member).read())
                dst.chmod(member.mode & 0o777)
    # Only this link changes between the monorepo and the nested SVN package.
    readme = stage / "README.md"
    readme.write_text(readme.read_text().replace(
        "(../berry_compiler/README.md)", "(berry_compiler/README.md)"))
    validate(stage)
    files = sorted(p for p in stage.rglob("*") if p.is_file())
    manifest = {
        "git_revision": revision,
        "snapshot": "working-tree preview; not a committed release" if working_tree else "committed",
        "files": [{"path": p.relative_to(stage).as_posix(), "bytes": p.stat().st_size,
                   "sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
                   "executable": bool(p.stat().st_mode & 0o111)} for p in files],
    }
    print(f"{manifest['snapshot']}: {revision}")
    print(f"Package: {len(files)} files, {sum(f['bytes'] for f in manifest['files']):,} bytes")
    return manifest


def compare_svn(stage, working_copy):
    wc = working_copy.resolve()
    info = ET.fromstring(run("svn", "info", "--xml", str(wc)))
    if info.find(".//wc-info") is None:
        raise ValueError("destination must be a local SVN working copy")
    trunk = wc / "trunk"
    if not trunk.is_dir():
        raise ValueError("SVN working copy must contain trunk/")
    status = ET.fromstring(run("svn", "status", "--xml", "--no-ignore", str(wc)))
    for entry in status.findall(".//entry"):
        state = entry.find("wc-status")
        if (state.get("item") not in {"normal", "none", "external"} or
                state.get("props") not in {"normal", "none"} or
                state.get("switched") == "true" or state.get("tree-conflicted") == "true" or
                state.get("item") == "external"):
            raise ValueError(f"SVN destination is not clean: {entry.get('path')}")
    # Exclude SVN administration only. Removed images, tests and scripts must
    # appear as deletions, not survive because of a matching exclusion rule.
    run_args = ["rsync", "-rclpni", "--delete", "--exclude=.svn/",
                str(stage) + "/", str(trunk) + "/"]
    print(run(*run_args).decode(), end="")
    print("Dry run only: no SVN files, properties, revisions or tags were changed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--stage", type=Path, help="new local package directory")
    target.add_argument("--svn-working-copy", type=Path, help="compare only; never modify SVN")
    parser.add_argument("--working-tree", action="store_true", help="local uncommitted preview")
    args = parser.parse_args()
    if args.working_tree and not args.stage:
        parser.error("--working-tree is only allowed with --stage")
    if args.stage and (args.stage.exists() or args.stage.with_name(args.stage.name + ".manifest.json").exists()):
        parser.error("stage directory and adjacent manifest must not already exist")
    try:
        with tempfile.TemporaryDirectory(prefix="channel-release-") as tmp:
            stage = Path(tmp) / "package"
            stage.mkdir()
            manifest = assemble(stage, args.working_tree)
            if args.stage:
                shutil.copytree(stage, args.stage)
                output = args.stage.with_name(args.stage.name + ".manifest.json")
                output.write_text(json.dumps(manifest, indent=2) + "\n")
                print(f"Package: {args.stage}\nManifest: {output}")
            else:
                compare_svn(stage, args.svn_working_copy)
    except (ValueError, OSError, subprocess.CalledProcessError, ET.ParseError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
