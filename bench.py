#!/usr/bin/env python3

import subprocess
import re

PROGRAM = ".pio/build/native/program"
DIR = "test/wav/20m_busy"

pattern = re.compile(r"^\d+\.\d+\s+\d+\s+\d{6}\s")

print(f"{'File':>4} {'Decoded':>8}")
print("-" * 16)

total = 0

for i in range(1, 39):

    wav = f"{DIR}/test_{i:02d}.wav"

    result = subprocess.run(
        [PROGRAM, "-file", wav],
        capture_output=True,
        text=True
    )

    count = sum(
        bool(pattern.match(line))
        for line in result.stdout.splitlines()
    )

    total += count
    print(f"{i:4d} {count:8d}")

print("-" * 16)
print(f"Total: {total}")
print(f"Average: {total/38:.1f}")