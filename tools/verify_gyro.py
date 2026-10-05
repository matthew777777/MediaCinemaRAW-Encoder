#!/usr/bin/env python3
"""Gyro/accelerometer container interop against external decoder checkouts.

Step 1 always runs: files written by this repo's ContainerWriter are opened
with motioncam-decoder's motion APIs (the upstream oracle).

Step 2 is optional: when a MediaCinemaRAW-Decoder checkout is given, the full
encode/decode plus container round-trip runs against the sibling implementation.

Usage:
    python3 tools/verify_gyro.py /path/to/motioncam-decoder [/path/to/MediaCinemaRAW-Decoder]
"""
import argparse
import pathlib
import subprocess
import sys
import tempfile
from typing import List, Optional

UPSTREAM_SOURCES = ("Decoder.cpp", "RawData.cpp", "RawData_Legacy.cpp")
SIBLING_SOURCES = ("src/Decoder.cpp", "src/ContainerReader.cpp",
                   "tests/InteropTests.cpp")


def compile_all(cxx: str, flags: List[str], sources: List[pathlib.Path],
                build: pathlib.Path,
                no_align_prefix: Optional[pathlib.Path] = None) -> List[str]:
    objects = []
    for source in sources:
        obj = build / (source.stem + ".o")
        extra = []
        if no_align_prefix is not None and no_align_prefix in source.parents:
            # Upstream intentionally uses unaligned access.
            extra = ["-fno-sanitize=alignment"]
        subprocess.run(flags + extra + ["-c", str(source), "-o", str(obj)],
                       check=True)
        objects.append(str(obj))
    return objects


def check_sources(label: str, paths: List[pathlib.Path]) -> None:
    for path in paths:
        if not path.exists():
            raise SystemExit(f"missing {label} source: {path}")


def run_upstream_oracle(root: pathlib.Path, decoder: pathlib.Path,
                        build: pathlib.Path, cxx: str) -> None:
    upstream = [decoder / "lib" / name for name in UPSTREAM_SOURCES]
    check_sources("upstream", upstream)
    flags = [
        cxx, "-std=c++17", "-O2", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-I" + str(root / "include"),
        "-I" + str(decoder / "lib/include"),
        "-I" + str(decoder / "thirdparty"),
    ]
    encoder = sorted(root.glob("src/*.cpp"))
    if not encoder:
        raise SystemExit(f"no encoder sources under {root / 'src'}")
    sources = (encoder + [root / "tests/ContainerGyroInterop.cpp"] + upstream)
    check_sources("encoder test", [root / "tests/ContainerGyroInterop.cpp"])
    objects = compile_all(cxx, flags, sources, build, no_align_prefix=decoder)
    exe = build / "gyro-interop"
    subprocess.run(flags + objects + ["-o", str(exe)], check=True)
    subprocess.run([str(exe), str(build / "gyro_interop.mcraw")],
                   check=True, cwd=str(build))


def run_sibling_oracle(root: pathlib.Path, sibling: pathlib.Path,
                       build: pathlib.Path, cxx: str) -> None:
    sources = [sibling / name for name in SIBLING_SOURCES]
    check_sources("sibling", sources)
    flags = [
        cxx, "-std=c++17", "-O2", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-I" + str(root / "include"),
        "-I" + str(sibling / "include"),
    ]
    encoder = sorted(root.glob("src/*.cpp"))
    if not encoder:
        raise SystemExit(f"no encoder sources under {root / 'src'}")
    objects = []
    for source in encoder + sources:
        obj = build / ("sibling_" + source.stem + ".o")
        subprocess.run(flags + ["-c", str(source), "-o", str(obj)], check=True)
        objects.append(str(obj))
    exe = build / "sibling-interop"
    subprocess.run(flags + objects + ["-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, cwd=str(build))


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Verify gyro/accel containers against external decoders.")
    parser.add_argument("decoder_dir",
                        help="path to a motioncam-decoder checkout")
    parser.add_argument("sibling_dir", nargs="?",
                        help="optional path to a MediaCinemaRAW-Decoder checkout")
    parser.add_argument("--cxx", default="clang++",
                        help="compiler to use (default: clang++)")
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parents[1]
    decoder = pathlib.Path(args.decoder_dir).resolve()
    sibling = (pathlib.Path(args.sibling_dir).resolve()
               if args.sibling_dir is not None else None)

    with tempfile.TemporaryDirectory(prefix="mediacinemaraw-gyro-") as tmp:
        run_upstream_oracle(root, decoder, pathlib.Path(tmp), args.cxx)
        if sibling is not None:
            run_sibling_oracle(root, sibling, pathlib.Path(tmp), args.cxx)

    print("gyro interop suites passed")


if __name__ == "__main__":
    sys.exit(main())
