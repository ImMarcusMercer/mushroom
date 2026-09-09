#!/usr/bin/env python3
"""Compile and run portable controller/logging tests using g++ or clang++."""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--sanitize", action="store_true", help="Enable address/undefined sanitizers")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
compiler = shutil.which("g++") or shutil.which("clang++")
if not compiler:
    raise SystemExit("Install a C++ compiler (g++ or clang++) to run the host tests.")
with tempfile.TemporaryDirectory(prefix="mushroom-tests-") as work:
    executable = str(Path(work) / "test_core")
    command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-g"]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command += ["-I", str(root / "MushroomFarmMonitor"), str(root / "tests/test_core.cpp")]
    command += [str(root / "MushroomFarmMonitor" / f) for f in ("ControlCore.cpp", "History.cpp", "LogWriter.cpp")]
    command += ["-o", executable]
    subprocess.run(command, check=True)
    subprocess.run([executable], check=True)
