#!/usr/bin/env python3
"""Generate the server-side encounter availability tables from the decompiled
spawn overlays.

The original game decides which fish or insect may appear from
``src/actor/ac_set_ovl_gyoei.c`` and ``src/actor/ac_set_ovl_insect.c``. The
dedicated server has to answer a narrower question -- "is the species this
client says it caught legal right now?" -- so this script distills those tables
into a per-species availability bitmask keyed by month and time slot.

Transcribing the tables by hand would silently drift from the game. Generating
them keeps ``net/src/encounter_tables.inc`` derived from the only authority in
the tree.

Usage:  python3 tools/gen_encounter_tables.py [--check]

``--check`` regenerates into memory and fails if the committed file differs,
which is what CI runs.
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GYOEI_SRC = ROOT / "src/actor/ac_set_ovl_gyoei.c"
INSECT_SRC = ROOT / "src/actor/ac_set_ovl_insect.c"
FISH_ENUM = ROOT / "include/ac_gyoei.h"
INSECT_ENUM = ROOT / "include/ac_insect_h.h"
OUTPUT = ROOT / "net/src/encounter_tables.inc"

# Fish time slots (include/ac_set_ovl_gyoei.h):
#   TIME_0 9pm-3:59am, TIME_1 4am-8:59am, TIME_2 9am-3:59pm, TIME_3 4pm-8:59pm
FISH_TIME_SLOTS = 4
# Insect terms (include/ac_set_ovl_insect.h):
#   TERM0 11pm-3:59am, TERM1 4am-7:59am, TERM2 8am-3:59pm,
#   TERM3 4pm-4:59pm,  TERM4 5pm-6:59pm, TERM5 7pm-10:59pm
INSECT_TERMS = 6
MONTHS = 12
# Fish tables split each month into a beginning and a latter half.
FISH_HALVES = 2


COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)


def read(path):
    """Read a C source file with comments removed.

    The month tables interleave `// t1` slot labels between entries, which
    otherwise breaks the brace-run matching below.
    """
    return COMMENT_RE.sub("", path.read_text(encoding="utf-8", errors="replace"))


def parse_enum(text, first_member, stop_member):
    """Return {MEMBER_SUFFIX: index} for a simple sequential C enum."""
    start = text.index(first_member)
    end = text.index(stop_member, start)
    body = text[start:end]
    prefix = first_member[: first_member.index("_", first_member.index("_") + 1) + 1]
    names = []
    for line in body.splitlines():
        line = line.split("/*")[0].split("//")[0].strip().rstrip(",")
        if not line or "=" in line:
            continue
        if line.startswith(prefix) or line.startswith(first_member.rsplit("_", 1)[0]):
            names.append(line)
    return names


def fish_species_index(text):
    """aGYO_TYPE_* -> index, in declaration order up to aGYO_TYPE_NUM."""
    names = parse_enum(text, "aGYO_TYPE_CRUCIAN_CARP", "aGYO_TYPE_NUM")
    return {n[len("aGYO_TYPE_"):]: i for i, n in enumerate(names)}


def insect_species_index(text):
    names = parse_enum(text, "aINS_INSECT_TYPE_COMMON_BUTTERFLY", "aINS_INSECT_TYPE_NUM")
    return {n[len("aINS_INSECT_TYPE_"):]: i for i, n in enumerate(names)}


SPAWN_RE = re.compile(r"(FISH|INSECT)_SPAWN\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\d+)\s*\)")
ARRAY_RE = re.compile(
    r"static\s+aSO[GI]_term_info_c\s+(\w+)\s*\[\s*\d*\s*\]\s*=\s*\{(.*?)\n\};",
    re.DOTALL,
)
LIST_RE = re.compile(
    r"static\s+aSO[GI]_term_list_c\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\n\};",
    re.DOTALL,
)
LIST_ENTRY_RE = re.compile(r"\{\s*(\d+)\s*,\s*(\w+)\s*\}")
MONTH_RE = re.compile(
    r"static\s+aSO[GI]_term_list_c\s*\*?\s*(\w+)\[lbRTC_MONTHS_MAX\]\[[^\]]*\]\s*=\s*\{(.*?)\n\};",
    re.DOTALL,
)


def parse_info_arrays(text):
    """name -> [(species_name, area, weight), ...]"""
    arrays = {}
    for name, body in ARRAY_RE.findall(text):
        arrays[name] = [
            (species, area, int(weight))
            for _kind, species, area, weight in SPAWN_RE.findall(body)
        ]
    return arrays


def parse_term_lists(text):
    """name -> [info_array_name or None, ...] indexed by time slot"""
    lists = {}
    for name, body in LIST_RE.findall(text):
        slots = []
        for chunk in body.split("},"):
            match = LIST_ENTRY_RE.search(chunk + "}")
            slots.append(match.group(2) if match else None)
        lists[name] = [s for s in slots if s is not None]
    return lists


def parse_month_table(text, table_name):
    """table_name[12][halves] -> [[term_list_name or None, ...], ...]"""
    for name, body in MONTH_RE.findall(text):
        if name != table_name:
            continue
        rows = []
        # Each month is one brace-delimited row of comma-separated identifiers.
        for row in re.findall(r"\{([^{}]*)\}", body):
            entries = [e.strip() for e in row.split(",") if e.strip()]
            rows.append([None if e == "NULL" else e for e in entries])
        return rows
    return []


def build_fish(species_index):
    text = read(GYOEI_SRC)
    arrays = parse_info_arrays(text)
    term_lists = parse_term_lists(text)
    # bit layout per month: half * FISH_TIME_SLOTS + slot  (2 * 4 = 8 bits)
    avail = [[0] * MONTHS for _ in species_index]
    areas = [set() for _ in species_index]
    for table in ("r_month", "s_month", "p_month"):
        rows = parse_month_table(text, table)
        if not rows:
            print(f"warning: month table {table} not found", file=sys.stderr)
            continue
        for month, halves in enumerate(rows):
            for half, list_name in enumerate(halves[:FISH_HALVES]):
                if list_name is None:
                    continue
                for slot, array_name in enumerate(term_lists.get(list_name, [])[:FISH_TIME_SLOTS]):
                    for species, area, _weight in arrays.get(array_name, []):
                        index = species_index.get(species)
                        if index is None:
                            continue
                        avail[index][month] |= 1 << (half * FISH_TIME_SLOTS + slot)
                        areas[index].add(area)
    return avail, areas


def build_insects(text, arrays, species_index):
    """l_insect_month stores {count, array} pairs inline rather than pointers."""
    avail = [[0] * MONTHS for _ in species_index]
    match = re.search(
        r"static\s+aSOI_term_list_c\s+l_insect_month\[lbRTC_MONTHS_MAX\]\[aSOI_TERM_NUM\]\s*=\s*\{(.*?)\n\};",
        text,
        re.DOTALL,
    )
    if not match:
        raise SystemExit("l_insect_month not found in " + str(INSECT_SRC))
    rows = re.findall(r"\{\s*((?:\{[^{}]*\}\s*,?\s*)+)\}", match.group(1))
    if len(rows) != MONTHS:
        raise SystemExit(f"expected {MONTHS} insect month rows, parsed {len(rows)}")
    for month, row in enumerate(rows):
        entries = LIST_ENTRY_RE.findall(row)
        if len(entries) != INSECT_TERMS:
            raise SystemExit(
                f"month {month + 1}: expected {INSECT_TERMS} terms, parsed {len(entries)}"
            )
        for term, (_count, array_name) in enumerate(entries):
            for species, _area, _weight in arrays.get(array_name, []):
                index = species_index.get(species)
                if index is None:
                    continue
                avail[index][month] |= 1 << term
    return avail


def emit(fish_avail, insect_avail):
    lines = [
        "/* Generated by tools/gen_encounter_tables.py -- do not edit by hand.",
        " *",
        " * Availability distilled from src/actor/ac_set_ovl_gyoei.c and",
        " * src/actor/ac_set_ovl_insect.c. Regenerate after any change to those",
        " * tables; `python3 tools/gen_encounter_tables.py --check` enforces it.",
        " *",
        " * Fish bit layout per month:   half * 4 + time_slot  (2 halves, 4 slots)",
        " * Insect bit layout per month: term                  (6 terms)",
        " */",
        "",
        "namespace {",
        "",
        f"constexpr std::size_t kEncounterMonths = {MONTHS};",
        f"constexpr std::size_t kFishSpeciesCount = {len(fish_avail)};",
        f"constexpr std::size_t kInsectSpeciesCount = {len(insect_avail)};",
        "",
        "constexpr std::uint8_t kFishAvailability[kFishSpeciesCount][kEncounterMonths] = {",
    ]
    for index, months in enumerate(fish_avail):
        values = ", ".join(f"0x{v:02X}" for v in months)
        lines.append(f"    {{{values}}}, // species {index}")
    lines.append("};")
    lines.append("")
    lines.append("constexpr std::uint8_t kInsectAvailability[kInsectSpeciesCount][kEncounterMonths] = {")
    for index, months in enumerate(insect_avail):
        values = ", ".join(f"0x{v:02X}" for v in months)
        lines.append(f"    {{{values}}}, // species {index}")
    lines.append("};")
    lines.append("")
    lines.append("} // namespace")
    lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    fish_index = fish_species_index(read(FISH_ENUM))
    insect_index = insect_species_index(read(INSECT_ENUM))

    fish_avail, _fish_areas = build_fish(fish_index)

    insect_text = read(INSECT_SRC)
    insect_avail = build_insects(insect_text, parse_info_arrays(insect_text), insect_index)

    generated = emit(fish_avail, insect_avail)

    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != generated:
            print(
                f"{OUTPUT.relative_to(ROOT)} is stale; rerun tools/gen_encounter_tables.py",
                file=sys.stderr,
            )
            return 1
        return 0

    OUTPUT.write_text(generated, encoding="utf-8")
    reachable_fish = sum(1 for months in fish_avail if any(months))
    reachable_insects = sum(1 for months in insect_avail if any(months))
    print(
        f"wrote {OUTPUT.relative_to(ROOT)}: "
        f"{reachable_fish}/{len(fish_avail)} fish, "
        f"{reachable_insects}/{len(insect_avail)} insects reachable"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
