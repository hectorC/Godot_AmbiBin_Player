"""Generate known-direction ambiX fixtures and check the standalone renderer."""

from pathlib import Path
import math
import random
import struct
import subprocess
import sys

from wav_fixture import LATENCY, PCM_GUID, RATE, read_stereo, write_wave

FRAMES = 2 * RATE + 37


def render(source, destination):
    result = subprocess.run([renderer, source, destination], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    print(result.stdout.strip())
    return read_stereo(destination)


def reject(source, message):
    destination = directory / "invalid-stereo.wav"
    destination.unlink(missing_ok=True)
    result = subprocess.run([renderer, source, destination], capture_output=True, text=True)
    assert result.returncode == 1 and message in result.stderr, (result.returncode, result.stderr)
    assert not destination.exists()


renderer = Path(sys.argv[1]).resolve()
directory = Path(sys.argv[2]).resolve()
directory.mkdir(parents=True, exist_ok=True)

# Two identical broadband bursts at +/-90 degrees, encoded as W, +/-Y, 0, 0.
random_source = random.Random(371)
burst = [random_source.uniform(-0.15, 0.15) * math.sin(math.pi * i / 12000) ** 2
         for i in range(12000)]
left_at, right_at = RATE // 4, 5 * RATE // 4
data = bytearray(FRAMES * 16)
for start, side in ((left_at, 1), (right_at, -1)):
    for i, sample in enumerate(burst, start):
        struct.pack_into("<ffff", data, i * 16, sample, sample * side, 0, 0)
# Place an impulse in the final partial block to exercise end padding.
struct.pack_into("<ffff", data, (FRAMES - 9) * 16, 0.15, 0, 0, 0)

source = directory / "foa-left-right.wav"
write_wave(source, data)
stereo = render(source, directory / "stereo-left-right.wav")
assert len(stereo) == FRAMES + LATENCY
peak = max(abs(sample) for frame in stereo for sample in frame)
assert all(math.isfinite(sample) for frame in stereo for sample in frame)
assert 0.01 < peak < 1, peak

for start, near in ((left_at, 0), (right_at, 1)):
    window = stereo[start:start + len(burst) + 2 * LATENCY]
    near_energy = sum(frame[near] ** 2 for frame in window)
    far_energy = sum(frame[1 - near] ** 2 for frame in window)
    assert near_energy > 2 * far_energy > 0, (near_energy, far_energy)
    print(f"{'Left' if near == 0 else 'Right'} burst near-ear advantage: "
          f"{10 * math.log10(near_energy / far_energy):.2f} dB")

assert max(abs(sample) for frame in stereo[FRAMES:] for sample in frame) > 0.0001

extensible = directory / "foa-extensible.wav"
write_wave(extensible, data, tag=0xFFFE)
assert stereo == render(extensible, directory / "stereo-extensible.wav")

for bits in (16, 24):
    scale = 1 << (bits - 1)
    quantized = [round(value * scale) for (value,) in struct.iter_unpack("<f", data)]
    reference = directory / f"pcm{bits}-reference.wav"
    write_wave(reference, struct.pack(f"<{len(quantized)}f", *(value / scale for value in quantized)))
    expected = render(reference, directory / f"pcm{bits}-reference-stereo.wav")

    packed = b"".join(value.to_bytes(bits // 8, "little", signed=True) for value in quantized)
    pcm = directory / f"pcm{bits}.wav"
    write_wave(pcm, packed, bits=bits, tag=1)
    assert render(pcm, directory / f"pcm{bits}-stereo.wav") == expected

    extended = directory / f"pcm{bits}-extensible.wav"
    write_wave(extended, packed, bits=bits, tag=0xFFFE, subformat=PCM_GUID)
    assert render(extended, directory / f"pcm{bits}-extensible-stereo.wav") == expected

    if bits == 24:
        padded = directory / "pcm24-in-32.wav"
        write_wave(padded, struct.pack(f"<{len(quantized)}i", *(value << 8 for value in quantized)),
                   bits=32, tag=0xFFFE, subformat=PCM_GUID, valid_bits=24)
        assert render(padded, directory / "pcm24-in-32-stereo.wav") == expected

wrong_rate = directory / "wrong-rate.wav"
write_wave(wrong_rate, bytes(16), rate=44100)
reject(wrong_rate, "48 kHz")
wrong_channels = directory / "wrong-channels.wav"
write_wave(wrong_channels, bytes(8), channels=2)
reject(wrong_channels, "4, 9, 16, 25, or 36 ambiX channels")
wrong_encoding = directory / "wrong-encoding.wav"
write_wave(wrong_encoding, bytes(4), bits=8, tag=1)
reject(wrong_encoding, "Expected 16-bit PCM, 24-bit PCM, or 32-bit IEEE float")
truncated = directory / "truncated.wav"
truncated.write_bytes(source.read_bytes()[:-3])
reject(truncated, "truncated data")
non_finite = directory / "non-finite.wav"
write_wave(non_finite, struct.pack("<ffff", float("nan"), 0, 0, 0))
reject(non_finite, "non-finite sample")
