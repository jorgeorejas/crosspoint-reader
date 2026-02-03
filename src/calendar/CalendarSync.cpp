#include "CalendarSync.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>

#include <ctime>

#include <algorithm>

#include "IcsParser.h"
#include "network/HttpDownloader.h"
#include "util/TimeUtils.h"

namespace {
constexpr char CAL_DIR[] = "/.crosspoint/calendars";
constexpr int CACHE_DAYS = 30;

time_t nowEpoch() {
  time_t now = time(nullptr);
  if (now < 0) {
    return 0;
  }
  return now;
}
}  // namespace

CalendarSyncResult CalendarSync::syncCalendars(CalendarConfig& config) {
  CalendarSyncResult result;

  SdMan.mkdir("/.crosspoint");
  SdMan.mkdir(CAL_DIR);

  TimeUtils::applyTimezoneOffset(config.timezoneOffsetMinutes);
  TimeUtils::syncTimeWithNtp();

  const time_t now = nowEpoch();
  const time_t rangeStart = now;
  const time_t rangeEnd = now + CACHE_DAYS * 24 * 60 * 60;

  std::vector<CalendarEvent> allEvents;

  for (auto& entry : config.calendars) {
    CalendarSyncItem item;
    item.id = entry.id;

    if (!entry.enabled) {
      item.ok = true;
      item.message = "disabled";
      result.items.push_back(item);
      continue;
    }

    if (entry.url.empty()) {
      item.ok = false;
      item.message = "missing url";
      result.items.push_back(item);
      continue;
    }

    const std::string filePath = std::string(CAL_DIR) + "/" + entry.id + ".ics";
    const auto downloadResult = HttpDownloader::downloadToFileNoAuth(entry.url, filePath);
    if (downloadResult != HttpDownloader::OK) {
      item.ok = false;
      item.message = "download failed";
      result.items.push_back(item);
      continue;
    }

    std::vector<CalendarEvent> events;
    std::string parseError;
    if (!IcsParser::parseFile(filePath, entry.id, entry.tag, config.timezoneOffsetMinutes, rangeStart, rangeEnd, events,
                              parseError)) {
      item.ok = false;
      item.message = parseError.empty() ? "parse failed" : parseError;
      result.items.push_back(item);
      continue;
    }

    size_t kept = 0;
    for (const auto& ev : events) {
      if (ev.endEpoch < rangeStart || ev.startEpoch > rangeEnd) {
        continue;
      }
      allEvents.push_back(ev);
      kept++;
    }

    entry.lastSyncEpoch = now;
    item.ok = true;
    item.eventCount = kept;
    item.message = "ok";
    result.items.push_back(item);
  }

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
