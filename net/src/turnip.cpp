#include "acnet/shop.hpp"

/* The stalk market. Split out of shop.cpp because the client links this and
 * must not drag in the generated shop tables to get it: the client never rolls
 * a shelf or prices a catalogue item -- it reads both from the server -- but it
 * does need turnip_sell_price to quote the counter the same number the server
 * will pay. */

namespace acnet {

namespace {

/* m_kabu_manager.c, transcribed. The odds row is indexed by the *previous*
 * trend and gives the chance of A then B; C takes the remainder. */
constexpr float kTurnipNextTrend[kTurnipTrendCount][2] = {
    {0.5F, 0.3F}, // after A
    {0.6F, 0.2F}, // after B
    {0.6F, 0.3F}, // after C
};

/* Kabu_decide_price_schedule_typeB: a random walk that mostly climbs. */
void turnip_schedule_random(TurnipMarket& market, const std::function<double()>& unit_random) {
    float price = static_cast<float>(market.daily_price[0]);
    const float minimum = static_cast<float>(market.daily_price[0]) * 0.8F;
    float increase_rate = 1.05F;
    float decrease_odds = 0.1F;

    for (std::size_t day = 1; day < kTurnipWeekdays; ++day) {
        if (static_cast<float>(unit_random()) < decrease_odds) {
            price *= 0.7F;
            /* Faithful to the original, including its inverted-looking test:
             * a decrease is pinned to 80% of Sunday whenever it lands above
             * that, so the fall is bounded rather than compounding. */
            if (price > minimum) price = minimum;
            decrease_odds *= 0.3F;
            if (decrease_odds > 0.1F) decrease_odds = 0.1F;
        } else {
            price *= increase_rate;
            increase_rate += 0.01F;
            decrease_odds += 0.14F;
        }
        market.daily_price[day] = static_cast<std::uint16_t>(price);
    }
}

/* Kabu_decide_price_schedule_typeC: a steady decline, 80-95% a day. */
void turnip_schedule_falling(TurnipMarket& market, const std::function<double()>& unit_random) {
    float price = static_cast<float>(market.daily_price[0]);
    for (std::size_t day = 1; day < kTurnipWeekdays; ++day) {
        price *= 0.8F + 0.14999998F * static_cast<float>(unit_random());
        market.daily_price[day] = static_cast<std::uint16_t>(price);
    }
}

/* Kabu_decide_price_schedule_typeA: the random walk, then one day between
 * Monday and Friday replaced by eight times Sunday's price. */
void turnip_schedule_spike(TurnipMarket& market, const std::function<double()>& unit_random) {
    turnip_schedule_random(market, unit_random);
    const float spike = static_cast<float>(market.daily_price[0]) * 8.0F;
    std::size_t day = static_cast<std::size_t>(unit_random() * 5.0);
    if (day > 4) day = 4;
    market.daily_price[day + 1] = static_cast<std::uint16_t>(spike);
}

} // namespace

void roll_turnip_week(TurnipMarket& market, const std::function<double()>& unit_random) {
    /* Kabu_decide_price_sunday: [0.7, 1.3) * 100, so [70, 130) bells. */
    const float sunday = static_cast<float>(1.0 + (unit_random() - 0.5) * 0.6) * 100.0F;
    market.daily_price[0] = static_cast<std::uint16_t>(sunday);

    /* Kabu_decide_trade_market: walk the previous trend's odds row. */
    const std::uint8_t previous = market.trend < kTurnipTrendCount ? market.trend : 0;
    float chosen = static_cast<float>(unit_random());
    std::uint8_t trend = kTurnipTrendCount - 1;
    for (std::uint8_t i = 0; i < kTurnipTrendCount - 1; ++i) {
        if (chosen < kTurnipNextTrend[previous][i]) {
            trend = i;
            break;
        }
        chosen -= kTurnipNextTrend[previous][i];
    }
    market.trend = trend;

    switch (trend) {
        case 0: turnip_schedule_spike(market, unit_random); break;
        case 1: turnip_schedule_random(market, unit_random); break;
        default: turnip_schedule_falling(market, unit_random); break;
    }

    /* Nothing in the original's arithmetic reaches Kabu_PRICE_MAX -- the
     * highest a spike can go is 129 * 8 -- but the wire validates against it,
     * so keep the producer inside what the decoder will accept. */
    for (std::uint16_t& price : market.daily_price) {
        if (price > kTurnipPriceMaximum) price = kTurnipPriceMaximum;
    }
}

std::uint32_t turnip_sell_price(const TurnipMarket& market, std::uint16_t item, int weekday) {
    /* ITM_KABU_START .. ITM_KABU_SPOILED. */
    constexpr std::uint16_t kTurnipItemBase = 0x2F00;
    constexpr std::uint32_t kBundle[4] = {10, 50, 100, 0};
    if (item < kTurnipItemBase || item >= kTurnipItemBase + 4) return 0;
    if (weekday < 0 || static_cast<std::size_t>(weekday) >= kTurnipWeekdays) return 0;
    return static_cast<std::uint32_t>(market.daily_price[static_cast<std::size_t>(weekday)]) *
           kBundle[item - kTurnipItemBase];
}


} // namespace acnet
