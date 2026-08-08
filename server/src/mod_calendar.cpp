#include "acserver/mod_calendar.hpp"

#include <cmath>

namespace acserver {
namespace {

bool leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/* Days since 1970-01-01, proleptic Gregorian. Howard Hinnant's days_from_civil,
 * which is what makes weekday_of agree with the town clock's own conversion. */
long long days_from_civil(int year, int month, int day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = static_cast<unsigned>((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<long long>(era) * 146097 + static_cast<long long>(doe) - 719468;
}

} // namespace

int days_in_month(int year, int month) {
    static const int kDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 0;
    if (month == 2 && leap_year(year)) return 29;
    return kDays[month - 1];
}

int weekday_of(int year, int month, int day) {
    const long long days = days_from_civil(year, month, day);
    /* 1970-01-01 was a Thursday (4). Add 4 then normalise, keeping the result
     * non-negative for pre-1970 dates. */
    long long weekday = (days + 4) % 7;
    if (weekday < 0) weekday += 7;
    return static_cast<int>(weekday);
}

/* Meeus' approximation, accurate to well under a day across the years a town
 * clock can reach, and the same shape the original game's table encodes. */
int vernal_equinox_day(int year) {
    const double y = (year - 2000) / 1000.0;
    const double jde = 2451623.80984 + 365242.37404 * y + 0.05169 * y * y
                     - 0.00411 * y * y * y - 0.00057 * y * y * y * y;
    /* Convert to a civil day-of-March. JD 2451623.80984 is 2000-03-20. */
    const long long march20 = days_from_civil(2000, 3, 20);
    const long long day = march20 + static_cast<long long>(std::floor(jde - 2451623.80984 + 0.5));
    const long long march1 = days_from_civil(year, 3, 1);
    const int offset = static_cast<int>(day - march1) + 1;
    return (offset < 1 || offset > 31) ? 20 : offset;
}

int autumn_equinox_day(int year) {
    const double y = (year - 2000) / 1000.0;
    const double jde = 2451810.21715 + 365242.01767 * y - 0.11575 * y * y
                     + 0.00337 * y * y * y + 0.00078 * y * y * y * y;
    const long long sept22 = days_from_civil(2000, 9, 22);
    const long long day = sept22 + static_cast<long long>(std::floor(jde - 2451810.21715 + 0.5));
    const long long sept1 = days_from_civil(year, 9, 1);
    const int offset = static_cast<int>(day - sept1) + 1;
    return (offset < 1 || offset > 30) ? 22 : offset;
}

bool validate_holiday(const HolidaySpec& spec, std::string& error) {
    if (spec.id.empty()) { error = "holiday id is empty"; return false; }
    if (spec.name_key.empty()) { error = "holiday '" + spec.id + "' has no name key"; return false; }
    if (spec.hour_from < 0 || spec.hour_from > 23 || spec.hour_to < 0 || spec.hour_to > 23) {
        error = "holiday '" + spec.id + "': hours must be 0-23";
        return false;
    }
    if (spec.hour_to < spec.hour_from) {
        error = "holiday '" + spec.id + "': hour_to is before hour_from";
        return false;
    }
    if (spec.rumor_days_before < 0 || spec.rumor_days_before > 60) {
        error = "holiday '" + spec.id + "': rumor_days_before must be 0-60";
        return false;
    }
    if (spec.recurrence != Recurrence::Computed && (spec.month < 1 || spec.month > 12)) {
        error = "holiday '" + spec.id + "': month must be 1-12";
        return false;
    }
    switch (spec.recurrence) {
        case Recurrence::FixedDate:
            /* Bounded against a leap year so 29 February is a legal spec: it is
             * a real date, and resolve_holiday is what skips it in the years
             * that do not have one. Validating against a common year instead
             * would reject the holiday outright. */
            if (spec.day < 1 || spec.day > days_in_month(2024, spec.month)) {
                error = "holiday '" + spec.id + "': day is out of range for that month";
                return false;
            }
            break;
        case Recurrence::NthWeekday:
            if (spec.week < 1 || spec.week > 5) {
                error = "holiday '" + spec.id + "': week must be 1-5";
                return false;
            }
            if (spec.days_after < 0 || spec.days_after > 6) {
                error = "holiday '" + spec.id + "': days_after must be 0-6";
                return false;
            }
            [[fallthrough]];
        case Recurrence::LastWeekday:
        case Recurrence::EveryWeekday:
            if (spec.weekday < 0 || spec.weekday > 6) {
                error = "holiday '" + spec.id + "': weekday must be 0-6 (0 = Sunday)";
                return false;
            }
            break;
        case Recurrence::Computed:
            break;
    }
    return true;
}

namespace {

ResolvedHoliday make_resolved(const HolidaySpec& spec, int month, int day) {
    ResolvedHoliday resolved;
    resolved.id = spec.id;
    resolved.name_key = spec.name_key;
    resolved.month = month;
    resolved.day = day;
    resolved.hour_from = spec.hour_from;
    resolved.hour_to = spec.hour_to;
    resolved.marker = spec.marker;
    resolved.rumor_days_before = spec.rumor_days_before;
    return resolved;
}

} // namespace

bool resolve_holiday(const HolidaySpec& spec, int year, std::vector<ResolvedHoliday>& out, std::string& error) {
    if (!validate_holiday(spec, error)) return false;

    switch (spec.recurrence) {
        case Recurrence::FixedDate: {
            /* 29 February in a common year simply does not occur. Silently
             * skipping is right: the holiday is real, this year just has no
             * such date, exactly as the stock scheduler behaves. */
            if (spec.day > days_in_month(year, spec.month)) return true;
            out.push_back(make_resolved(spec, spec.month, spec.day));
            return true;
        }
        case Recurrence::NthWeekday: {
            const int first = weekday_of(year, spec.month, 1);
            int day = 1 + ((spec.weekday - first + 7) % 7) + (spec.week - 1) * 7;
            day += spec.days_after;
            /* A fifth Thursday, or a days_after that runs off the end, means no
             * occurrence this year rather than a wrapped date. */
            if (day > days_in_month(year, spec.month)) return true;
            out.push_back(make_resolved(spec, spec.month, day));
            return true;
        }
        case Recurrence::LastWeekday: {
            const int last = days_in_month(year, spec.month);
            const int last_weekday = weekday_of(year, spec.month, last);
            const int day = last - ((last_weekday - spec.weekday + 7) % 7);
            out.push_back(make_resolved(spec, spec.month, day));
            return true;
        }
        case Recurrence::EveryWeekday: {
            const int first = weekday_of(year, spec.month, 1);
            const int last = days_in_month(year, spec.month);
            for (int day = 1 + ((spec.weekday - first + 7) % 7); day <= last; day += 7) {
                out.push_back(make_resolved(spec, spec.month, day));
            }
            return true;
        }
        case Recurrence::Computed: {
            const bool vernal = spec.computed == ComputedDate::VernalEquinox;
            const int month = vernal ? 3 : 9;
            const int day = vernal ? vernal_equinox_day(year) : autumn_equinox_day(year);
            out.push_back(make_resolved(spec, month, day));
            return true;
        }
    }
    error = "holiday '" + spec.id + "': unknown recurrence";
    return false;
}

bool holiday_active_at(const ResolvedHoliday& holiday, const acnet::TownDate& date) {
    if (holiday.day == 0) return false;
    if (date.month != holiday.month || date.day != holiday.day) return false;
    return date.hour >= holiday.hour_from && date.hour <= holiday.hour_to;
}

bool holiday_rumor_at(const ResolvedHoliday& holiday, int year, const acnet::TownDate& date) {
    if (holiday.day == 0 || holiday.rumor_days_before <= 0) return false;
    /* Compared in absolute days so a window that reaches back over a month or
     * year boundary works without special cases. */
    const long long holiday_day = days_from_civil(year, holiday.month, holiday.day);
    const long long today = days_from_civil(date.year, date.month, date.day);
    const long long delta = holiday_day - today;
    return delta > 0 && delta <= holiday.rumor_days_before;
}

} // namespace acserver
