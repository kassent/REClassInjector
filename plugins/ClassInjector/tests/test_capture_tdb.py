"""Exercise the capture CLI against local process memory, without a game."""
import ctypes
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(sys.platform == "win32", "capture uses ReadProcessMemory")
class CaptureTdbTests(unittest.TestCase):
    def test_initializer_table_and_empty_tables(self):
        initializers = (ctypes.c_int32 * 3)(0, 4, -8)
        header = bytearray(0xF0)
        header[:8] = b"TDB\0Q\0\0\0"
        struct.pack_into("<i", header, 0x3C, len(initializers))
        struct.pack_into("<Q", header, 0xC0, ctypes.addressof(initializers))
        memory = ctypes.create_string_buffer(bytes(header))
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "snapshot.bin"
            subprocess.run(
                [sys.executable, str(Path(__file__).with_name("capture_tdb.py")),
                 "--pid", str(os.getpid()), "--tdb", hex(ctypes.addressof(memory)),
                 "--output", str(output)], check=True, capture_output=True, text=True)
            self.assertEqual(output.read_bytes(), b"CITDB\x00\x01\x00" + header + bytes(initializers))


if __name__ == "__main__":
    unittest.main()
