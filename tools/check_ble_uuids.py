#!/usr/bin/env python3
"""Assert the BLE UUID sets in the firmware and the webapp stay in sync.

The 10 GATT UUIDs are duplicated in two hand-maintained places:
  - firmware/src/ble.h   (#define BLE_UUID_* "....")
  - webapp/js/ble.js     (export const UUID_* = "....";)

Both READMEs warn they must be kept identical by hand. This script extracts
every UUID string from each file and fails (exit 1) if the two *sets* differ.

Run locally from the repo root:  python tools/check_ble_uuids.py
"""
import re
import sys
from pathlib import Path

# UUID strings, case-insensitive; normalised to lowercase for comparison.
UUID_RE = re.compile(
    r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
    r"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}"
)

REPO_ROOT = Path(__file__).resolve().parent.parent
FIRMWARE = REPO_ROOT / "firmware" / "src" / "ble.h"
WEBAPP = REPO_ROOT / "webapp" / "js" / "ble.js"


def extract(path: Path) -> set[str]:
    if not path.exists():
        sys.exit(f"ERROR: expected file not found: {path}")
    text = path.read_text(encoding="utf-8")
    found = {m.lower() for m in UUID_RE.findall(text)}
    if not found:
        sys.exit(f"ERROR: no UUIDs found in {path} — has its format changed?")
    return found


def main() -> int:
    fw = extract(FIRMWARE)
    web = extract(WEBAPP)

    fw_only = sorted(fw - web)
    web_only = sorted(web - fw)

    if fw_only or web_only:
        print("BLE UUID sets are OUT OF SYNC between firmware and webapp:\n")
        if fw_only:
            print(f"  Only in {FIRMWARE.relative_to(REPO_ROOT)}:")
            for u in fw_only:
                print(f"    {u}")
        if web_only:
            print(f"  Only in {WEBAPP.relative_to(REPO_ROOT)}:")
            for u in web_only:
                print(f"    {u}")
        print("\nKeep firmware/src/ble.h and webapp/js/ble.js in sync.")
        return 1

    print(f"OK: {len(fw)} UUIDs match between firmware and webapp.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
