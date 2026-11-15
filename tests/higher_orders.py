"""Generate ACN/SN3D fixtures and compare SAF rotation to direct HOA encoding."""

import math
from pathlib import Path
import struct
import subprocess
import sys
import time

from rotation import (BACK, BURST_STARTS, FRAMES, RIGHT, UP, camera_basis, dot,
                      null_error, render, write_fixed_fixture)
from wav_fixture import write_wave


def sn3d(direction, order):
    right, up, back = direction
    azimuth = math.atan2(-right, -back)
    radial = math.hypot(right, back)
    coefficients = [0.0] * ((order + 1) ** 2)
    diagonal = 1.0
    for m in range(order + 1):
        # Associated Legendre polynomials with no Condon–Shortley phase.
        if m:
            diagonal *= (2 * m - 1) * radial
        previous, current = 0.0, diagonal
        for degree in range(m, order + 1):
            scale = math.sqrt((2 if m else 1) * math.factorial(degree - m) /
                              math.factorial(degree + m))
            center = degree * (degree + 1)
            coefficients[center + m] = scale * current * math.cos(m * azimuth)
            if m:
                coefficients[center - m] = scale * current * math.sin(m * azimuth)
            previous, current = current, ((2 * degree + 1) * up * current -
                                          (degree + m) * previous) / (degree - m + 1)
    return coefficients


def write_sources(path, directions, noise, order, *, highest_only=False):
    channels = (order + 1) ** 2
    frame = struct.Struct("<" + channels * "f")
    data = bytearray(FRAMES * frame.size)
    for start, direction in zip(BURST_STARTS, directions):
        gains = sn3d(direction, order)
        if highest_only:
            gains[:order * order] = [0.0] * (order * order)
        for index, sample in enumerate(noise, start):
            frame.pack_into(data, index * frame.size, *(gain * sample for gain in gains))
    write_wave(path, data, channels=channels, tag=0xFFFE)


def generate_fixtures(directory):
    directory.mkdir(parents=True, exist_ok=True)
    directions, noise = write_fixed_fixture(directory / "fixed-sources.wav")
    for order in range(2, 6):
        write_sources(directory / f"fixed-sources-order-{order}.wav", directions, noise, order)
    return directions, noise


def verify(executable, directory):
    directions, noise = generate_fixtures(directory)
    basis = camera_basis(((BACK, 24), (RIGHT, -41), (UP, 67)))
    rotated = [tuple(dot(vector, column) for column in basis) for vector in directions]
    for order in range(2, 6):
        raw = directory / f"fixed-sources-order-{order}.wav"
        reference = directory / f"order-{order}-projected.wav"
        write_sources(reference, rotated, noise, order)
        start = time.perf_counter()
        actual = render(executable, raw, directory / f"order-{order}-rotated-stereo.wav", basis)
        elapsed = time.perf_counter() - start
        expected = render(executable, reference, directory / f"order-{order}-reference-stereo.wav")
        peak, rms = null_error(actual, expected)
        assert peak < 3e-6 and rms < 1e-5, (order, peak, rms)
        assert all(math.isfinite(sample) and abs(sample) < 1 for frame in actual for sample in frame)

        highest = directory / f"order-{order}-highest-band.wav"
        write_sources(highest, directions, noise, order, highest_only=True)
        isolated = render(executable, highest, directory / f"order-{order}-highest-band-stereo.wav")
        assert max(abs(sample) for frame in isolated for sample in frame) > 0.001, order
        print(f"Order {order} ({(order + 1) ** 2} ch): peak error {peak:.3g}, "
              f"relative RMS {rms:.3g}, offline render + read {elapsed:.3f} s")

    for channels in (8, 49):
        bad = directory / f"unsupported-{channels}-channels.wav"
        write_wave(bad, bytes(channels * 4), channels=channels)
        output = directory / "unsupported-stereo.wav"
        result = subprocess.run([executable, bad, output], capture_output=True, text=True)
        assert result.returncode == 1 and "orders 1-5" in result.stderr, result.stderr
        assert not output.exists()


if __name__ == "__main__":
    if sys.argv[1] == "--fixtures-only":
        generate_fixtures(Path(sys.argv[2]).resolve())
    else:
        verify(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
