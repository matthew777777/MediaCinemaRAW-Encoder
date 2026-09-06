#!/usr/bin/env python3
"""Decode 240 generated payloads with a separate motioncam-decoder checkout."""
import pathlib
import subprocess
import sys
import tempfile

if len(sys.argv) != 2:
    raise SystemExit("usage: verify_compatibility.py /path/to/motioncam-decoder")

root = pathlib.Path(__file__).resolve().parents[1]
decoder = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="mediacinemaraw-") as tmp:
    build = pathlib.Path(tmp)
    flags = [
        "clang++", "-std=c++17", "-O2", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-I" + str(root / "include"),
        "-I" + str(decoder / "lib/include"), "-I" + str(decoder / "thirdparty"),
    ]
    sources = [
        root / "src/Encoder.cpp",
        root / "tests/CompatibilityTests.cpp",
        decoder / "lib/Decoder.cpp",
        decoder / "lib/RawData.cpp",
        decoder / "lib/RawData_Legacy.cpp",
    ]
    objects = []
    for source in sources:
        obj = build / (source.stem + ".o")
        extra = ["-fno-sanitize=alignment"] if decoder in source.parents else []
        subprocess.run(flags + extra + ["-c", str(source), "-o", str(obj)], check=True)
        objects.append(str(obj))
    executable = build / "compatibility-tests"
    subprocess.run(flags + objects + ["-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
