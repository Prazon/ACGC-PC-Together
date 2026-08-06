#!/usr/bin/env python3
"""Generate the server-side Nook shop tables from the decompiled item data.

The original store rolls its daily stock in ``src/game/m_shop.c`` out of the
per-category item lists in ``src/data/item/*_list.c`` and prices it from the
tables in ``src/data/item/*_price.c``. The dedicated server has to reproduce
that without the game headers, so this script distils both into
``net/src/shop_tables.inc``.

The lists are written as macro expressions (``FTR_START(FTR_SUM_BLUE_BUREAU01)``),
so rather than re-implementing the macro layer in Python this script generates a
small C program that includes the real headers and data files, compiles it with
the same flags the PC build uses, and dumps the resolved values. The compiler is
the only thing that can be trusted to agree with the game.

Usage:  python3 tools/gen_shop_tables.py [--check]
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "net/src/shop_tables.inc"
SHOP_SRC = ROOT / "src/game/m_shop.c"

# Category order is mSP_goods_seg_inf in m_shop.c, indexed by mSP_KIND_*.
CATEGORIES = [
    ("Furniture", "mSP_ftr_list"),
    ("Paper", "mSP_binsen_list"),
    ("Cloth", "mSP_cloth_list"),
    ("Carpet", "mSP_carpet_list"),
    ("Wallpaper", "mSP_wall_list"),
    ("Diary", "mSP_diary_list"),
]

# Price tables consumed by the shop categories above.
PRICE_TABLES = [
    ("Ftr", "ftr_price_table"),
    ("Paper", "binsen_price_table"),
    ("Cloth", "cloth_price_table"),
    ("Carpet", "carpet_price_table"),
    ("Wallpaper", "wall_price_table"),
    ("Diary", "diary_price_table"),
]

LIST_SOURCES = [
    "src/data/item/ftr_list.c",
    "src/data/item/binsen_list.c",
    "src/data/item/cloth_list.c",
    "src/data/item/carpet_list.c",
    "src/data/item/wall_list.c",
    "src/data/item/ftr_price.c",
    "src/data/item/binsen_price.c",
    "src/data/item/cloth_price.c",
    "src/data/item/carpet_price.c",
    "src/data/item/wall_price.c",
    "src/data/item/diary_price.c",
]

CFLAGS = [
    "-DTARGET_PC", "-DVERSION=0", "-DF3DEX_GBI_2", "-DNDEBUG", "-DBUGFIXES",
    "-D_LANGUAGE_C", "-DPC_ENHANCEMENTS", "-DKEYBOARD_TYPING", "-DMOUSE_INPUT",
    "-O0", "-fno-strict-aliasing", "-fwrapv", "-w",
]
INCLUDES = ["-Iinclude", "-Isrc", "-I.", "-Ipc/include", "-Ipc/lib/glad/include",
            "-Ipc/lib/fixnes", "-Inet/include", "-Iserver/include"]


def extract_diary_lists():
    """The diary lists are file-static in m_shop.c, so lift their initialisers.

    Only the text is taken; the macros inside are still resolved by the
    compiler, so this cannot drift from the game's values.
    """
    text = SHOP_SRC.read_text(encoding="utf-8", errors="replace")
    out = []
    for name in ("diary_listA", "diary_listB", "diary_listC"):
        match = re.search(
            r"static\s+mActor_name_t\s+" + name + r"\[\d*\]\s*=\s*\{(.*?)\};",
            text, re.DOTALL)
        if match is None:
            raise SystemExit(f"could not find {name} in {SHOP_SRC}")
        out.append(f"static mActor_name_t {name}[] = {{{match.group(1)}}};")
    out.append(
        "static mActor_name_t* mSP_diary_list[mSP_LIST_NUM] = {"
        " diary_listA, diary_listB, diary_listC };")
    return "\n".join(out)


def build_dumper():
    includes = "\n".join(f'#include "{src}"' for src in LIST_SOURCES)
    externs = "\n".join(
        f"extern unsigned short {table}[];" for _label, table in PRICE_TABLES
        if table != "diary_price_table")
    cat_arrays = ", ".join(name for _label, name in CATEGORIES)
    price_arrays = ", ".join(table for _label, table in PRICE_TABLES)
    return f"""
#include "m_name_table.h"
#include "m_shop.h"
#include "m_room_type.h"
#include <stdio.h>

{includes}

{externs}

{extract_diary_lists()}

static mActor_name_t** g_lists[] = {{ {cat_arrays} }};
static unsigned short* g_prices[] = {{ {price_arrays} }};
static const char* g_cat_names[] = {{ {", ".join('"' + l + '"' for l, _ in CATEGORIES)} }};
static const char* g_price_names[] = {{ {", ".join('"' + l + '"' for l, _ in PRICE_TABLES)} }};

