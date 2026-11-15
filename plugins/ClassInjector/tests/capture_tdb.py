"""Read-only TDB81 snapshot for ClassInjectorTests (Windows, CITDB format v1)."""
import argparse
import ctypes as C
import struct
from ctypes import wintypes as W

parser = argparse.ArgumentParser(__doc__)
parser.add_argument("--pid", type=int, required=True)
parser.add_argument("--tdb", type=lambda value: int(value, 0), required=True)
parser.add_argument("--output", required=True)
args = parser.parse_args()
kernel = C.WinDLL("kernel32", use_last_error=True)
kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
kernel.OpenProcess.restype = W.HANDLE
kernel.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
kernel.ReadProcessMemory.restype = W.BOOL
kernel.CloseHandle.argtypes = [W.HANDLE]
process = kernel.OpenProcess(0x410, False, args.pid)
if not process:
    raise C.WinError(C.get_last_error())

def read(address, size):
    buffer = C.create_string_buffer(size)
    count = C.c_size_t()
    if not kernel.ReadProcessMemory(process, address, buffer, size, C.byref(count)):
        raise C.WinError(C.get_last_error())
    if count.value != size:
        raise OSError(f"incomplete TDB read: {count.value}/{size} bytes")
    return buffer.raw

try:
    header = read(args.tdb, 0xF0)
    if header[:8] != b"TDB\0Q\0\0\0":
        raise ValueError("expected a relocated TDB81 header")
    with open(args.output, "wb") as output:
        output.write(b"CITDB\x00\x01\x00")
        output.write(header)
        for pointer_offset, count_offset, stride in [
            (0x68, 8, 80), (0x70, 0x1C, 48), (0x78, 0x14, 12),
            (0x80, 0x24, 12), (0x88, 0x18, 8), (0x90, 0x20, 12),
            (0xB0, 0x34, 12), (0xD8, 0x54, 1), (0xE0, 0x58, 1),
            (0xC0, 0x3C, 4),  # initializer offsets into bytePool/stringPool
        ]:
            address = struct.unpack_from("<Q", header, pointer_offset)[0]
            size = struct.unpack_from("<I", header, count_offset)[0] * stride
            if size:
                output.write(read(address, size))
finally:
    kernel.CloseHandle(process)
