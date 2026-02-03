#include "CalendarStore.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>

#include <algorithm>

namespace {
constexpr char CONFIG_PATH[] = "/.crosspoint/calendars.json";
constexpr char CACHE_PATH[] = "/.crosspoint/calendar_cache.json";
constexpr char CAL_DIR[] = "/.crosspoint/calendars";

bool readFileToString(const char* path, std::string& out) {
  FsFile file;
  if (!SdMan.openFileForRead("CAL", path, file)) {
    return false;
  }
  const size_t size = file.size();
  out.clear();
  out.reserve(size + 1);
  constexpr size_t bufSize = 1024;
  uint8_t buf[bufSize];
  size_t remaining = size;
  while (remaining > 0) {
    const size_t toRead = remaining > bufSize ? bufSize : remaining;
    const size_t read = file.read(buf, toRead);
    if (read == 0) {
      break;
    }
    out.append(reinterpret_cast<const char*>(buf), read);
    remaining -= read;
  }
  file.close();
  return true;
}

bool writeStringToFile(const char* path, const std::string& data) {
  SdMan.mkdir("/.crosspoint");
  FsFile file;
  if (!SdMan.openFileForWrite("CAL", path, file)) {
    return false;
  }
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  file.close();
  return written == data.size();
}
}  // namespace

bool CalendarStore::loadConfig(CalendarConfig& outConfig) {
  outConfig = CalendarConfig();

  std::string content;
  if (!readFileToString(CONFIG_PATH, content)) {
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, content);
  if (err) {
    Serial.printf("[%lu] [CAL] Failed to parse config: %s\n", millis(), err.c_str());
    return false;
  }

  outConfig.version = doc["version"] | 1;
  outConfig.timezoneOffsetMinutes = doc["timezoneOffsetMinutes"] | 0;
  outConfig.autoSyncOnOpen = doc["autoSyncOnOpen"] | true;
  outConfig.autoSyncHourly = doc["autoSyncHourly"] | false;
  outConfig.autoSyncIntervalMinutes = doc["autoSyncIntervalMinutes"] | 60;

  const JsonArray calendars = doc["calendars"].as<JsonArray>();
  if (!calendars.isNull()) {
    outConfig.calendars.clear();
    for (const JsonObject item : calendars) {
      CalendarConfigEntry entry;
      entry.id = item["id"] | "";
      entry.url = item["url"] | "";
      entry.tag = item["tag"] | "";
      entry.enabled = item["enabled"] | true;
      entry.lastSyncEpoch = item["lastSyncEpoch"] | 0;
      outConfig.calendars.push_back(entry);
    }
  }

  return true;
}

bool CalendarStore::saveConfig(const CalendarConfig& config) {
  JsonDocument doc;
  doc["version"] = config.version;
  doc["timezoneOffsetMinutes"] = config.timezoneOffsetMinutes;
  doc["autoSyncOnOpen"] = config.autoSyncOnOpen;
  doc["autoSyncHourly"] = config.autoSyncHourly;
  doc["autoSyncIntervalMinutes"] = config.autoSyncIntervalMinutes;

  JsonArray calendars = doc["calendars"].to<JsonArray>();
  for (const auto& entry : config.calendars) {
    JsonObject item = calendars.add<JsonObject>();
    item["id"] = entry.id;
    item["url"] = entry.url;
    item["tag"] = entry.tag;
    item["enabled"] = entry.enabled;
    item["lastSyncEpoch"] = static_cast<int64_t>(entry.lastSyncEpoch);
  }

  String json;
  serializeJson(doc, json);
  return writeStringToFile(CONFIG_PATH, std::string(json.c_str()));
}

bool CalendarStore::loadCache(CalendarCache& outCache) {
  outCache = CalendarCache();

  std::string content;
  if (!readFileToString(CACHE_PATH, content)) {
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, content);
  if (err) {
    Serial.printf("[%lu] [CAL] Failed to parse cache: %s\n", millis(), err.c_str());
    return false;
  }

  outCache.version = doc["version"] | 1;
  outCache.generatedAtEpoch = doc["generatedAtEpoch"] | 0;
  outCache.rangeStartEpoch = doc["rangeStartEpoch"] | 0;
  outCache.rangeEndEpoch = doc["rangeEndEpoch"] | 0;

  const JsonArray events = doc["events"].as<JsonArray>();
  if (!events.isNull()) {
    outCache.events.clear();
    for (const JsonObject item : events) {
      CalendarEvent ev;
      ev.calendarId = item["calendarId"] | "";
      ev.tag = item["tag"] | "";
      ev.uid = item["uid"] | "";
      ev.startEpoch = item["startEpoch"] | 0;
      ev.endEpoch = item["endEpoch"] | 0;
      ev.allDay = item["allDay"] | false;
      ev.summary = item["summary"] | "";
      ev.location = item["location"] | "";
      ev.description = item["description"] | "";
      outCache.events.push_back(ev);
    }
  }

  return true;
}

bool CalendarStore::saveCache(const CalendarCache& cache) {
  JsonDocument doc;
  doc["version"] = cache.version;
  doc["generatedAtEpoch"] = static_cast<int64_t>(cache.generatedAtEpoch);
  doc["rangeStartEpoch"] = static_cast<int64_t>(cache.rangeStartEpoch);
  doc["rangeEndEpoch"] = static_cast<int64_t>(cache.rangeEndEpoch);

  JsonArray events = doc["events"].to<JsonArray>();
  for (const auto& ev : cache.events) {
    JsonObject item = events.add<JsonObject>();
    item["calendarId"] = ev.calendarId;
    item["tag"] = ev.tag;
    item["uid"] = ev.uid;
    item["startEpoch"] = static_cast<int64_t>(ev.startEpoch);
    item["endEpoch"] = static_cast<int64_t>(ev.endEpoch);
    item["allDay"] = ev.allDay;
    item["summary"] = ev.summary;
    item["location"] = ev.location;
    item["description"] = ev.description;
  }

  String json;
  serializeJson(doc, json);
  return writeStringToFile(CACHE_PATH, std::string(json.c_str()));
}

bool CalendarStore::validateConfig(const CalendarConfig& config, std::string& error) {
  if (static_cast<int>(config.calendars.size()) > MAX_CALENDARS) {
    error = "Too many calendars";
    return false;
  }

  if (config.autoSyncIntervalMinutes != 60) {
    error = "Auto-sync interval must be 60 minutes";
    return false;
  }

  for (const auto& entry : config.calendars) {
    if (entry.id.empty()) {
      error = "Calendar id is required";
      return false;
    }
    if (entry.url.empty()) {
      error = "Calendar url is required";
      return false;
    }
    if (entry.url.size() > MAX_URL_LENGTH) {
      error = "Calendar url is too long";
      return false;
    }
    if (entry.tag.size() > MAX_TAG_LENGTH) {
      error = "Calendar tag is too long";
      return false;
    }
  }

  return true;
}

std::string CalendarStore::nextId(const CalendarConfig& config) {
  int maxId = 0;
  for (const auto& entry : config.calendars) {
    if (entry.id.size() >= 2 && entry.id[0] == 'c') {
      const int val = atoi(entry.id.c_str() + 1);
      if (val > maxId) {
        maxId = val;
      }
    }
  }
  return "c" + std::to_string(maxId + 1);
}
