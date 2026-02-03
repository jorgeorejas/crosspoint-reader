#pragma once

#include <string>
#include <vector>

#include "CalendarStore.h"

struct CalendarSyncItem {
  std::string id;
  bool ok = false;
  std::string message;
  size_t eventCount = 0;
};

struct CalendarSyncResult {
  bool ok = false;
  std::vector<CalendarSyncItem> items;
  CalendarCache cache;
};

class CalendarSync {
 public:
  static CalendarSyncResult syncCalendars(CalendarConfig& config);
};
