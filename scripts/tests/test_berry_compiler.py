"""Exercise the standalone compiler and its FWSC output.

Set BERRY_BASELINE to a pre-change compiler to compare acceptance and complete
container bytes. Both compilers use the same paths and input files.
"""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
HANDLERS = b"""def on_note_on(key, velocity)
    start_note()
end
def on_note_off()
    note_end()
end
def on_ramp_end()
end
"""


class CompilerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix="berry-compiler-test-")
        cls.directory = Path(cls.work.name)
        cls.compiler = cls.directory / "berry"
        result = subprocess.run([
            "make", "-C", str(ROOT / "berry_compiler"), "-j4",
            "BUILD_DIR=" + str(cls.directory / "build"), "OUTPUT=" + str(cls.compiler),
        ], capture_output=True, text=True)
        if result.returncode:
            cls.work.cleanup()
            raise RuntimeError(result.stdout + result.stderr)
        cls.baseline = os.environ.get("BERRY_BASELINE")

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def compile(self, executable, source, output):
        output.unlink(missing_ok=True)
        result = subprocess.run([str(executable), str(source), "-o", str(output)],
                                capture_output=True)
        data = output.read_bytes() if output.exists() else None
        self.assertFalse(list(output.parent.glob("*.tmp")))
        return result, data

    def check_source(self, text, valid):
        source = self.directory / "input.be"
        output = self.directory / "nested" / "output.bec"
        source.write_bytes(text)
        result, data = self.compile(self.compiler, source, output)
        self.assertEqual(result.returncode == 0, valid, result.stderr.decode(errors="replace"))
        if valid:
            self.assertEqual(data[:4], b"FWSC")
            version, runtime, config, abi, header, length, crc = struct.unpack("<HBBHHII", data[4:20])
            self.assertEqual((version, runtime, config, abi, header), (1, 1, 3, 2, 20))
            self.assertEqual(length, len(data) - 20)
            self.assertLessEqual(length, 16384)
            self.assertEqual(crc, zlib.crc32(data[20:]))
            repeated, repeated_data = self.compile(self.compiler, source, output)
            self.assertEqual(repeated.returncode, 0)
            self.assertEqual(data, repeated_data)
        if self.baseline:
            old, old_data = self.compile(self.baseline, source, output)
            self.assertEqual(result.returncode, old.returncode)
            self.assertEqual(data, old_data)

    def test_valid_programs(self):
        cases = [HANDLERS, HANDLERS.replace(b"\n", b"\r\n"), HANDLERS.rstrip(b"\n"),
                 b"# " + b"comment " * 110 + b"\n" + HANDLERS,
                 b"\n" * 1500 + HANDLERS,
                 (ROOT / "berry_compiler/examples/channel_envelope.be").read_bytes(),
                 (ROOT / "scripts/tests/channel_card/oscillator.be").read_bytes(),
                 (ROOT / "scripts/tests/channel_card/fault.be").read_bytes()]
        for index, source in enumerate(cases):
            with self.subTest(case=index):
                self.check_source(source, True)

    def test_invalid_programs(self):
        cases = [b"", b"import math\n" + HANDLERS, b"var surprise = 1\n" + HANDLERS,
                 HANDLERS.replace(b"key, velocity", b"key"),
                 HANDLERS.replace(b"start_note()", b"start_note("),
                 HANDLERS.replace(b"def on_note_off()\n    note_end()\nend\n", b"")]
        for index, source in enumerate(cases):
            with self.subTest(case=index):
                self.check_source(source, False)


if __name__ == "__main__":
    unittest.main()
