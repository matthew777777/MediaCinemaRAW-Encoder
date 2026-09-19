#!/usr/bin/env python3
"""Gyro container interop: encoder writer -> upstream motioncam-decoder.

Optionally also runs the sibling MediaCinemaRAW-Decoder round-trip when a
second path is given:

    python3 tools/verify_gyro.py /path/to/motioncam-decoder [/path/to/MediaCinemaRAW-Decoder]
"""
import pathlib
import subprocess
import sys
import tempfile

if len(sys.argv) not in (2, 3):
    raise SystemExit(
        "usage: verify_gyro.py /path/to/motioncam-decoder "
        "[/path/to/MediaCinemaRAW-Decoder]"
    )

root = pathlib.Path(__file__).resolve().parents[1]
decoder = pathlib.Path(sys.argv[1]).resolve()
sibling = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) == 3 else None

for p in (decoder / "lib/Decoder.cpp", decoder / "lib/RawData.cpp",
          decoder / "lib/RawData_Legacy.cpp"):
    if not p.exists():
        raise SystemExit(f"missing upstream source: {p}")
if sibling is not None:
    for p in (sibling / "src/Decoder.cpp", sibling / "src/ContainerReader.cpp",
              sibling / "tests/InteropTests.cpp"):
        if not p.exists():
            raise SystemExit(f"missing sibling source: {p}")

with tempfile.TemporaryDirectory(prefix="mediacinemaraw-gyro-") as tmp:
    build = pathlib.Path(tmp)
    # 1) Upstream oracle: encoder writer -> motioncam::Decoder gyro API.
    flags = [
        "clang++", "-std=c++17", "-O2", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-I" + str(root / "include"),
        "-I" + str(decoder / "lib/include"), "-I" + str(decoder / "thirdparty"),
    ]
    sources = [
        root / "src/Encoder.cpp",
        root / "src/ContainerWriter.cpp",
        root / "tests/ContainerGyroInterop.cpp",
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
    exe = build / "gyro-interop"
    subprocess.run(flags + objects + ["-o", str(exe)], check=True)
    subprocess.run([str(exe), str(build / "gyro_interop.mcraw")], check=True, cwd=str(build))

    # 2) Sibling oracle, when provided: full encode/decode + container round-trip.
    if sibling is not None:
        sflags = [
            "clang++", "-std=c++17", "-O2", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-I" + str(root / "include"),
            "-I" + str(sibling / "include"),
        ]
        ssources = [
            root / "src/Encoder.cpp",
            root / "src/ContainerWriter.cpp",
            sibling / "src/Decoder.cpp",
            sibling / "src/ContainerReader.cpp",
            sibling / "tests/InteropTests.cpp",
        ]
        sobjs = []
        for source in ssources:
            obj = build / ("sibling_" + source.stem + ".o")
            subprocess.run(sflags + ["-c", str(source), "-o", str(obj)], check=True)
            sobjs.append(str(obj))
        sexe = build / "sibling-interop"
        subprocess.run(sflags + sobjs + ["-o", str(sexe)], check=True)
        subprocess.run([str(sexe)], check=True, cwd=str(build))

print("gyro interop suites passed")
