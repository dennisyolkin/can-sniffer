"""Copy the fault-code names from PSA-RE into the live page, so it can name the DTCs that 0x19 returns.

Usage: uv run --project tools tools/dtc_names.py <PSA-RE checkout> [--page main/web/index.html]

For each ECU the page has requests for, reads diag/AEE2010.ev/<variant>/dtcs.yml and rewrites the page's
<script type="application/json" id="dtcs"> block: {"<ECU>": {"<raw>": ["P0560", "name", {"<failure type hex>":
"name"}], "_ftb": {...}}, ...}. <ECU> is the page's name for it (ECUS in index.html), <raw> the DTC's two bytes in
hex (what the ECU sends), and "_ftb" the failure types shared by all of that ECU's codes.

Variants: the ones PSA-RE lists for D34 (DS 3 Crossback E-Tense), the e-CMP car closest to the ë-C4. PSA-RE has no
ë-C4 entry; the BMS list fitted every code the car returned so far.

Network codes (U, "lost communication with <ECU>") name the other ECU, so they mean the same in every list. A U code
missing from an ECU's own list is borrowed from the first of the other lists that has it (the motor's U11A0 / U110E /
U114A on 2026-11-23). They go once into "_net" ({"<raw>": [code, name, {failure types}, "<source ECU>"]}), which
the page falls back to and marks as borrowed.
"""

import argparse
import json
import re
from pathlib import Path

import yaml

VARIANTS = {
    "BMS": "TBMU/BMU_CTE1",
    "OBC": "OBC/OBC_DCDC_ECMP",
    "Motor": "CMEL/MCU_ECMP",
    "VCU": "VCU/E_VCU",
    "ABS": "ABS/ESPMK100_UDS",
    "Steering": "DAE/DAE_UDS",
    "Park lock": "EPLU/EPLU",
    "Camera": "CVM/CVM_3",
}
BLOCK = re.compile(r'(<script type="application/json" id="dtcs">)(.*?)(</script>)', re.S)


def english(names: dict | None) -> str:
    return (names or {}).get("en", "")


def failure_types(types: dict | None) -> dict[str, str]:
    return {f"{int(k):02X}": english(v) for k, v in (types or {}).items()}


def ecu_table(path: Path) -> dict[str, object]:
    data = yaml.safe_load(open(path))
    table: dict[str, object] = {"_ftb": failure_types(data.get("failure_types"))}
    for code, entry in data.items():
        if not isinstance(entry, dict) or "raw" not in entry:
            continue
        table[entry["raw"].upper()] = [code, english(entry.get("display_name")), failure_types(entry.get("failure_types"))]
    return table


def is_network(raw: str) -> bool:
    return int(raw[0], 16) >> 2 == 3


def network_codes(tables: dict[str, dict[str, object]]) -> dict[str, list[object]]:
    """U codes, first list in VARIANTS order wins: {"<raw>": ["U115E", "name", {failure types}, "<source ECU>"]}."""
    shared: dict[str, list[object]] = {}
    for ecu, table in tables.items():
        for raw, entry in table.items():
            if raw != "_ftb" and is_network(raw) and raw not in shared:
                shared[raw] = [*entry, ecu]
    return shared


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("psa_re")
    parser.add_argument("--page", default=str(Path(__file__).resolve().parent.parent / "main/web/index.html"))
    args = parser.parse_args()

    tables = {ecu: ecu_table(Path(args.psa_re) / "diag/AEE2010.ev" / variant / "dtcs.yml")
              for ecu, variant in VARIANTS.items()}
    counts = ", ".join(f"{ecu} {len(t) - 1}" for ecu, t in tables.items())
    shared = network_codes(tables)
    tables["_net"] = shared
    page = Path(args.page)
    text = page.read_text()
    blob = json.dumps(tables, ensure_ascii=False, separators=(",", ":"))
    new, count = BLOCK.subn(lambda m: m.group(1) + blob + m.group(3), text)
    if count != 1:
        raise SystemExit(f"{page}: expected one dtcs block, found {count}")
    page.write_text(new)
    print(f"{page}: {counts} fault codes, {len(shared)} network codes shared, {len(blob)} bytes")


if __name__ == "__main__":
    main()
