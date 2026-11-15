"""Minimal RIFF fixture I/O for the standalone renderer tests."""

import struct


RATE = 48000
LATENCY = 1536
FLOAT_GUID = bytes.fromhex("0300000000001000800000aa00389b71")
PCM_GUID = bytes.fromhex("0100000000001000800000aa00389b71")


def chunk(name, data):
    return name + struct.pack("<I", len(data)) + data + bytes(len(data) % 2)


def write_wave(path, data, *, channels=4, rate=RATE, bits=32, tag=3,
               subformat=FLOAT_GUID, valid_bits=None):
    block_align = channels * bits // 8
    fmt = struct.pack("<HHIIHH", tag, channels, rate, rate * block_align, block_align, bits)
    if tag == 0xFFFE:
        fmt += struct.pack("<HHI", 22, bits if valid_bits is None else valid_bits, 0) + subformat
    riff = b"WAVE" + chunk(b"fmt ", fmt) + chunk(b"data", data)
    path.write_bytes(chunk(b"RIFF", riff))


def read_stereo(path):
    data = path.read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    offset = 12
    chunks = {}
    while offset + 8 <= len(data):
        kind, size = struct.unpack_from("<4sI", data, offset)
        offset += 8
        chunks[kind] = data[offset:offset + size]
        offset += size + size % 2
    assert struct.unpack_from("<HHI", chunks[b"fmt "]) == (3, 2, RATE)
    assert struct.unpack_from("<H", chunks[b"fmt "], 14)[0] == 32
    return list(struct.iter_unpack("<ff", chunks[b"data"]))
