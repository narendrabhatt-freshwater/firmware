"""Build Channel firmware in a temporary tree; never flash a device."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ("make", "python3", "arm-none-eabi-gcc", "arm-none-eabi-g++",
         "arm-none-eabi-ar", "arm-none-eabi-objcopy", "arm-none-eabi-objdump")


@unittest.skipUnless(all(shutil.which(tool) for tool in TOOLS),
                     "GNU Make, Python 3 and GNU Arm toolchain required")
class ChannelMakeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="channel-make-test-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.card = Path(cls.tmp.name) / "channel_card"
        shutil.copytree(ROOT / "channel_card", cls.card,
                        ignore=shutil.ignore_patterns("build", "bin", ".build",
                                                      ".git", "__pycache__"))
        cls.binary = cls.card / "bin/channel_MCU.bin"
        cls.build = cls.card / "build/release"
        cls.report = cls.build / "channel_MCU.info"
        result = cls.make()
        if result.returncode:
            raise RuntimeError(result.stdout)
        cls.baseline = cls.binary.read_bytes()

    @classmethod
    def make(cls, *args):
        return subprocess.run(["make", "-C", str(cls.card / "app"), "-j4", *args],
                              text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=120)

    def successful_make(self, *args):
        result = self.make(*args)
        self.assertEqual(result.returncode, 0, result.stdout)
        return result

    def artifact_times(self):
        return {p.relative_to(self.card): p.stat().st_mtime_ns
                for p in self.build.rglob("*") if p.is_file()} | {
                    Path("bin/channel_MCU.bin"): self.binary.stat().st_mtime_ns}

    def test_noop_and_info_do_not_rebuild_or_change_baud(self):
        before = self.artifact_times()
        self.successful_make()
        result = self.successful_make("info", "BAUDRATE=3000000")
        self.assertIn("921600 baud", result.stdout)
        self.assertEqual(before, self.artifact_times())

    def test_baud_override_and_return_to_default(self):
        try:
            self.successful_make("BAUDRATE=3000000")
            self.assertIn("3000000 baud", self.report.read_text())
            self.assertNotEqual(self.baseline, self.binary.read_bytes())
        finally:
            self.successful_make()
        self.assertIn("921600 baud", self.report.read_text())
        self.assertEqual(self.baseline, self.binary.read_bytes())

    def test_invalid_baud_preserves_last_successful_build(self):
        before = self.artifact_times()
        for baud in ("", "0", "-1", "abc", "4000001", "12.5"):
            with self.subTest(baud=baud):
                result = self.make("BAUDRATE=" + baud)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("BAUDRATE must be a positive integer", result.stdout)
                self.assertEqual(before, self.artifact_times())

    def test_header_dependencies_recompile_affected_source(self):
        obj = self.build / "app/channel.cpp.o"
        # macOS system Make 3.81 compares timestamps at whole-second precision.
        # Backdate this temporary object so the header is unambiguously newer.
        older = obj.stat().st_mtime_ns - 2_000_000_000
        os.utime(obj, ns=(older, older))
        before = obj.stat().st_mtime_ns
        os.utime(self.card / "app/channel.h", None)
        self.successful_make()
        self.assertGreater(obj.stat().st_mtime_ns, before)
        self.assertEqual(self.baseline, self.binary.read_bytes())

    def test_failed_compile_preserves_delivered_binary_and_report(self):
        source = self.card / "app/channel.cpp"
        original = source.read_bytes()
        report = self.report.read_bytes()
        try:
            source.write_bytes(original + b"\n#error deliberate_build_test_failure\n")
            result = self.make()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("deliberate_build_test_failure", result.stdout)
            self.assertEqual(self.baseline, self.binary.read_bytes())
            self.assertEqual(report, self.report.read_bytes())
        finally:
            source.write_bytes(original)
            self.successful_make()

    def test_clean_and_rebuild(self):
        self.successful_make("clean")
        self.assertFalse(self.build.exists())
        self.assertFalse(self.binary.exists())
        self.assertNotEqual(self.make("info").returncode, 0)
        self.successful_make()
        self.assertEqual(self.baseline, self.binary.read_bytes())


if __name__ == "__main__":
    unittest.main()
