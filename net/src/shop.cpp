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

/* Per-tier goods counts, l_goods_count_table in src/game/m_shop.c. Only the
 * categories this authority rolls are kept; saplings, tools, and plants are
 * fixed shelf stock rather than a daily draw. */
struct TierCounts {
    std::uint8_t paper;
    std::uint8_t cloth;
    std::uint8_t furniture;
    std::uint8_t rare_furniture;
    std::uint8_t carpet;
    std::uint8_t wallpaper;
};

constexpr TierCounts kTierCounts[4] = {
    {1, 1, 1, 0, 1, 1}, // l_zakka_goods
    {2, 2, 2, 0, 1, 1}, // l_conbini_goods
    {2, 3, 3, 1, 2, 2}, // l_super_goods
    {4, 5, 5, 1, 3, 3}, // l_dsuper_goods
};

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

} // namespace

std::uint32_t shop_item_price(std::uint16_t item) {
    if (item == 0) return 0;
    /* Furniture: the low two bits are the facing, and the price table is
     * indexed by furniture index (mRmTp_FtrItemNo2FtrIdx). */
    if (item >= kShop_FTR1_START) {
        const std::size_t index = 0x400 + ((item - kShop_FTR1_START) >> 2);
        return index < kShopPriceCounts[kCatFurniture] ? kShopPriceTables[kCatFurniture][index] : 0;
    }
    if (item >= kShop_FTR0_START && item < kShop_ITM_PAPER_START) {
        const std::size_t index = (item - kShop_FTR0_START) >> 2;
        return index < kShopPriceCounts[kCatFurniture] ? kShopPriceTables[kCatFurniture][index] : 0;
    }
    /* Stationery repeats one price block across its design sets, and each
     * successive set costs a multiple of the base. */
    if (item >= kShop_ITM_PAPER_START && item < kShop_ITM_PAPER_START + 1024) {
        const std::uint32_t paper_index = item - kShop_ITM_PAPER_START;
        const std::size_t index = paper_index % kShop_PAPER_UNIQUE_NUM;
        if (index >= kShopPriceCounts[kCatPaper]) return 0;
        return kShopPriceTables[kCatPaper][index] * ((paper_index / kShop_PAPER_UNIQUE_NUM) + 1);
    }
    struct Simple {
        std::uint32_t base;
        std::size_t category;
    };
    const Simple simple[] = {
        {kShop_ITM_CLOTH_START, kCatCloth},
        {kShop_ITM_CARPET_START, kCatCarpet},
        {kShop_ITM_WALL_START, kCatWallpaper},
        {kShop_ITM_DIARY_START, kCatDiary},
    };
    for (const Simple& entry : simple) {
        if (item < entry.base) continue;
        const std::size_t index = item - entry.base;
        if (index < kShopPriceCounts[entry.category])
            return kShopPriceTables[entry.category][index];
    }
    return 0;
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
    const TierCounts& counts = kTierCounts[static_cast<std::size_t>(state.tier) & 3];
    std::vector<std::uint16_t> items;
    items.reserve(kShopMaximumGoods);

    /* Rare furniture is rolled first and kept aside, as the original does. */
    state.rare_item = 0;
    if (counts.rare_furniture != 0) {
        std::vector<std::uint16_t> rare;
        std::uint32_t offset = 0;
        std::uint32_t count = 0;
        if (item_span(kCatFurniture, kListC, offset, count)) {
            state.rare_item = kShopItemPool[offset + (random() % count)];
            if (state.rare_item != 0) items.push_back(state.rare_item);
        }
        (void)rare;
    }

    draw_items(state, kCatFurniture, counts.furniture, random, items);
    draw_items(state, kCatPaper, counts.paper, random, items);
    /* Only Nookway and above stock a diary. */
    if (state.tier >= ShopTier::Super) draw_items(state, kCatDiary, 1, random, items);
    draw_items(state, kCatCloth, counts.cloth, random, items);
    draw_items(state, kCatCarpet, counts.carpet, random, items);
    draw_items(state, kCatWallpaper, counts.wallpaper, random, items);

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
