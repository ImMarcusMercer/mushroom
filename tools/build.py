#!/usr/bin/env python3
"""Optional pinned Arduino CLI build. Does not upload to hardware."""
from pathlib import Path
import argparse
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--install", action="store_true", help="Install pinned ESP32 platform and libraries first")
parser.add_argument("--headless", action="store_true", help="Compile without TFT support")
args = parser.parse_args()
cli = shutil.which("arduino-cli")
if not cli:
    raise SystemExit("Install Arduino CLI and add arduino-cli to PATH first, or use Arduino IDE.")
root = Path(__file__).resolve().parents[1]
url = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"
def run(*items):
    subprocess.run([cli, *items], check=True)
if args.install:
    run("core", "update-index", "--additional-urls", url)
    run("core", "install", "esp32:esp32@3.3.11", "--additional-urls", url)
    run("lib", "update-index")
    run("lib", "install", "ArduinoJson@7.3.2", "TFT_eSPI@2.5.43", "RTClib@2.1.4",
        "Adafruit MCP23017 Arduino Library@2.3.2", "Adafruit BusIO@1.17.4")
command = ["compile", "--fqbn", "esp32:esp32:esp32", "--warnings", "all"]
if args.headless:
    command += ["--build-property", "compiler.cpp.extra_flags=-DMUSHROOM_ENABLE_TFT=0"]
run(*command, str(root / "MushroomFarmMonitor"))
