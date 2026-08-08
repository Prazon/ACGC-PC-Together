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
    /* Lifetime sales. Nook's Cranny, and only Nook's Cranny, unlocks the net,
     * rod, and axe as this passes mSP_{NET,ROD,AXE}_SALES_SUM. */
    std::uint32_t sales_sum = 0;
    /* Which paint colour is next on the shelf; rotates once per roll at
     * Nookway and above. Persisted, like the original's Shop_c field. */
    std::uint16_t paint_color = 0;
};

constexpr std::uint32_t kShopSellBuyRatio = 4; // SELL_BUY_RATIO

/* Buy price for an item, generated from the original mSP_ItemNo2ItemPrice.
 * Returns 0 for anything the game does not price.
 *
 * Two prices depend on the town rather than the tables, so the caller supplies
 * them: `native_fruit` is the fruit the town grows (Save_Get(fruit)), which
 * costs mSP_FOREIGN_FRUIT_PRICE anywhere else, and `year` is what the new
 * year's grab bag costs. Both default to "unknown", which prices fruit as
 * foreign and the bag at nothing -- correct for shop stock, which contains
 * neither. */
std::uint32_t shop_item_price(std::uint16_t item,
                              std::uint16_t native_fruit = 0,
                              std::uint16_t year = 0);

/* What Nook pays for an item, price / kShopSellBuyRatio, matching the
 * mSP_ItemNo2ItemPrice(item) / SELL_BUY_RATIO the shop dialogue quotes. */
std::uint32_t shop_sell_price(std::uint16_t item,
                              std::uint16_t native_fruit = 0,
                              std::uint16_t year = 0);

/* The wallet cannot hold more than mPr_WALLET_MAX bells. Above it the original
 * peels off 30,000 at a time into a money bag item in the pockets, which is why
 * selling something valuable hands back a bag. Reported here so the town
 * runtime can configure EconomyAuthority with the game's own numbers rather
 * than the authority hardcoding them. */
struct WalletOverflowRule {
    std::uint32_t maximum;    // mPr_WALLET_MAX
    std::uint32_t chunk;      // bells moved into one bag
    std::uint16_t bag_item;   // ITM_MONEY_30000
};
WalletOverflowRule shop_wallet_overflow_rule();

/* Randomise the per-category A/B/C rarity permutation for a new town. */
void shop_randomise_priorities(ShopStockState& state, const std::function<std::uint64_t()>& random);

/* The stalk market. One town has one weekly schedule, and it has to be server
 * state for two independent reasons: every client used to roll its own, so no
 * two players were quoted the same price for the same turnip; and turnips are
 * absent from the static price tables, because the original prices them from
 * this schedule rather than from mSP_ItemNo2ItemPrice, so a sale priced through
 * shop_sell_price came back as zero and the economy refused it outright.
 *
 * daily_price is indexed by weekday with Sunday at 0, exactly as
 * Kabu_price_c::daily_price is. Sunday's entry is the price Joan sells at; the
 * other six are what Nook pays. */
constexpr std::size_t kTurnipWeekdays = 7;
constexpr std::uint16_t kTurnipPriceMaximum = 2000; // Kabu_PRICE_MAX
/* Kabu_TRADE_MARKET_TYPE_NUM: A spike, B random, C falling. */
constexpr std::uint8_t kTurnipTrendCount = 3;

struct TurnipMarket {
    std::array<std::uint16_t, kTurnipWeekdays> daily_price{};
    std::uint8_t trend = 0;
    Revision revision = 1;

    bool operator==(const TurnipMarket& other) const {
        return daily_price == other.daily_price && trend == other.trend && revision == other.revision;
    }
};

/* Reproduces Kabu_decide_price_schedule: a new Sunday price, a new trend chosen
 * from the old one's odds, and the six selling days that follow from it.
 * `unit_random` returns [0, 1), standing in for the original's fqrand(). */
void roll_turnip_week(TurnipMarket& market, const std::function<double()>& unit_random);

/* What Nook pays for one turnip stack today, or 0 if `item` is not a turnip.
 * The bundle sizes are aNSC_kabu_sum {10, 50, 100, 0} -- a spoiled turnip is
 * worth nothing and stays worth nothing. Turnips deliberately bypass
 * kShopSellBuyRatio: the original multiplies the schedule price by the bundle
 * size and does not divide. */
std::uint32_t turnip_sell_price(const TurnipMarket& market, std::uint16_t item, int weekday);

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
