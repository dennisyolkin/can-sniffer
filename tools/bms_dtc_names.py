"""Copy the BMS fault-code names from PSA-RE into the live page, so it can name the DTCs that 0x19 returns.

Usage: uv run --project tools tools/bms_dtc_names.py <PSA-RE checkout> [--page main/web/index.html]

Reads diag/AEE2010.ev/TBMU/BMU_CTE1/dtcs.yml and rewrites the page's <script type="application/json"
id="bms-dtcs"> block: {"<raw>": ["P0560", "name", {"<failure type hex>": "name"}], "_ftb": {...}}, where <raw> is
the DTC's two bytes in hex (what the ECU sends), and "_ftb" holds failure types shared by all codes.
"""

import argparse
import json
import re
from pathlib import Path

import yaml

DTCS = "diag/AEE2010.ev/TBMU/BMU_CTE1/dtcs.yml"
BLOCK = re.compile(r'(<script type="application/json" id="bms-dtcs">)(.*?)(</script>)', re.S)


def english(names: dict | None) -> str:
    return (names or {}).get("en", "")


def failure_types(types: dict | None) -> dict[str, str]:
    return {f"{int(k):02X}": english(v) for k, v in (types or {}).items()}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("psa_re")
    parser.add_argument("--page", default=str(Path(__file__).resolve().parent.parent / "main/web/index.html"))
    args = parser.parse_args()

    data = yaml.safe_load(open(Path(args.psa_re) / DTCS))
    table: dict[str, object] = {"_ftb": failure_types(data.get("failure_types"))}
    for code, entry in data.items():
        if not isinstance(entry, dict) or "raw" not in entry:
            continue
        table[entry["raw"].upper()] = [code, english(entry.get("display_name")), failure_types(entry.get("failure_types"))]

    page = Path(args.page)
    text = page.read_text()
    blob = json.dumps(table, ensure_ascii=False, separators=(",", ":"))
    new, count = BLOCK.subn(lambda m: m.group(1) + blob + m.group(3), text)
    if count != 1:
        raise SystemExit(f"{page}: expected one bms-dtcs block, found {count}")
    page.write_text(new)
    print(f"{page}: {len(table) - 1} BMS fault codes, {len(blob)} bytes")


if __name__ == "__main__":
    main()
