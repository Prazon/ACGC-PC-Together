#pragma once

#include "acnet/encounter.hpp"   /* TownDate */

#include <cstdint>
#include <string>
#include <vector>

namespace acserver {

/* How a mod says when its holiday happens.
 *
 * The vocabulary deliberately mirrors event_schedule_data's
 * (docs/CUSTOM_CONTENT_SYSTEMS.md sec 2.1) rather than inventing a new one:
 * that is the shape the original scheduler already expresses, so a mod holiday
 * and a stock holiday can describe the same real-world date the same way. */
enum class Recurrence : std::uint8_t {
    FixedDate,     /* month + day */
    NthWeekday,    /* month + week (1-5) + weekday, optionally + days_after */
    LastWeekday,   /* month + weekday, last one in the month */
    EveryWeekday,  /* month + weekday, every occurrence */
    Computed,      /* an engine-derived date: equinoxes */
};

enum class ComputedDate : std::uint8_t { VernalEquinox, AutumnEquinox };

constexpr std::size_t kMaxModHolidays = 64;

struct HolidaySpec {
    std::string id;            /* namespaced <mod-id>.<id> by the host */
    std::string name_key;      /* key into the mod's string table */
    Recurrence recurrence = Recurrence::FixedDate;

    int month = 1;             /* 1-12; ignored for Computed */
    int day = 1;               /* FixedDate only */
    int week = 1;              /* NthWeekday: 1-5 */
    int weekday = 0;           /* 0 = Sunday */
    int days_after = 0;        /* NthWeekday: offset, how Black Friday is expressed */
    ComputedDate computed = ComputedDate::VernalEquinox;

    int hour_from = 0;         /* 0-23 inclusive */
    int hour_to = 23;
    bool marker = true;        /* draw it on the in-game calendar */
    int rumor_days_before = 0; /* villagers gossip for this many days first */
};

/* One holiday resolved against a specific year. `day` is 0 when the spec has no
 * occurrence that year -- an "every Sunday in June" spec yields several, and a
 * fifth-Monday spec may yield none. */
struct ResolvedHoliday {
    std::string id;
    std::string name_key;
    int month = 0;
    int day = 0;
    int hour_from = 0;
    int hour_to = 23;
    bool marker = true;
    int rumor_days_before = 0;
};

/* Days in `month` of `year`, proleptic Gregorian -- the same rule the town
 * clock's own date conversion uses. */
int days_in_month(int year, int month);

/* 0 = Sunday. */
int weekday_of(int year, int month, int day);

/* March equinox day, and September equinox day, for `year`. The original game
 * derives the sports-fair dates the same way (lbRk_VernalEquinoxDay). */
int vernal_equinox_day(int year);
int autumn_equinox_day(int year);

/* Expands one spec into every occurrence in `year`. EveryWeekday yields up to
 * five; the rest yield zero or one. Appends to `out` and returns false without
 * appending if the spec is malformed. */
bool resolve_holiday(const HolidaySpec& spec, int year, std::vector<ResolvedHoliday>& out, std::string& error);

/* True when `date` falls inside the holiday's day and hour window. */
bool holiday_active_at(const ResolvedHoliday& holiday, const acnet::TownDate& date);

/* True when `date` is within the holiday's rumour window: the `rumor_days_before`
 * days leading up to it, not including the day itself. */
bool holiday_rumor_at(const ResolvedHoliday& holiday, int year, const acnet::TownDate& date);

/* Validates ranges a mod could get wrong. Separate from resolve_holiday so the
 * host can reject a bad spec at registration, where the error can name the mod. */
bool validate_holiday(const HolidaySpec& spec, std::string& error);

} // namespace acserver
