#include "CalendarSync.h"

#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <WiFi.h>

#include <algorithm>
#include <ctime>

#include "network/HttpDownloader.h"
#include "util/TimeUtils.h"

namespace {
constexpr int CACHE_DAYS = 30;
constexpr char LOCAL_API_PATH[] = "/api/calendar";

inline time_t nowEpoch() {
  const time_t now = time(nullptr);
  return (now < 0) ? 0 : now;
}

// Parse ISO 8601 datetime string to epoch time
// Supports formats: "2024-01-15T09:00:00Z", "2024-01-15T09:00:00+00:00"
time_t parseIso8601(const std::string& isoStr, bool& isAllDay) {
  isAllDay = false;
  if (isoStr.empty()) {
    return 0;
  }

  // Check if it's a date-only format (YYYY-MM-DD)
  if (isoStr.size() == 10 && isoStr[4] == '-' && isoStr[7] == '-') {
    isAllDay = true;
    std::tm tmVal = {};
    tmVal.tm_year = atoi(isoStr.substr(0, 4).c_str()) - 1900;
    tmVal.tm_mon = atoi(isoStr.substr(5, 2).c_str()) - 1;
    tmVal.tm_mday = atoi(isoStr.substr(8, 2).c_str());
    tmVal.tm_hour = 0;
    tmVal.tm_min = 0;
    tmVal.tm_sec = 0;
    return mktime(&tmVal);
  }

  // Parse datetime format
  if (isoStr.size() < 19) {
    return 0;
  }

  std::tm tmVal = {};
  tmVal.tm_year = atoi(isoStr.substr(0, 4).c_str()) - 1900;
  tmVal.tm_mon = atoi(isoStr.substr(5, 2).c_str()) - 1;
  tmVal.tm_mday = atoi(isoStr.substr(8, 2).c_str());
  tmVal.tm_hour = atoi(isoStr.substr(11, 2).c_str());
  tmVal.tm_min = atoi(isoStr.substr(14, 2).c_str());
  tmVal.tm_sec = atoi(isoStr.substr(17, 2).c_str());

  // For now, treat all times as local time
  // TODO: Handle timezone offsets properly
  return mktime(&tmVal);
}

std::string buildLocalApiUrl() {
  // Build URL to local CrossPoint web server
  const IPAddress localIP = WiFi.localIP();
  if (localIP == IPAddress(0, 0, 0, 0)) {
    return "";
  }

  std::string url = "http://";
  url += localIP.toString().c_str();
  url += LOCAL_API_PATH;

  return url;
}
}  // namespace

CalendarSyncResult CalendarSync::syncCalendars(CalendarConfig& config) {
  CalendarSyncResult result;

  TimeUtils::applyTimezoneOffset(config.timezoneOffsetMinutes);
  TimeUtils::syncTimeWithNtp();

  const time_t now = nowEpoch();
  const time_t rangeStart = now;
  const time_t rangeEnd = now + CACHE_DAYS * 24 * 60 * 60;

  std::vector<CalendarEvent> allEvents;

  // Build URL to local CrossPoint web server API
  const std::string apiUrl = buildLocalApiUrl();
  if (apiUrl.empty()) {
    result.ok = false;
    return result;
  }

  Serial.printf("[%lu] [CAL] Fetching calendars from local API: %s\n", millis(), apiUrl.c_str());

  // Fetch JSON response from local API (which merges all calendar sources)
  std::string jsonResponse;
  if (!HttpDownloader::fetchUrl(apiUrl, jsonResponse)) {
    result.ok = false;
    return result;
  }

  // Parse JSON response
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, jsonResponse);
  if (err) {
    Serial.printf("[%lu] [CAL] JSON parse error: %s\n", millis(), err.c_str());
    result.ok = false;
    return result;
  }

  // Helper lambda to parse events from a day object
  auto parseDay = [&](const JsonObject& day) {
    const JsonArray dayEvents = day["events"].as<JsonArray>();
    if (dayEvents.isNull()) {
      return;
    }

    for (const JsonObject& eventObj : dayEvents) {
      CalendarEvent ev;
      ev.calendarId = eventObj["calendar_id"] | "";
      ev.tag = eventObj["tag"] | "";
      ev.uid = eventObj["id"] | "";
      ev.summary = eventObj["title"] | "";
      ev.location = eventObj["location"] | "";
      ev.description = eventObj["description"] | "";

      // Parse start time
      bool startAllDay = false;
      const char* startStr = eventObj["start"];
      if (startStr) {
        ev.startEpoch = parseIso8601(startStr, startAllDay);
      }

      // Parse end time
      bool endAllDay = false;
      const char* endStr = eventObj["end"];
      if (endStr) {
        ev.endEpoch = parseIso8601(endStr, endAllDay);
      }

      // Check isAllDay flag from JSON
      ev.allDay = eventObj["isAllDay"] | false;

      // Validate event has required fields
      if (!ev.uid.empty() && !ev.summary.empty() && ev.startEpoch > 0) {
        allEvents.push_back(ev);
      }
    }
  };

  // Parse past days
  const JsonArray past = doc["past"].as<JsonArray>();
  if (!past.isNull()) {
    for (const JsonObject& day : past) {
      parseDay(day);
    }
  }

  // Parse today
  const JsonObject today = doc["today"].as<JsonObject>();
  if (!today.isNull()) {
    parseDay(today);
  }

  // Parse future days
  const JsonArray future = doc["future"].as<JsonArray>();
  if (!future.isNull()) {
    for (const JsonObject& day : future) {
      parseDay(day);
    }
  }

  // Update last sync time for all enabled calendars
  for (auto& entry : config.calendars) {
    if (entry.enabled) {
      entry.lastSyncEpoch = now;
    }
  }

  Serial.printf("[%lu] [CAL] Parsed %zu total events\n", millis(), allEvents.size());

  std::sort(allEvents.begin(), allEvents.end(), [](const CalendarEvent& a, const CalendarEvent& b) {
    if (a.startEpoch == b.startEpoch) {
      return a.summary < b.summary;
    }
    return a.startEpoch < b.startEpoch;
  });

  result.cache.version = 1;
  result.cache.generatedAtEpoch = now;
  result.cache.rangeStartEpoch = rangeStart;
  result.cache.rangeEndEpoch = rangeEnd;
  result.cache.events = std::move(allEvents);

  result.ok = true;
  return result;
}
