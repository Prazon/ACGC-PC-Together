#!/usr/bin/env python3
"""Generate the server-side Nook shop tables from the decompiled item data.

The original store rolls its daily stock in ``src/game/m_shop.c`` out of the
per-category item lists in ``src/data/item/*_list.c`` and prices every item
through ``mSP_ItemNo2ItemPrice``. The dedicated server has to reproduce both
without the game headers, so this script distils them into
``net/src/shop_tables.inc``.

Two dumpers do the work, because the two halves have different needs:

* The **list** dumper includes the real list data files and prints each
  category's A/B/C sublists. The lists are macro expressions
  (``FTR_START(FTR_SUM_BLUE_BUREAU01)``), so letting the compiler resolve them
  is the only way to be sure the macro layer cannot drift.

* The **price** dumper links the game's own ``mSP_ItemNo2ItemPrice`` and calls
  it once per 16-bit item id. Pricing runs items through
  ``mRmTp_FtrItemNo2Item1ItemNo``, which remaps the furniture forms of clothes,
  fish, insects, umbrellas, balloons, diaries, fans, pinwheels, and tools back
  to the item they are priced as -- a large body of logic with its own index
  tables. Calling the real function is exact and cannot drift; re-implementing
  it in C++ would be neither. Unresolved symbols are ignored at link time
  because the pricing call path touches only the price tables, the fish index
  helper, and ``common_data``; the rest of the game is never entered.

Two inputs to pricing are town state rather than table data, so they are
dumped separately and applied by the reader:

* the grab bag, whose price is the current year, and
* fruit, which costs ``mSP_FOREIGN_FRUIT_PRICE`` unless it is the town's own.
  The sweep runs with no native fruit set, so the table holds every fruit's
  foreign price, and each native price is dumped alongside it.

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

# Translation units the price dumper links so mSP_ItemNo2ItemPrice can run:
# the function itself, the furniture/item remap it calls, the fish index
# helper that remap needs, and every price table they read.
PRICE_SOURCES = [
    "src/game/m_shop.c",
    "src/game/m_room_type.c",
    "src/game/m_name_table.c",
    "src/data/item/binsen_price.c",
    "src/data/item/carpet_price.c",
    "src/data/item/cloth_price.c",
    "src/data/item/diary_price.c",
    "src/data/item/fish_price.c",
    "src/data/item/food_price.c",
    "src/data/item/ftr_price.c",
    "src/data/item/insect_price.c",
    "src/data/item/md_price.c",
    "src/data/item/plant_price.c",
    "src/data/item/tool_price.c",
    "src/data/item/wall_price.c",
]

# The five fruits, whose price depends on which one the town grows natively.
FRUITS = ["ITM_FOOD_APPLE", "ITM_FOOD_CHERRY", "ITM_FOOD_PEAR",
          "ITM_FOOD_PEACH", "ITM_FOOD_ORANGE"]

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


def build_list_dumper():
    includes = "\n".join(f'#include "{src}"' for src in LIST_SOURCES)
    cat_arrays = ", ".join(name for _label, name in CATEGORIES)
    return f"""
#include "m_name_table.h"
#include "m_shop.h"
#include "m_room_type.h"
#include <stdio.h>

{includes}

{extract_diary_lists()}

static mActor_name_t** g_lists[] = {{ {cat_arrays} }};
static const char* g_cat_names[] = {{ {", ".join('"' + l + '"' for l, _ in CATEGORIES)} }};

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
    return 0;
}}
"""


def extract_goods_tables():
    """Lift the four per-tier goods count tables, which are file-static.

    Same approach as the diary lists: only the initialiser text is taken, so
    the values are still resolved by the compiler.
    """
    text = SHOP_SRC.read_text(encoding="utf-8", errors="replace")
    out = []
    for name in ("l_zakka_goods", "l_conbini_goods", "l_super_goods", "l_dsuper_goods"):
        match = re.search(
            r"static\s+u8\s+" + name + r"\[[A-Za-z_]*\]\s*=\s*\{(.*?)\};",
            text, re.DOTALL)
        if match is None:
            raise SystemExit(f"could not find {name} in {SHOP_SRC}")
        out.append(f"static unsigned char {name}[] = {{{match.group(1)}}};")
    out.append(
        "static unsigned char* g_goods_counts[] = {"
        " l_zakka_goods, l_conbini_goods, l_super_goods, l_dsuper_goods };")
    return "\n".join(out)


def build_price_dumper():
    fruit_ids = ", ".join(FRUITS)
    return f"""
