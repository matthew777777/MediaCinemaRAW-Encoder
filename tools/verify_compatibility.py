#!/usr/bin/env python3
"""Encode 240 generated payloads here, decode them with motioncam-decoder.

Builds tests/CompatibilityTests.cpp together with this repo's encoder
sources and the upstream decoder sources from a separate checkout, under
AddressSanitizer/UBSan, and runs the round-trip suite. Upstream sources are
compiled in place, never vendored.

Usage:
    python3 tools/verify_compatibility.py /path/to/motioncam-decoder
"""
import argparse
import pathlib
import subprocess
import sys
import tempfile

UPSTREAM_SOURCES = ("Decoder.cpp", "RawData.cpp", "RawData_Legacy.cpp")


def build_and_run(root: pathlib.Path, decoder: pathlib.Path, cxx: str) -> None:
    for name in UPSTREAM_SOURCES:
        source = decoder / "lib" / name
        if not source.exists():
            raise SystemExit(f"missing upstream source: {source}")
    encoder_sources = sorted(root.glob("src/*.cpp"))
    if not encoder_sources:
        raise SystemExit(f"no encoder sources under {root / 'src'}")

    with tempfile.TemporaryDirectory(prefix="mediacinemaraw-compat-") as tmp:
        build = pathlib.Path(tmp)
        flags = [
            cxx, "-std=c++17", "-O2", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", "-I" + str(root / "include"),
            "-I" + str(decoder / "lib/include"),
            "-I" + str(decoder / "thirdparty"),
        ]
        sources = (
            encoder_sources
            + [root / "tests/CompatibilityTests.cpp"]
            + [decoder / "lib" / name for name in UPSTREAM_SOURCES]
        )
        objects = []
        for source in sources:
            obj = build / (source.stem + ".o")
            # Upstream intentionally uses unaligned access.
            extra = ["-fno-sanitize=alignment"] if decoder in source.parents else []
            subprocess.run(flags + extra + ["-c", str(source), "-o", str(obj)],
                           check=True)
            objects.append(str(obj))
        executable = build / "compatibility-tests"
        subprocess.run(flags + objects + ["-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Round-trip 240 payloads through motioncam-decoder.")
    parser.add_argument("decoder_dir",
                        help="path to a motioncam-decoder checkout")
    parser.add_argument("--cxx", default="clang++",
                        help="compiler to use (default: clang++)")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    build_and_run(root, pathlib.Path(args.decoder_dir).resolve(), args.cxx)


if __name__ == "__main__":
    sys.exit(main())
