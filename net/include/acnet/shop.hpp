#pragma once

#include "acnet/economy.hpp"
#include "acnet/types.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace acnet {

/* Nook's four upgrade tiers, in the original's order
 * (mSP_SHOP_TYPE_* / l_goods_count_table in src/game/m_shop.c). */
enum class ShopTier : std::uint8_t {
    Zakka = 0,           // Nook's Cranny
    Conbini = 1,         // Nook 'n' Go
    Super = 2,           // Nookway
    DepartmentStore = 3, // Nookington's
};

constexpr std::size_t kShopCategories = 6; // furniture, paper, cloth, carpet, wallpaper, diary
constexpr std::size_t kShopMaximumGoods = 39; // mSP_GOODS_COUNT

/* Which of the A/B/C sublists carries the common, uncommon, and rare draws for
 * one category. The original stores this permutation per town in
 * Save_Get(shop).priority_lists and consults it on every roll, so the same item
 * pool yields a different rarity mix from town to town. */
struct ShopCategoryPriority {
    std::uint8_t a = 0; // rarity assigned to list A
    std::uint8_t b = 1;
    std::uint8_t c = 2;
};

/* Rarity values stored in ShopCategoryPriority, matching mSP_LISTTYPE_*. */
constexpr std::uint8_t kShopRarityCommon = 0;
constexpr std::uint8_t kShopRarityUncommon = 1;
constexpr std::uint8_t kShopRarityRare = 2;

struct ShopStockState {
    ShopTier tier = ShopTier::Zakka;
    /* mPr_GetGoodsPower(): shifts the rare/uncommon/common split. Negative
     * values suppress rare draws, positive values promote them. */
    std::int16_t goods_power = 0;
    std::array<ShopCategoryPriority, kShopCategories> priorities{};
    /* The single rare furniture slot the upper tiers roll separately. */
    std::uint16_t rare_item = 0;
};

/* Buy price for an item, from the original per-category price tables
 * (mSP_ItemNo2ItemPrice). Returns 0 for anything the shop does not price.
 * Selling yields price / kShopSellBuyRatio. */
std::uint32_t shop_item_price(std::uint16_t item);

constexpr std::uint32_t kShopSellBuyRatio = 4; // SELL_BUY_RATIO

/* Randomise the per-category A/B/C rarity permutation for a new town. */
void shop_randomise_priorities(ShopStockState& state, const std::function<std::uint64_t()>& random);

/* Roll one day of stock. `random` supplies uniform 64-bit values; the caller
 * owns the source so the server can stay reproducible under test and use its
 * secure generator in production.
 *
 * This reproduces mSP_MakeRandomGoodsList: the tier fixes how many of each
 * category are drawn, each draw picks a rarity from the goods_power-weighted
 * roll in mSP_GetItemList, and the town's priority permutation turns that
 * rarity into one of the A/B/C sublists. Duplicates are avoided unless the
 * chosen sublist is smaller than the number of goods still to place, which is
 * the same escape hatch the original uses. */
std::vector<ShopEntry> roll_shop_stock(ShopStockState& state,
                                       const std::function<std::uint64_t()>& random);

} // namespace acnet
