"""Compare AmbiBIN rotation with directional FOA encoded in camera coordinates."""

import math
from pathlib import Path
import random
import struct
import subprocess
import sys

from wav_fixture import LATENCY, RATE, read_stereo, write_wave


RIGHT = (1, 0, 0)
UP = (0, 1, 0)
BACK = (0, 0, 1)
FRONT = (0, 0, -1)
BURST_STARTS = (RATE // 8, RATE // 2, 7 * RATE // 8, 5 * RATE // 4)
BURST_LENGTH = RATE // 8
FRAMES = 3 * RATE // 2 + 13


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def rotate(v, axis, degrees):
    """Actively rotate a Godot-space vector using Rodrigues' formula."""
    x, y, z = axis
    a, b, c = v
    cross = (y * c - z * b, z * a - x * c, x * b - y * a)
    angle = math.radians(degrees)
    cos, sin = math.cos(angle), math.sin(angle)
    along_axis = dot(axis, v)
    return tuple(cos * component + sin * tangent + (1 - cos) * along_axis * unit
                 for component, tangent, unit in zip(v, cross, axis))


def camera_basis(turns):
    columns = (RIGHT, UP, BACK)
    for axis, degrees in turns:
        columns = tuple(rotate(vector, axis, degrees) for vector in columns)
    return columns


def write_sources(path, directions, noise):
    data = bytearray(FRAMES * 16)
    for start, (right, up, back) in zip(BURST_STARTS, directions):
        # ACN/SN3D plane-wave coefficients W, Y(left), Z(up), X(front).
        for frame, s in enumerate(noise, start):
            struct.pack_into("<ffff", data, frame * 16, s, -right * s, up * s, -back * s)
    write_wave(path, data)


def render(executable, source, output, basis=None):
    args = [executable, source, output]
    if basis is not None:
        args += ["--listener-basis", *(format(value, ".17g") for axis in basis for value in axis)]
    result = subprocess.run(args, capture_output=True, text=True, check=True)
    assert not result.stderr, result.stderr
    return read_stereo(output)


def null_error(actual, expected):
    assert len(actual) == len(expected) == FRAMES + LATENCY
    error_energy, reference_energy, peak_error = 0.0, 0.0, 0.0
    for frame, reference in zip(actual, expected):
        for sample, target in zip(frame, reference):
            error = abs(sample - target)
            peak_error = max(peak_error, error)
            error_energy += error * error
            reference_energy += target * target
    return peak_error, math.sqrt(error_energy / reference_energy)


def write_fixed_fixture(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    rng = random.Random(805)
    noise = [rng.uniform(-0.12, 0.12) * math.sin(math.pi * i / BURST_LENGTH) ** 2
             for i in range(BURST_LENGTH)]
    oblique = (2 / 7, 3 / 7, -6 / 7)
    directions = (FRONT, (-1, 0, 0), UP, oblique)
    write_sources(path, directions, noise)
    return directions, noise


def main(executable, directory):
    source = directory / "foa-fixed-sources.wav"
    directions, noise = write_fixed_fixture(source)

    cases = {
        "neutral": (),
        "yaw-left": ((UP, 90),),
        "yaw-right": ((UP, -90),),
        "pitch-up": ((RIGHT, 90),),
        "pitch-down": ((RIGHT, -90),),
        "roll-left": ((BACK, 90),),
        "roll-right": ((BACK, -90),),
        "combined": ((BACK, 24), (RIGHT, -41), (UP, 67)),
        "combined-at-zenith": ((BACK, -37), (RIGHT, 90), (UP, 29)),
        "combined-at-nadir": ((BACK, -37), (RIGHT, -90), (UP, 29)),
        "near-zenith": ((BACK, -37), (RIGHT, 89.99), (UP, 29)),
    }
    max_peak_error = 0.0
    max_rms_ratio = 0.0
    lateral_cases = {"yaw-left": (0, 1), "yaw-right": (0, 0),
                     "roll-left": (2, 1), "roll-right": (2, 0)}
    for name, turns in cases.items():
        basis = camera_basis(turns)
        # The camera-space direction is its projection onto each camera axis.
        # Re-encode those directions directly. The reference never calls SAF rotation.
        reference = directory / f"foa-reference-{name}.wav"
        write_sources(reference, [tuple(dot(vector, column) for column in basis)
                                  for vector in directions], noise)
        expected = render(executable, reference, directory / f"reference-{name}.wav")
        actual = render(executable, source, directory / f"listener-{name}.wav", basis)
        peak_error, rms_ratio = null_error(actual, expected)
        assert peak_error < 2e-6 and rms_ratio < 1e-5, (name, peak_error, rms_ratio)
        max_peak_error = max(max_peak_error, peak_error)
        max_rms_ratio = max(max_rms_ratio, rms_ratio)
        print(f"{name}: peak sample error {peak_error:.3g}, relative RMS {rms_ratio:.3g}")

        if name in lateral_cases:
            burst, near = lateral_cases[name]
            start = BURST_STARTS[burst]
            window = actual[start:start + BURST_LENGTH + 2 * LATENCY]
            near_energy = sum(frame[near] ** 2 for frame in window)
            far_energy = sum(frame[1 - near] ** 2 for frame in window)
            assert near_energy > 2 * far_energy > 0, (name, near_energy, far_energy)

    print(f"Maximum errors: peak {max_peak_error:.3g}, relative RMS {max_rms_ratio:.3g}")

    for basis_args, message in (
        (["2", "0", "0", "0", "1", "0", "0", "0", "1"], "orthonormal"),
        (["1", "0", "0", "0", "1", "0", "0", "0", "-1"], "right-handed"),
        (["nan", "0", "0", "0", "1", "0", "0", "0", "1"], "orthonormal"),
    ):
        output = directory / "invalid-basis.wav"
        output.unlink(missing_ok=True)
        result = subprocess.run([executable, source, output, "--listener-basis", *basis_args],
                                capture_output=True, text=True)
        assert result.returncode == 1 and message in result.stderr, result.stderr
        assert not output.exists()


if __name__ == "__main__":
    if sys.argv[1] == "--fixture-only":
        write_fixed_fixture(Path(sys.argv[2]).resolve())
    else:
        main(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
