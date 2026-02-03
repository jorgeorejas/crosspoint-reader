#pragma once

#include <string>
#include <vector>

#include "CalendarStore.h"

class IcsParser {
 public:
  static bool parseFile(const std::string& path, const std::string& calendarId, const std::string& tag,
                        int timezoneOffsetMinutes, std::vector<CalendarEvent>& outEvents, std::string& error);
};
