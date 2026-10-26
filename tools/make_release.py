"""Prepare a KevFM radio release in the repo root, ready to commit and push to GitHub.

    python tools\\make_release.py

Run build.cmd first. This copies build\\yoRadio.ino.bin to kevfm-radio.bin and writes
update.json from the version in kevfm_version.h. Radios that press "Check for update" read
update.json from GitHub (raw.githubusercontent.com) and offer the update when its version
is higher than their own.

Bump KEVFM_VERSION in yoRadio/src/plugins/KevFM/kevfm_version.h before building a release.
"""
import json
import pathlib
import re
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
VERSION_H = ROOT / "yoRadio" / "src" / "plugins" / "KevFM" / "kevfm_version.h"
BUILT_BIN = ROOT / "build" / "yoRadio.ino.bin"
RELEASE_BIN = ROOT / "kevfm-radio.bin"
MANIFEST = ROOT / "update.json"
WEB_DIR = ROOT / "yoRadio" / "data" / "www"
# Web files a release replaces on the radios (fetched from the repo's yoRadio/data/www/).
# Only KevFM's own pages: yoRadio's files never change, and saved Wi-Fi is never touched.
NUL = bytes(1)
WEB_FILES = ["kevfm.html", "setup.html", "firmware.html"]


def main():
    match = re.search(r'#define\s+KEVFM_VERSION\s+"([^"]+)"', VERSION_H.read_text(encoding="utf-8"))
    if not match:
        sys.exit(f"KEVFM_VERSION not found in {VERSION_H}")
    version = match.group(1)
    if not BUILT_BIN.exists():
        sys.exit(f"{BUILT_BIN} missing: run build.cmd first")
    if NUL + version.encode() + NUL not in BUILT_BIN.read_bytes():   # the KEVFM_VERSION string
        sys.exit(f"build\\yoRadio.ino.bin is not version {version}: run build.cmd again")
    for name in WEB_FILES:
        if not (WEB_DIR / name).exists():
            sys.exit(f"web file missing: {WEB_DIR / name}")
    previous = json.loads(MANIFEST.read_text(encoding="utf-8")).get("version") if MANIFEST.exists() else None
    if previous == version:
        print(f"note: update.json already says {version}; radios on {version} won't see a new update")
    shutil.copyfile(BUILT_BIN, RELEASE_BIN)
    manifest = {"version": version, "file": RELEASE_BIN.name, "size": RELEASE_BIN.stat().st_size, "web": WEB_FILES}
    MANIFEST.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Release {version}: {RELEASE_BIN.name} ({manifest['size']:,} bytes), update.json written.")
    print("Now commit and push (see README, 'Publishing an update').")


if __name__ == "__main__":
    main()
