#!/usr/bin/env python3

import csv
import re
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT_DIR = Path(__file__).parent


def find_schematic() -> Path:
    schematics = [
        p for p in PROJECT_DIR.glob("*.kicad_sch")
        if not p.name.startswith("_autosave-")
    ]
    if len(schematics) != 1:
        sys.exit(f"Expected exactly one schematic in {PROJECT_DIR}, found {len(schematics)}")
    return schematics[0]


SCHEMATIC = find_schematic()
OUTPUT = PROJECT_DIR / "fab" / f"{SCHEMATIC.stem}-BOM.csv"

def kicad_footprint_dirs() -> list[Path]:
    """Find KiCad's stock footprint libraries, which move depending on how KiCad was installed."""
    flatpak = "runtime/org.kicad.KiCad.Library.Footprints/x86_64/stable/active/files/footprints"
    candidates = [
        Path("/usr/share/kicad/footprints"),
        Path("/usr/local/share/kicad/footprints"),
        Path("/var/lib/flatpak") / flatpak,
        Path.home() / ".local/share/flatpak" / flatpak,
    ]
    return [p for p in candidates if p.is_dir()]


FOOTPRINT_SEARCH_PATHS = [PROJECT_DIR / "footprints", *kicad_footprint_dirs()]

GENERIC_DESCRIPTIONS = {
    "unpolarized capacitor",
    "resistor",
    "inductor",
    "light emitting diode",
    "two pin crystal",
}

OUT_FIELDS = [
    "Item", "Designator", "Qty", "Manufacturer", "MPN", "Value", "Footprint", "Type", "Notes",
]


def kicad_cli() -> list[str]:
    """kicad-cli is not on PATH when KiCad is installed as a Flatpak."""
    if shutil.which("kicad-cli"):
        return ["kicad-cli"]
    if shutil.which("flatpak"):
        return ["flatpak", "run", "--command=kicad-cli", "org.kicad.KiCad"]
    sys.exit("kicad-cli not found (install KiCad or its Flatpak)")


def export_raw_bom(schematic: Path, output: Path):
    subprocess.run(
        [
            *kicad_cli(), "sch", "export", "bom",
            "--fields", "ITEM_NUMBER,Reference,QUANTITY,Manufacturer,MPN,Value,Description,Footprint,Notes",
            "--labels", "Item #,Designator,Qty,Manufacturer,Mfg Part #,Value,Description,Footprint,Notes",
            "--group-by", "Value,Footprint",
            "--sort-field", "ITEM_NUMBER",
            "--exclude-dnp",
            "--output", str(output),
            str(schematic),
        ],
        check=True,
    )


def clean_description(desc: str) -> str:
    desc = re.sub(r",\s*small symbol$", "", desc, flags=re.IGNORECASE).strip()
    return "" if desc.lower() in GENERIC_DESCRIPTIONS else desc


def shorten_footprint(fp_full: str) -> str:
    name = fp_full.split(":")[-1] if ":" in fp_full else fp_full
    name = re.sub(r'_(?:\d{3,4}Metric|Pad|L\d+\.\d|\d+\.\d+x).*', '', name)
    name = re.sub(r'-\d+[A-Za-z].*', '', name)  # strip -2Pin, -4Pin etc.
    return name


def find_kicad_mod(footprint_ref: str) -> Path | None:
    if ":" not in footprint_ref:
        return None
    lib, name = footprint_ref.split(":", 1)
    for base in FOOTPRINT_SEARCH_PATHS:
        candidate = base / f"{lib}.pretty" / f"{name}.kicad_mod"
        if candidate.exists():
            return candidate
    return None


def infer_type(footprint_ref: str) -> str:
    mod = find_kicad_mod(footprint_ref)
    if mod:
        content = mod.read_text(errors="ignore")
        if re.search(r'\(pad\s+\S+\s+thru_hole', content):
            return "THT"
        return "SMD"
    fp = footprint_ref.lower()
    if any(x in fp for x in ("tht", "through", "_th_", "dip", "sip")):
        return "THT"
    return "SMD"


def designator_sort_key(designator: str) -> tuple:
    first = designator.split(",")[0].strip()
    m = re.match(r'([A-Za-z]+)(\d+)', first)
    if m:
        return (m.group(1), int(m.group(2)))
    return (first, 0)


def main():
    # Keep the intermediate inside the project: a Flatpak kicad-cli writes into its
    # own sandboxed /tmp, which this process cannot read back.
    raw = PROJECT_DIR / ".bom-raw.csv"

    export_raw_bom(SCHEMATIC, raw)

    with open(raw, newline="") as f:
        rows = list(csv.DictReader(f))
    raw.unlink()

    out_rows = []
    for row in rows:
        desc = clean_description(row["Description"])
        value = row["Value"]
        fp_full = row["Footprint"]
        t = infer_type(fp_full)
        out_rows.append({
            "Item": 0,
            "Designator": row["Designator"],
            "Qty": row["Qty"],
            "Manufacturer": row["Manufacturer"],
            "MPN": row["Mfg Part #"],
            "Value": f"{value} - {desc}" if desc else value,
            "Footprint": shorten_footprint(fp_full),
            "Type": t,
            "Notes": row["Notes"],
            "_sort": (t, *designator_sort_key(row["Designator"])),
        })

    out_rows.sort(key=lambda r: r["_sort"])
    for i, row in enumerate(out_rows, 1):
        row["Item"] = i
        del row["_sort"]

    with open(OUTPUT, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=OUT_FIELDS, quoting=csv.QUOTE_ALL)
        writer.writeheader()
        writer.writerows(out_rows)

    print(f"Wrote {len(out_rows)} lines to {OUTPUT}")


if __name__ == "__main__":
    sys.exit(main())
