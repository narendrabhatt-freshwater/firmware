"""Exercise the flashing wrapper with a fake programmer; never access a board."""

import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]


class ChannelFlashTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="channel flash ")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "app").mkdir()
        (self.root / "bin").mkdir()
        self.script = self.root / "flash.sh"
        shutil.copy2(ROOT / "channel_card/flash.sh", self.script)
        shutil.copy2(ROOT / "channel_card/app/Makefile", self.root / "app/Makefile")
        self.binary = self.root / "bin/channel_MCU.bin"
        self.binary.write_bytes(b"mock firmware")
        self.log = self.root / "calls.jsonl"
        self.programmer = self.root / "mock programmer"
        self.programmer.write_text(f"#!{sys.executable}\n" + '''
import json, os, sys
with open(os.environ['CALL_LOG'], 'a') as f:
    f.write(json.dumps(sys.argv[1:]) + '\\n')
if sys.argv[1:] == ['-l', 'usb']:
    with open(os.environ['CALL_LOG']) as f:
        polls = sum(json.loads(line) == ['-l', 'usb'] for line in f)
    print(os.environ.get('USB_WAIT_LIST', 'No STM32 device in DFU mode connected')
          if polls <= int(os.environ.get('WAIT_POLLS', '0')) else os.environ['USB_LIST'])
    sys.exit(int(os.environ.get('LIST_EXIT', '0')))
sys.exit(int(os.environ.get('WRITE_EXIT', '0')))
''')
        self.programmer.chmod(0o755)
        self.env = dict(os.environ, CUBE_PROGRAMMER=str(self.programmer),
                        CALL_LOG=str(self.log), USB_LIST="Device Index : USB1\n")

    def run_script(self, *args):
        return subprocess.run([str(self.script), *args], cwd="/", env=self.env,
                              text=True, capture_output=True, timeout=8)

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def test_check_and_help_never_access_programmer(self):
        for option in ["--check", "--help"]:
            self.assertEqual(self.run_script(option).returncode, 0)
        self.assertEqual(self.calls(), [])

    def test_missing_programmer(self):
        self.env["CUBE_PROGRAMMER"] = str(self.root / "missing")
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Install STM32CubeProgrammer", result.stderr)
        self.assertEqual(self.calls(), [])

    def test_missing_or_empty_binary(self):
        self.binary.unlink()
        for empty in [False, True]:
            if empty:
                self.binary.touch()
            result = self.run_script()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Run make", result.stderr)
            self.assertEqual(self.calls(), [])

    def test_waits_for_bootloader(self):
        self.env["WAIT_POLLS"] = "2"
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Waiting for USB bootloader", result.stdout)
        self.assertEqual(self.calls()[:3], [["-l", "usb"]] * 3)
        self.assertEqual(len(self.calls()), 4)

    def test_list_failure_stops_without_writing(self):
        self.env["LIST_EXIT"] = "1"
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Could not list", result.stderr)
        self.assertEqual(self.calls(), [["-l", "usb"]])

    def test_unknown_output_stops_without_writing(self):
        self.env["USB_LIST"] = "Unexpected programmer output"
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Could not identify", result.stderr)
        self.assertEqual(self.calls(), [["-l", "usb"]])

    def test_cancel_wait_never_writes(self):
        self.env["WAIT_POLLS"] = "10000"
        proc = subprocess.Popen([str(self.script)], env=self.env, start_new_session=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 4
            while not self.log.exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue(self.log.exists())
            os.killpg(proc.pid, signal.SIGINT)
            out, err = proc.communicate(timeout=4)
            self.assertEqual(proc.returncode, 130, out + err)
            self.assertIn("Cancelled", err)
            self.assertTrue(all(c == ["-l", "usb"] for c in self.calls()))
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.communicate()

    def test_single_device_write_and_verify(self):
        self.env["USB_LIST"] = "\x1b[32mDevice Index : USB1\x1b[0m\nUSB Port : USB1"
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), [["-l", "usb"],
                         ["-c", "port=USB1", "-w", str(self.binary), "0x08000000", "-v"]])
        self.assertIn("Move BOOT down", result.stdout)

    def test_multiple_devices_require_selection(self):
        self.env["USB_LIST"] = "Device Index : USB1\nDevice Index : USB2"
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Multiple USB bootloaders", result.stderr)
        self.assertEqual(self.calls(), [["-l", "usb"]])
        result = self.run_script("--port", "USB2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls()[-1][1], "port=USB2")

    def test_invalid_arguments(self):
        for args in [("--port",), ("--port", "SWD"), ("--unknown",)]:
            self.assertNotEqual(self.run_script(*args).returncode, 0)
        self.assertEqual(self.calls(), [])

    def test_waits_for_selected_port(self):
        self.env.update(WAIT_POLLS="1", USB_WAIT_LIST="Device Index : USB1",
                        USB_LIST="Device Index : USB1\nDevice Index : USB2")
        result = self.run_script("--port", "USB2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls()[:2], [["-l", "usb"]] * 2)
        self.assertEqual(self.calls()[-1][1], "port=USB2")

    def test_write_failure_is_not_success(self):
        self.env["WRITE_EXIT"] = "1"
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Programming or verification failed", result.stderr)
        self.assertNotIn("verification complete", result.stdout)



if __name__ == "__main__":
    unittest.main()