#include "m_common_data.h"
#include "m_name_table.h"
#include "m_shop.h"
#include "m_room_type.h"
#include "ac_npc_shop_common.h"
#include "m_private.h"
#include <stdio.h>

/* The game's own definition lives in a translation unit this dumper does not
 * link. Pricing reads the year and the town's native fruit out of it and
 * touches nothing else. */
common_data_t common_data;

static mActor_name_t g_fruit[] = {{ {fruit_ids} }};

{extract_goods_tables()}

int main(void) {{
    unsigned i;
    unsigned f;
    int tier;
    int type;
    /* Year zero prices the grab bag at zero, which keeps it out of the swept
     * table entirely; the reader substitutes the live town year instead. */
    common_data.time.rtc_time.year = 0;
    /* No native fruit, so all five fruits report the foreign price and each
     * native price can be dumped on its own below. */
    Save_Set(fruit, 0);
    for (i = 1; i < 0x10000u; ++i) {{
        unsigned price = (unsigned)mSP_ItemNo2ItemPrice((mActor_name_t)i);
        if (price != 0) printf("V %u %u\\n", i, price);
    }}
    for (f = 0; f < sizeof(g_fruit) / sizeof(g_fruit[0]); ++f) {{
        Save_Set(fruit, g_fruit[f]);
        printf("F %u %u\\n", (unsigned)g_fruit[f],
               (unsigned)mSP_ItemNo2ItemPrice(g_fruit[f]));
    }}
    Save_Set(fruit, 0);
    for (tier = 0; tier < mSP_SHOP_TYPE_NUM; ++tier) {{
        printf("G %d", tier);
        for (type = 0; type < mSP_GOODS_TYPE_NUM; ++type)
            printf(" %u", (unsigned)g_goods_counts[tier][type]);
        printf("\\n");
    }}
    printf("K HUKUBUKURO_BAG %u\\n", (unsigned)ITM_HUKUBUKURO_BAG);
    printf("K SELL_BUY_RATIO %u\\n", (unsigned)SELL_BUY_RATIO);
    printf("K SHOP_TIER_COUNT %u\\n", (unsigned)mSP_SHOP_TYPE_NUM);
    printf("K GOODS_TYPE_COUNT %u\\n", (unsigned)mSP_GOODS_TYPE_NUM);
    printf("K GOODS_TYPE_PAPER %u\\n", (unsigned)mSP_GOODS_TYPE_PAPER);
    printf("K GOODS_TYPE_CLOTH %u\\n", (unsigned)mSP_GOODS_TYPE_CLOTH);
    printf("K GOODS_TYPE_FTR %u\\n", (unsigned)mSP_GOODS_TYPE_FTR);
    printf("K GOODS_TYPE_RARE_FTR %u\\n", (unsigned)mSP_GOODS_TYPE_RARE_FTR);
    printf("K GOODS_TYPE_CARPET %u\\n", (unsigned)mSP_GOODS_TYPE_CARPET);
    printf("K GOODS_TYPE_WALL %u\\n", (unsigned)mSP_GOODS_TYPE_WALL);
    printf("K GOODS_TYPE_SAPLING %u\\n", (unsigned)mSP_GOODS_TYPE_SAPLING);
    printf("K GOODS_TYPE_TOOL %u\\n", (unsigned)mSP_GOODS_TYPE_TOOL);
    printf("K GOODS_TYPE_PLANT %u\\n", (unsigned)mSP_GOODS_TYPE_PLANT);
    printf("K SHOP_TYPE_SUPER %u\\n", (unsigned)mSP_SHOP_TYPE_SUPER);
    printf("K SHOP_TYPE_ZAKKA %u\\n", (unsigned)mSP_SHOP_TYPE_ZAKKA);
    printf("K NET_SALES_SUM %u\\n", (unsigned)mSP_NET_SALES_SUM);
    printf("K ROD_SALES_SUM %u\\n", (unsigned)mSP_ROD_SALES_SUM);
    printf("K AXE_SALES_SUM %u\\n", (unsigned)mSP_AXE_SALES_SUM);
    printf("K ITM_SHOVEL %u\\n", (unsigned)ITM_SHOVEL);
    printf("K ITM_NET %u\\n", (unsigned)ITM_NET);
    printf("K ITM_ROD %u\\n", (unsigned)ITM_ROD);
    printf("K ITM_AXE %u\\n", (unsigned)ITM_AXE);
    printf("K ITM_RED_PAINT %u\\n", (unsigned)ITM_RED_PAINT);
    printf("K PAINT_NUM %u\\n", (unsigned)PAINT_NUM);
    printf("K ITM_SIGNBOARD %u\\n", (unsigned)ITM_SIGNBOARD);
    printf("K ITM_UMBRELLA00 %u\\n", (unsigned)ITM_UMBRELLA00);
    printf("K UMBRELLA_NUM %u\\n", (unsigned)UMBRELLA_NUM);
    printf("K ITM_CEDAR_SAPLING %u\\n", (unsigned)ITM_CEDAR_SAPLING);
    printf("K ITM_SAPLING %u\\n", (unsigned)ITM_SAPLING);
    printf("K ITM_WHITE_PANSY_BAG %u\\n", (unsigned)ITM_WHITE_PANSY_BAG);
    printf("K FLOWER_NUM %u\\n", (unsigned)FLOWER_NUM);
    printf("K WALLET_MAX %u\\n", (unsigned)mPr_WALLET_MAX);
    printf("K MONEY_BAG_ITEM %u\\n", (unsigned)ITM_MONEY_30000);
    printf("K MONEY_BAG_VALUE %u\\n", (unsigned)30000);
    return 0;
}}
"""


def run_dumper(name, source_text, extra_sources=(), link_flags=()):
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "dump.c"
        binary = Path(tmp) / "dump"
        source.write_text(source_text, encoding="utf-8")
        compile_cmd = ["gcc", *CFLAGS, *INCLUDES, str(source), *extra_sources,
                       "-o", str(binary), *link_flags]
        result = subprocess.run(compile_cmd, cwd=ROOT, capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write(result.stderr[-4000:])
            raise SystemExit(f"failed to compile the {name} dumper")
        run = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
        if run.returncode != 0:
            sys.stderr.write(run.stderr[-4000:])
            raise SystemExit(f"{name} dumper failed")
        return run.stdout


def parse_lists(output):
    lists = {}
    for line in output.splitlines():
        parts = line.split()
        if parts and parts[0] == "L":
            lists[(parts[1], int(parts[2]))] = [int(v) for v in parts[3:]]
    return lists


def parse_prices(output):
    prices, native_fruit, goods, constants = {}, {}, {}, {}
    for line in output.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "V":
            prices[int(parts[1])] = int(parts[2])
        elif parts[0] == "F":
            native_fruit[int(parts[1])] = int(parts[2])
        elif parts[0] == "G":
            goods[int(parts[1])] = [int(v) for v in parts[2:]]
        elif parts[0] == "K":
            constants[parts[1]] = int(parts[2])
    if not prices:
        raise SystemExit("the price sweep produced nothing")
    if len(native_fruit) != len(FRUITS):
        raise SystemExit("the native fruit sweep is incomplete")
    if len(goods) != constants.get("SHOP_TIER_COUNT"):
        raise SystemExit("the per-tier goods tables are incomplete")
    return prices, native_fruit, goods, constants


def emit(lists, prices, native_fruit, goods, constants, list_count):
    pool, spans = [], {}
    for label, _ in CATEGORIES:
        for index in range(list_count):
            items = lists.get((label, index), [])
            spans[(label, index)] = (len(pool), len(items))
            pool.extend(items)

    out = [
        "/* Generated by tools/gen_shop_tables.py -- do not edit by hand.",
        " *",
        " * Item pools from the per-category list files in src/data/item plus the",
        " * file-static diary lists in src/game/m_shop.c, resolved by the C",
        " * preprocessor so the macro layer cannot drift.",
        " *",
        " * Prices come from calling the game's own mSP_ItemNo2ItemPrice once per",
        " * 16-bit item id, so the furniture/item remap and every per-category",
        " * table are reproduced exactly rather than re-implemented. Ids absent",
        " * from kShopPriceIds are unpriced. Two entries are town state and are",
        " * not in the sweep: the grab bag costs the current year, and a fruit",
        " * costs kShopNativeFruitPrices only in the town that grows it, the",
        " * swept value being the foreign price.",
        " *",
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

    ids = sorted(prices)
    out += [f"constexpr std::size_t kShopPriceCount = {len(ids)};", "",
            "/* Ascending, so the reader can binary search. */",
            "constexpr std::uint16_t kShopPriceIds[kShopPriceCount] = {"]
    for i in range(0, len(ids), 12):
        out.append("    " + ", ".join(str(v) for v in ids[i:i + 12]) + ",")
    out += ["};", "", "constexpr std::uint32_t kShopPriceValues[kShopPriceCount] = {"]
    for i in range(0, len(ids), 12):
        out.append("    " + ", ".join(str(prices[v]) for v in ids[i:i + 12]) + ",")
    out += ["};", ""]

    fruit_ids = sorted(native_fruit)
    out += [f"constexpr std::size_t kShopFruitCount = {len(fruit_ids)};", "",
            "constexpr std::uint16_t kShopFruitIds[kShopFruitCount] = {"]
    out.append("    " + ", ".join(str(v) for v in fruit_ids) + ",")
    out += ["};", "",
            "/* What the fruit costs in the town that grows it. */",
            "constexpr std::uint32_t kShopNativeFruitPrices[kShopFruitCount] = {"]
    out.append("    " + ", ".join(str(native_fruit[v]) for v in fruit_ids) + ",")
    out += ["};", ""]

    tiers = sorted(goods)
    width = len(goods[tiers[0]])
    out += [f"constexpr std::size_t kShopGoodsTypeCount = {width};", "",
            "/* l_goods_count_table: how many of each goods type a tier stocks,",
            " * indexed by kShop_GOODS_TYPE_*. */",
            f"constexpr std::uint8_t kShopGoodsCounts[{len(tiers)}][kShopGoodsTypeCount] = {{"]
    for tier in tiers:
        out.append("    {" + ", ".join(str(v) for v in goods[tier]) + "},")
    out += ["};", ""]

    for name, value in constants.items():
        out.append(f"constexpr std::uint32_t kShop_{name} = {value};")
    out += ["", "} // namespace", ""]
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    list_output = run_dumper("shop list", build_list_dumper())
    lists = parse_lists(list_output)
    header = next(l for l in list_output.splitlines() if l.startswith("LISTS"))
    list_count = int(header.split()[2])

    # The pricing call path reaches only the price tables, the fish index
    # helper, and common_data; the rest of the game links as unresolved and is
    # never entered.
    price_output = run_dumper("shop price", build_price_dumper(), PRICE_SOURCES,
                              ["-Wl,--unresolved-symbols=ignore-all"])
    prices, native_fruit, goods, constants = parse_prices(price_output)
    generated = emit(lists, prices, native_fruit, goods, constants, list_count)

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
          f"{len(prices)} priced items")
    return 0


if __name__ == "__main__":
    sys.exit(main())
