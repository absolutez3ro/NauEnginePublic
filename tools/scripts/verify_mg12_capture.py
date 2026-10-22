"""Check the actual GPU 0 texture readback against the MG-12 shader at the captured phase.

No image or numerical packages are needed. Usage: python verify_mg12_capture.py file.ppm [...]
The normal sample path does not read back; enable captureAtFrame only for this acceptance check.
"""

import hashlib
import json
from pathlib import Path
import re
import sys


def verify(path):
    with path.open("rb") as stream:
        assert stream.readline() == b"P6\n", "Expected MG-12 PPM capture"
        metadata = stream.readline().decode("ascii")
        match = re.fullmatch(r"# MG-12 frame=(\d+) phase=(\d+)\n", metadata)
        assert match, "Missing frame/phase metadata"
        frame, phase = map(int, match.groups())
        width, height = map(int, stream.readline().split())
        assert stream.readline() == b"255\n"
        pixels = stream.read()
    assert width > 0 and height > 0 and len(pixels) == width * height * 3
    marker = phase * (width - 1) // 255
    mismatches = 0
    first_mismatch = None
    for y in range(height):
        for x in range(width):
            odd = (((x + phase * 3) // 48) ^ (y // 48)) & 1
            expected = (phase, 180, 255 - phase) if odd else (255 - phase, 40, phase)
            if abs(x - marker) < 8:
                expected = (255, 255, 255)
            offset = (y * width + x) * 3
            actual = tuple(pixels[offset:offset + 3])
            if actual != expected:
                mismatches += 1
                if first_mismatch is None:
                    first_mismatch = dict(x=x, y=y, actual=actual, expected=expected)
    result = dict(path=str(path), frame=frame, phase=phase, width=width, height=height,
                  checked_pixels=width * height, mismatches=mismatches, first_mismatch=first_mismatch,
                  sha256=hashlib.sha256(pixels).hexdigest())
    print(json.dumps(result))
    return mismatches == 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("Usage: verify_mg12_capture.py file.ppm [...]")
    results = [verify(Path(name)) for name in sys.argv[1:]]
    raise SystemExit(0 if all(results) else 1)