int main(void) {{
    int cat, list, i;
    printf("LISTS %d %d\\n", (int)(sizeof(g_lists) / sizeof(g_lists[0])), mSP_LIST_NUM);
    for (cat = 0; cat < (int)(sizeof(g_lists) / sizeof(g_lists[0])); ++cat) {{
        for (list = 0; list < mSP_LIST_NUM; ++list) {{
            mActor_name_t* items = g_lists[cat] ? g_lists[cat][list] : 0;
            printf("L %s %d", g_cat_names[cat], list);
            if (items != 0) {{
                /* Every list is EMPTY_NO terminated; the bound is a guard
                 * against a malformed table rather than a real limit. */
                for (i = 0; i < 4096 && items[i] != EMPTY_NO; ++i) printf(" %u", (unsigned)items[i]);
            }}
            printf("\\n");
        }}
    }}
    for (cat = 0; cat < (int)(sizeof(g_prices) / sizeof(g_prices[0])); ++cat) {{
        unsigned short* table = g_prices[cat];
        printf("P %s", g_price_names[cat]);
        if (table != 0) for (i = 0; i < 8192 && table[i] != 0xFFFF; ++i) printf(" %u", (unsigned)table[i]);
        printf("\\n");
    }}
    printf("C FTR0_START %u\\n", (unsigned)FTR0_START);
    printf("C FTR1_START %u\\n", (unsigned)FTR1_START);
    printf("C ITM_PAPER_START %u\\n", (unsigned)ITM_PAPER_START);
    printf("C ITM_CLOTH_START %u\\n", (unsigned)ITM_CLOTH_START);
    printf("C ITM_CARPET_START %u\\n", (unsigned)ITM_CARPET_START);
    printf("C ITM_WALL_START %u\\n", (unsigned)ITM_WALL_START);
    printf("C ITM_DIARY_START %u\\n", (unsigned)ITM_DIARY_START);
    printf("C PAPER_UNIQUE_NUM %u\\n", (unsigned)PAPER_UNIQUE_NUM);
    printf("C SELL_BUY_RATIO %u\\n", (unsigned)4);
    return 0;
}}
"""


def run_dumper():
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "dump.c"
        binary = Path(tmp) / "dump"
        source.write_text(build_dumper(), encoding="utf-8")
        compile_cmd = ["gcc", *CFLAGS, *INCLUDES, str(source), "-o", str(binary)]
        result = subprocess.run(compile_cmd, cwd=ROOT, capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write(result.stderr[-4000:])
            raise SystemExit("failed to compile the shop table dumper")
        run = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
        if run.returncode != 0:
            sys.stderr.write(run.stderr[-4000:])
            raise SystemExit("shop table dumper failed")
        return run.stdout


def parse(output):
    lists, prices, constants = {}, {}, {}
    for line in output.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "L":
            lists[(parts[1], int(parts[2]))] = [int(v) for v in parts[3:]]
        elif parts[0] == "P":
            prices[parts[1]] = [int(v) for v in parts[2:]]
        elif parts[0] == "C":
            constants[parts[1]] = int(parts[2])
    return lists, prices, constants


def emit(lists, prices, constants, list_count):
    pool, spans = [], {}
    for label, _ in CATEGORIES:
        for index in range(list_count):
            items = lists.get((label, index), [])
            spans[(label, index)] = (len(pool), len(items))
            pool.extend(items)

    out = [
        "/* Generated by tools/gen_shop_tables.py -- do not edit by hand.",
        " *",
        " * Item pools from the per-category list files in src/data/item (plus",
        " * the file-static diary lists in src/game/m_shop.c) and prices from the",
        " * matching price files, all resolved by the C preprocessor so the macro",
        " * layer cannot drift.",
        " * Regenerate after any change to those tables;",
        " * `python3 tools/gen_shop_tables.py --check` enforces it.",
        " */",
        "",
        "namespace {",
        "",
        f"constexpr std::size_t kShopCategoryCount = {len(CATEGORIES)};",
        f"constexpr std::size_t kShopListCount = {list_count};",
        "",
        "struct ShopListSpan {",
        "    std::uint32_t offset;",
        "    std::uint32_t count;",
        "};",
        "",
        f"constexpr std::uint16_t kShopItemPool[{len(pool)}] = {{",
    ]
    for i in range(0, len(pool), 12):
        out.append("    " + ", ".join(str(v) for v in pool[i:i + 12]) + ",")
    out += ["};", "",
            "constexpr ShopListSpan kShopListSpans[kShopCategoryCount][kShopListCount] = {"]
    for label, _ in CATEGORIES:
        entries = ", ".join(
            "{%d, %d}" % spans[(label, index)] for index in range(list_count))
        out.append(f"    {{{entries}}}, // {label}")
    out += ["};", ""]

    for label, _table in PRICE_TABLES:
        # The dumper keys price rows by label, not by the C variable name.
        values = prices.get(label, [])
        if not values:
            raise SystemExit(f"price table for {label} came out empty")
        out.append(f"constexpr std::uint16_t kShopPrice{label}[{len(values)}] = {{")
        for i in range(0, len(values), 12):
            out.append("    " + ", ".join(str(v) for v in values[i:i + 12]) + ",")
        out += ["};", ""]

    out.append("constexpr const std::uint16_t* kShopPriceTables[kShopCategoryCount] = {")
    out.append("    " + ", ".join(f"kShopPrice{label}" for label, _ in PRICE_TABLES) + ",")
    out += ["};", "",
            "constexpr std::size_t kShopPriceCounts[kShopCategoryCount] = {"]
    out.append("    " + ", ".join(str(len(prices.get(l, []))) for l, _t in PRICE_TABLES) + ",")
    out += ["};", ""]

    for name, value in constants.items():
        out.append(f"constexpr std::uint32_t kShop_{name} = {value};")
    out += ["", "} // namespace", ""]
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    output = run_dumper()
    lists, prices, constants = parse(output)
    header = next(l for l in output.splitlines() if l.startswith("LISTS"))
    list_count = int(header.split()[2])
    generated = emit(lists, prices, constants, list_count)

    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != generated:
            sys.stderr.write(
                f"{OUTPUT.relative_to(ROOT)} is stale; rerun tools/gen_shop_tables.py\n")
            return 1
        return 0

    OUTPUT.write_text(generated, encoding="utf-8")
    populated = sum(1 for v in lists.values() if v)
    print(f"wrote {OUTPUT.relative_to(ROOT)}: {populated} non-empty lists, "
          f"{sum(len(v) for v in lists.values())} item entries, "
          f"{sum(len(v) for v in prices.values())} prices")
    return 0


if __name__ == "__main__":
    sys.exit(main())
