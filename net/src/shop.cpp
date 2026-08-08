#include "acnet/shop.hpp"

#include <algorithm>
#include <cstddef>

namespace acnet {

#include "shop_tables.inc"

namespace {

/* Category indices, matching mSP_goods_seg_inf order. */
enum : std::size_t {
    kCatFurniture = 0,
    kCatPaper = 1,
    kCatCloth = 2,
    kCatCarpet = 3,
    kCatWallpaper = 4,
    kCatDiary = 5,
};

/* Sublist indices within a category, matching mSP_LIST_*. */
constexpr std::size_t kListA = 0;
constexpr std::size_t kListB = 1;
constexpr std::size_t kListC = 2;

bool item_span(std::size_t category, std::size_t list, std::uint32_t& offset, std::uint32_t& count) {
    if (category >= kShopCategoryCount || list >= kShopListCount) return false;
    const ShopListSpan& span = kShopListSpans[category][list];
    offset = span.offset;
    count = span.count;
    return count != 0;
}

/* mSP_GetItemList: pick a rarity, then map it through the town's priority
 * permutation onto one of the A/B/C sublists. */
std::size_t select_list(const ShopStockState& state, std::size_t category, std::uint64_t roll) {
    const int goods_power = static_cast<int>(state.goods_power);
    int rare_chance;
    int uncommon_chance;
    if (goods_power < 0) {
        uncommon_chance = goods_power + 40;
        rare_chance = 5;
    } else {
        rare_chance = goods_power + 5;
        uncommon_chance = rare_chance + 35;
    }
    const int value = static_cast<int>(roll % 100);
    std::uint8_t rarity = kShopRarityCommon;
    if (value < rare_chance) rarity = kShopRarityRare;
    else if (value < uncommon_chance) rarity = kShopRarityUncommon;

    const ShopCategoryPriority& priority =
        state.priorities[std::min<std::size_t>(category, kShopCategories - 1)];
    if (priority.a == rarity) return kListA;
    if (priority.b == rarity) return kListB;
    if (priority.c == rarity) return kListC;
    /* An unmapped rarity falls back to A, matching the original's NULL guard. */
    return kListA;
}

/* Diaries share the furniture priority permutation (mSP_GetGoodsPriority
 * remaps mSP_KIND_DIARY onto mSP_KIND_FURNITURE). */
std::size_t priority_category(std::size_t category) {
    return category == kCatDiary ? kCatFurniture : category;
}

void draw_items(const ShopStockState& state,
                std::size_t category,
                std::size_t wanted,
                const std::function<std::uint64_t()>& random,
                std::vector<std::uint16_t>& out) {
    if (wanted == 0) return;
    std::size_t placed = 0;
    /* The original loops until it has filled every slot; bound the attempts so
     * a category whose sublists are all empty cannot spin forever. */
    for (std::size_t attempt = 0; placed < wanted && attempt < wanted * 64; ++attempt) {
        const std::size_t list = select_list(state, priority_category(category), random());
        std::uint32_t offset = 0;
        std::uint32_t count = 0;
        if (!item_span(category, list, offset, count)) continue;
        const std::uint16_t item = kShopItemPool[offset + (random() % count)];
        if (item == 0) continue;
        const bool duplicate = std::find(out.begin(), out.end(), item) != out.end();
        /* Duplicates are allowed only when the pool is too small to fill the
         * shelf, which is the original's own escape hatch. */
        if (duplicate && count >= wanted) continue;
        out.push_back(item);
        ++placed;
    }
}

/* mSP_SelectTool: the tools, then the rotating paint colour and a signboard at
 * Nookway and above, then one umbrella.
 *
 * The original picks a tool at random and retries on a duplicate, in a loop
 * with no bound -- fine on a console where a hang is a hang, not acceptable in
 * a server tick. The attempt cap only ever fires if the draw is pathologically
 * unlucky; the shelf is then one tool short for the day rather than stuck. */
void select_tools(ShopStockState& state,
                  const std::uint8_t* counts,
                  const std::function<std::uint64_t()>& random,
                  std::vector<std::uint16_t>& out) {
    constexpr std::uint16_t kTools[4] = {
        static_cast<std::uint16_t>(kShop_ITM_SHOVEL), static_cast<std::uint16_t>(kShop_ITM_NET),
        static_cast<std::uint16_t>(kShop_ITM_ROD), static_cast<std::uint16_t>(kShop_ITM_AXE)};
    const std::uint32_t tier = static_cast<std::uint32_t>(state.tier) & 3U;

    /* Nook's Cranny unlocks tools by lifetime sales; every later tier stocks
     * all four. */
    std::uint32_t tool_max;
    if (tier > kShop_SHOP_TYPE_ZAKKA) tool_max = 4;
    else if (state.sales_sum < kShop_NET_SALES_SUM) tool_max = 1;
    else if (state.sales_sum < kShop_ROD_SALES_SUM) tool_max = 2;
    else if (state.sales_sum < kShop_AXE_SALES_SUM) tool_max = 3;
    else tool_max = 4;

    std::uint32_t wanted = counts[kShop_GOODS_TYPE_TOOL];
    if (wanted > tool_max) wanted = tool_max;
    std::uint32_t added = 0;
    for (std::uint32_t attempt = 0; added < wanted && attempt < wanted * 256U; ++attempt) {
        const std::uint16_t tool = kTools[random() % tool_max];
        if (std::find(out.begin(), out.end(), tool) != out.end()) continue;
        out.push_back(tool);
        ++added;
    }

    if (tier >= kShop_SHOP_TYPE_SUPER) {
        if (state.paint_color >= kShop_PAINT_NUM) state.paint_color = 0;
        out.push_back(static_cast<std::uint16_t>(kShop_ITM_RED_PAINT + state.paint_color));
        ++state.paint_color;
        out.push_back(static_cast<std::uint16_t>(kShop_ITM_SIGNBOARD));
    }

    out.push_back(static_cast<std::uint16_t>(kShop_ITM_UMBRELLA00 + random() % kShop_UMBRELLA_NUM));
}

/* mSP_SelectPlant: a cedar sapling at Nookway and above, then plain saplings,
 * then distinct flower bags. The Halloween and grab-bag-sale variations are not
 * modelled; the server rolls the ordinary shelf on those days. */
void select_plants(const ShopStockState& state,
                   const std::uint8_t* counts,
                   const std::function<std::uint64_t()>& random,
                   std::vector<std::uint16_t>& out) {
    const std::uint32_t tier = static_cast<std::uint32_t>(state.tier) & 3U;
    std::uint32_t saplings = counts[kShop_GOODS_TYPE_SAPLING];
    std::uint32_t flowers = counts[kShop_GOODS_TYPE_PLANT];

    if (tier >= kShop_SHOP_TYPE_SUPER && saplings > 0) {
        out.push_back(static_cast<std::uint16_t>(kShop_ITM_CEDAR_SAPLING));
        --saplings;
    }
    for (; saplings > 0; --saplings) out.push_back(static_cast<std::uint16_t>(kShop_ITM_SAPLING));

    std::vector<bool> used(kShop_FLOWER_NUM, false);
    for (std::uint32_t attempt = 0; flowers > 0 && attempt < kShop_FLOWER_NUM * 256U; ++attempt) {
        const std::size_t index = static_cast<std::size_t>(random() % kShop_FLOWER_NUM);
        if (used[index]) continue;
        used[index] = true;
        out.push_back(static_cast<std::uint16_t>(kShop_ITM_WHITE_PANSY_BAG + index));
        --flowers;
    }
}

} // namespace

std::uint32_t shop_item_price(std::uint16_t item, std::uint16_t native_fruit, std::uint16_t year) {
    if (item == 0) return 0;
    /* The grab bag costs whatever year it is bought in, so it is town state
     * rather than table data and the generated sweep leaves it out. */
    if (item == kShop_HUKUBUKURO_BAG) return year;
    /* A fruit is cheap at home and dear everywhere else. The swept table holds
     * the foreign price, so only the town's own fruit needs the override. */
    if (item == native_fruit) {
        for (std::size_t i = 0; i < kShopFruitCount; ++i) {
            if (kShopFruitIds[i] == item) return kShopNativeFruitPrices[i];
        }
    }
    const std::uint16_t* const end = kShopPriceIds + kShopPriceCount;
    const std::uint16_t* const found = std::lower_bound(kShopPriceIds, end, item);
    if (found == end || *found != item) return 0;
    return kShopPriceValues[found - kShopPriceIds];
}

std::uint32_t shop_sell_price(std::uint16_t item, std::uint16_t native_fruit, std::uint16_t year) {
    return shop_item_price(item, native_fruit, year) / kShopSellBuyRatio;
}

WalletOverflowRule shop_wallet_overflow_rule() {
    return {kShop_WALLET_MAX, kShop_MONEY_BAG_VALUE, static_cast<std::uint16_t>(kShop_MONEY_BAG_ITEM)};
}

void shop_randomise_priorities(ShopStockState& state, const std::function<std::uint64_t()>& random) {
    for (ShopCategoryPriority& priority : state.priorities) {
        std::array<std::uint8_t, 3> rarities{kShopRarityCommon, kShopRarityUncommon, kShopRarityRare};
        /* Fisher-Yates over three entries. */
        for (std::size_t i = rarities.size(); i > 1; --i) {
            const std::size_t j = static_cast<std::size_t>(random() % i);
            std::swap(rarities[i - 1], rarities[j]);
        }
        priority.a = rarities[0];
        priority.b = rarities[1];
        priority.c = rarities[2];
    }
}

std::vector<ShopEntry> roll_shop_stock(ShopStockState& state,
                                       const std::function<std::uint64_t()>& random) {
    const std::uint8_t* counts = kShopGoodsCounts[static_cast<std::size_t>(state.tier) & 3];
    std::vector<std::uint16_t> items;
    items.reserve(kShopMaximumGoods);

    /* Rare furniture is rolled first and kept aside, as the original does. */
    state.rare_item = 0;
    if (counts[kShop_GOODS_TYPE_RARE_FTR] != 0) {
        std::vector<std::uint16_t> rare;
        std::uint32_t offset = 0;
        std::uint32_t count = 0;
        if (item_span(kCatFurniture, kListC, offset, count)) {
            state.rare_item = kShopItemPool[offset + (random() % count)];
            if (state.rare_item != 0) items.push_back(state.rare_item);
        }
        (void)rare;
    }

    draw_items(state, kCatFurniture, counts[kShop_GOODS_TYPE_FTR], random, items);
    draw_items(state, kCatPaper, counts[kShop_GOODS_TYPE_PAPER], random, items);
    /* Only Nookway and above stock a diary. */
    if (state.tier >= ShopTier::Super) draw_items(state, kCatDiary, 1, random, items);
    draw_items(state, kCatCloth, counts[kShop_GOODS_TYPE_CLOTH], random, items);
    draw_items(state, kCatCarpet, counts[kShop_GOODS_TYPE_CARPET], random, items);
    draw_items(state, kCatWallpaper, counts[kShop_GOODS_TYPE_WALL], random, items);

    /* The rest of the shelf is not a daily draw from the rarity lists but the
     * fixed stock mSP_MakeGoodsList appends after it: tools, paint, signboard,
     * umbrella, saplings, and flower bags. They belong in ShopState::stock
     * because a Buy names a row by index, and an index that skipped them would
     * not agree with the shelf the player is looking at. */
    select_tools(state, counts, random, items);
    select_plants(state, counts, random, items);

    std::vector<ShopEntry> stock;
    stock.reserve(items.size());
    for (std::uint16_t item : items) {
        if (stock.size() >= kShopMaximumGoods) break;
        ShopEntry entry;
        entry.item = item;
        entry.price = shop_item_price(item);
        entry.quantity = 1;
        stock.push_back(entry);
    }
    return stock;
}

} // namespace acnet
