#pragma once

#include <cstdint>
#include <ctime>

namespace TimeUtils {
// Sync system time with NTP (blocking with timeout).
void syncTimeWithNtp();

// Apply timezone offset in minutes (e.g., -300 for EST). Uses a fixed offset (no DST).
void applyTimezoneOffset(int minutesOffset);

// Convert UTC struct tm to epoch with fixed offset, without relying on timegm.
time_t utcToEpoch(const std::tm& tmUtc, int minutesOffset);
}
