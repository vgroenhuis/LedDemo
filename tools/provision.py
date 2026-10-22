#!/usr/bin/env python3
"""Builds the images for the first USB flash of a CloSer demo board.

    . ~/esp/idf-env.sh                        # ESP-IDF environment
    python tools/provision.py [name]          # after: idf.py build; name: the demo id, default turtle

One firmware serves several boards: each is named after the demo id in its key (turtle, turtle-2, ...).
Reads the Wi-Fi settings  {"ssid": ..., "password": ..., "base_url": ...}  from local/<name>.json (turtle:
local/provision.json) and the key from local/<name>.key (turtle: local/device.key; `python -m labdemos.cli add <name>`),
and writes into local/:
    <name>-factory.bin   everything (bootloader, partitions, firmware, settings): flash once at 0x0
    <name>-settings.bin  only the Wi-Fi settings and key: flash at 0x9000 to change them later

These files contain the Wi-Fi password and the device key: keep them private (the folder isn't in git).
The over-the-air updates never touch the settings.
"""
import csv
import json
import os
import subprocess
import sys
from pathlib import Path

here = Path(__file__).resolve().parent.parent
local = here / "local"
build = here / "build"
name = sys.argv[1] if len(sys.argv) > 1 else "turtle"
conf = json.loads((local / ("provision.json" if name == "turtle" else f"{name}.json")).read_text())
key = (local / ("device.key" if name == "turtle" else f"{name}.key")).read_text().strip()
if not key.startswith(name + "."):
    sys.exit(f"the key in local/ is not for {name}")
idf = Path(os.environ["IDF_PATH"])
NVS_SIZE = "0x6000"

with (local / "settings.csv").open("w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["key", "type", "encoding", "value"])
    w.writerow(["closer", "namespace", "", ""])
    w.writerow(["wifi_ssid", "data", "string", conf["ssid"]])
    w.writerow(["wifi_pass", "data", "string", conf.get("password", "")])
    w.writerow(["base_url", "data", "string", conf.get("base_url", "https://closer.ram.eemcs.utwente.nl/labdemos")])
    w.writerow(["key", "data", "string", key])
(local / "settings.csv").chmod(0o600)

subprocess.check_call([sys.executable, str(idf / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"),
                       "generate", str(local / "settings.csv"), str(local / f"{name}-settings.bin"), NVS_SIZE],
                      stdout=subprocess.DEVNULL)

flash = json.loads((build / "flasher_args.json").read_text())
parts = [(int(off, 16), build / name) for off, name in flash["flash_files"].items()]
parts.append((0x9000, local / f"{name}-settings.bin"))
cmd = [sys.executable, "-m", "esptool", "--chip", "esp32c5", "merge_bin", "-o", str(local / f"{name}-factory.bin"),
       "--flash_mode", flash["flash_settings"]["flash_mode"], "--flash_size", flash["flash_settings"]["flash_size"],
       "--flash_freq", flash["flash_settings"]["flash_freq"]]
for off, path in sorted(parts):
    cmd += [hex(off), str(path)]
subprocess.check_call(cmd, stdout=subprocess.DEVNULL)
for n in (f"{name}-factory.bin", f"{name}-settings.bin"):
    (local / n).chmod(0o600)
    print(f"{local / n}  {(local / n).stat().st_size} bytes")
(local / "settings.csv").unlink()           # (it holds the password and key in plain text)
