#pragma once

#include <ctime>
#include <string>
#include <vector>

struct CalendarConfigEntry {
  std::string id;
  std::string url;
  std::string tag;
  bool enabled = true;
  time_t lastSyncEpoch = 0;
};

struct CalendarConfig {
  uint8_t version = 1;
  int timezoneOffsetMinutes = 0;
  bool autoSyncOnOpen = true;
  bool autoSyncHourly = false;
  int autoSyncIntervalMinutes = 60;
  int pastDays = 2;     // Days in the past to fetch (0-30)
  int futureDays = 7;   // Days in the future to fetch (0-30)
  std::vector<CalendarConfigEntry> calendars;
};

struct CalendarEvent {
  std::string calendarId;
  std::string tag;
  std::string uid;
  time_t startEpoch = 0;
  time_t endEpoch = 0;
  bool allDay = false;
  std::string summary;
  std::string location;
  std::string description;
};

struct CalendarCache {
  uint8_t version = 1;
  time_t generatedAtEpoch = 0;
  time_t rangeStartEpoch = 0;
  time_t rangeEndEpoch = 0;
  std::vector<CalendarEvent> events;
};

class CalendarStore {
 public:
  static constexpr int MAX_CALENDARS = 8;
  static constexpr size_t MAX_URL_LENGTH = 256;
  static constexpr size_t MAX_TAG_LENGTH = 24;

  static bool loadConfig(CalendarConfig& outConfig);
  static bool saveConfig(const CalendarConfig& config);
  static bool loadCache(CalendarCache& outCache);
  static bool saveCache(const CalendarCache& cache);

  static bool validateConfig(const CalendarConfig& config, std::string& error);
  static std::string nextId(const CalendarConfig& config);
};
