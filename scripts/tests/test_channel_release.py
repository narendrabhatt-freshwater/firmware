"""Local release checks. No SVN process is invoked by these tests."""

import contextlib
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("channel_release", ROOT / "scripts/channel_release.py")
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


class ChannelReleaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="channel-release-test-")
        cls.base = Path(cls.tmp.name)
        cls.repo = cls.base / "source"
        cls.repo.mkdir()
        paths = subprocess.check_output([
            "git", "ls-files", "-z", "--cached", "--others", "--exclude-standard",
            "--", "channel_card", "berry_compiler"], cwd=ROOT)
        for raw in set(paths.split(b"\0")) - {b""}:
            rel = os.fsdecode(raw)
            src = ROOT / rel
            if release.selected(rel) and src.is_file():
                dst = cls.repo / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
        for rel in ["channel_card/.gitignore", "berry_compiler/.gitignore",
                    "scripts/channel_release.py", "scripts/svn_publish.sh"]:
            dst = cls.repo / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / rel, dst)
        for args in [("init", "-q"), ("config", "user.name", "Release test"),
                     ("config", "user.email", "release-test@example.invalid"),
                     ("add", "."), ("-c", "core.hooksPath=/dev/null", "commit", "-qm", "Test fixture")]:
            subprocess.run(["git", *args], cwd=cls.repo, check=True)
        cls.package = cls.base / "package"
        cls.package.mkdir()
        with patch.object(release, "ROOT", cls.repo), contextlib.redirect_stdout(io.StringIO()):
            # run() captures its default cwd at definition time; the Git root
            # must be supplied explicitly in the test adapter.
            original = release.run
            def fixture_run(*args, cwd=None):
                return original(*args, cwd=cls.repo if cwd is None else cwd)
            with patch.object(release, "run", side_effect=fixture_run):
                cls.manifest = release.assemble(cls.package)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def fixture_run(self, *args, cwd=None):
        return subprocess.check_output(args, cwd=self.repo if cwd is None else cwd)

    def test_export_is_committed_and_bundled(self):
        self.assertEqual(self.manifest["snapshot"], "committed")
        self.assertEqual(self.manifest["git_revision"],
                         self.fixture_run("git", "rev-parse", "HEAD").decode().strip())
        self.assertTrue((self.package / "app/usb.cpp").is_file())
        self.assertTrue((self.package / "flash.sh").stat().st_mode & 0o111)
        self.assertTrue((self.package / "berry_compiler/berry.linux-arm64").stat().st_mode & 0o111)
        for p in self.package.rglob("*"):
            if p.is_file():
                rel = p.relative_to(self.package).as_posix()
                original = rel if rel.startswith("berry_compiler/") else "channel_card/" + rel
                self.assertTrue(release.selected(original), rel)

    def test_excluded_categories(self):
        for path in ["channel_card/tests/test_usb.c", "channel_card/tests/fixtures/test.bec",
                     "channel_card/app/test_transport.cpp", "channel_card/app/usb_test.cpp",
                     "channel_card/build/channel_MCU.bin", "channel_card/.vscode/settings.json",
                     "channel_card/docs/board.png", "channel_card/scripts/flash_dfu.sh",
                     "berry_compiler/examples/envelope.bec", "berry_compiler/berry",
                     "channel_card/berry_runtime/third_party/berry/tests/check.c"]:
            self.assertFalse(release.selected(path), path)

    def test_dirty_source_rejected_but_preview_allowed(self):
        src = self.repo / "channel_card/app/usb.cpp"
        before = src.read_bytes()
        try:
            src.write_bytes(before + b"\n/* local edit */\n")
            with tempfile.TemporaryDirectory(dir=self.base) as tmp:
                with patch.object(release, "ROOT", self.repo), patch.object(release, "run", side_effect=self.fixture_run):
                    with self.assertRaisesRegex(ValueError, "uncommitted"):
                        release.assemble(Path(tmp))
                    with contextlib.redirect_stdout(io.StringIO()):
                        manifest = release.assemble(Path(tmp), working_tree=True)
                    self.assertIn("working-tree preview", manifest["snapshot"])
                    self.assertEqual((Path(tmp) / "app/usb.cpp").read_bytes(), src.read_bytes())
        finally:
            src.write_bytes(before)

    def test_ignored_outputs_never_enter_committed_package(self):
        generated = self.repo / "berry_compiler/examples/ignored.bec"
        generated.write_bytes(b"not release data")
        try:
            with tempfile.TemporaryDirectory(dir=self.base) as tmp:
                with patch.object(release, "run", side_effect=self.fixture_run), contextlib.redirect_stdout(io.StringIO()):
                    release.assemble(Path(tmp))
                self.assertFalse((Path(tmp) / "berry_compiler/examples/ignored.bec").exists())
        finally:
            generated.unlink()

    def test_missing_input_and_snapshot_mismatch_rejected(self):
        with tempfile.TemporaryDirectory(dir=self.base) as tmp:
            stage = Path(tmp) / "package"
            shutil.copytree(self.package, stage)
            header = stage / "berry_runtime/vm.h"
            header.write_text(header.read_text() + "\n/* divergent ABI copy */\n")
            with self.assertRaisesRegex(ValueError, "snapshot differs"):
                release.validate(stage)
            (stage / "berry_compiler/berry.linux-arm64").unlink()
            with self.assertRaisesRegex(ValueError, "missing package input"):
                release.validate(stage)

    def test_broken_links_rejected(self):
        with tempfile.TemporaryDirectory(dir=self.base) as tmp:
            stage = Path(tmp) / "package"
            shutil.copytree(self.package, stage)
            readme = stage / "README.md"
            readme.write_text(readme.read_text() + "\n[bad](missing.md)\n")
            with self.assertRaisesRegex(ValueError, "broken package link"):
                release.validate(stage)

    def test_read_only_comparison_reports_obsolete_files(self):
        with tempfile.TemporaryDirectory(dir=self.base) as tmp:
            wc = Path(tmp)
            trunk = wc / "trunk"
            shutil.copytree(self.package, trunk)
            for rel in ["tests/old_test.c", "docs/old.png", "scripts/old.sh"]:
                p = trunk / rel
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text("obsolete")
            def digest():
                return {p.relative_to(wc).as_posix(): (hashlib.sha256(p.read_bytes()).hexdigest(),
                                                     p.stat().st_mode, p.stat().st_mtime_ns)
                        for p in wc.rglob("*") if p.is_file()}
            before = digest()
            calls = []
            def readonly_run(*args, **kwargs):
                calls.append(args)
                if args[:2] == ("svn", "info"):
                    return b"<info><entry><wc-info/></entry></info>"
                if args[:2] == ("svn", "status"):
                    return b"<status><target/></status>"
                self.assertEqual(args[0], "rsync")
                self.assertIn("n", args[1])
                return subprocess.check_output(args)
            output = io.StringIO()
            with patch.object(release, "run", side_effect=readonly_run), contextlib.redirect_stdout(output):
                release.compare_svn(self.package, wc)
            self.assertEqual(before, digest())
            for rel in ["tests/old_test.c", "docs/old.png", "scripts/old.sh"]:
                self.assertIn(rel, output.getvalue())
            self.assertEqual([c[1] for c in calls if c[0] == "svn"], ["info", "status"])

    def test_dirty_destination_rejected_before_rsync(self):
        with tempfile.TemporaryDirectory(dir=self.base) as tmp:
            wc = Path(tmp)
            (wc / "trunk").mkdir()
            for state in ["modified", "unversioned", "ignored", "conflicted", "external"]:
                responses = [b"<info><entry><wc-info/></entry></info>",
                             f'<status><target><entry path="trunk/file"><wc-status item="{state}" props="none"/></entry></target></status>'.encode()]
                with patch.object(release, "run", side_effect=responses) as calls:
                    with self.assertRaisesRegex(ValueError, "not clean"):
                        release.compare_svn(self.package, wc)
                    self.assertEqual(calls.call_count, 2)

    def test_channel_wrapper_cannot_publish(self):
        for args in [("channel_card", "unused"), ("channel_card", "unused", "--tag", "v1"),
                     ("berry_compiler", "unused", "--dry-run")]:
            result = subprocess.run(["bash", str(ROOT / "scripts/svn_publish.sh"), *args],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("error:", result.stderr)


if __name__ == "__main__":
    unittest.main()
