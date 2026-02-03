#include "IcsParser.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "util/TimeUtils.h"

namespace {
constexpr size_t MAX_LINE_LENGTH = 1024;

std::string trim(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
  return s.substr(start, end - start);
}

std::string toUpper(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::toupper(c); });
  return out;
}

bool parseDateTime(const std::string& value, int timezoneOffsetMinutes, time_t& outEpoch, bool& outAllDay) {
  std::string v = value;
  if (v.size() < 8) {
    return false;
  }

  outAllDay = false;

  auto parseInt = [](const std::string& s, size_t pos, size_t len) -> int {
    char buf[8] = {0};
    if (pos + len > s.size() || len >= sizeof(buf)) {
      return 0;
    }
    memcpy(buf, s.c_str() + pos, len);
    buf[len] = '\0';
    return atoi(buf);
  };

  // DATE only
  if (v.size() == 8) {
    outAllDay = true;
    std::tm tmLocal = {};
    tmLocal.tm_year = parseInt(v, 0, 4) - 1900;
    tmLocal.tm_mon = parseInt(v, 4, 2) - 1;
    tmLocal.tm_mday = parseInt(v, 6, 2);
    tmLocal.tm_hour = 0;
    tmLocal.tm_min = 0;
    tmLocal.tm_sec = 0;
    outEpoch = mktime(&tmLocal);
    return true;
  }

  bool isUtc = false;
  if (v.back() == 'Z') {
    isUtc = true;
    v.pop_back();
  }

  if (v.size() != 15 || v[8] != 'T') {
    return false;
  }

  std::tm tmVal = {};
  tmVal.tm_year = parseInt(v, 0, 4) - 1900;
  tmVal.tm_mon = parseInt(v, 4, 2) - 1;
  tmVal.tm_mday = parseInt(v, 6, 2);
  tmVal.tm_hour = parseInt(v, 9, 2);
  tmVal.tm_min = parseInt(v, 11, 2);
  tmVal.tm_sec = parseInt(v, 13, 2);

  if (isUtc) {
    outEpoch = TimeUtils::utcToEpoch(tmVal, timezoneOffsetMinutes);
  } else {
    outEpoch = mktime(&tmVal);
  }

  return true;
}

struct ParsedEvent {
  bool inEvent = false;
  bool hasRecurrence = false;
  bool hasStart = false;
  bool hasEnd = false;
  bool allDay = false;
  time_t startEpoch = 0;
  time_t endEpoch = 0;
  std::string summary;
  std::string location;
};

void finalizeEvent(const ParsedEvent& ev, const std::string& calendarId, const std::string& tag,
                   std::vector<CalendarEvent>& outEvents) {
  if (!ev.hasStart) {
    return;
  }
  if (ev.hasRecurrence) {
    Serial.printf("[%lu] [CAL] Skipping recurring event (RRULE)\n", millis());
    return;
  }

  CalendarEvent event;
  event.calendarId = calendarId;
  event.tag = tag;
  event.startEpoch = ev.startEpoch;
  event.endEpoch = ev.hasEnd ? ev.endEpoch : ev.startEpoch + (ev.allDay ? 24 * 60 * 60 : 60 * 60);
  event.allDay = ev.allDay;
  event.summary = ev.summary;
  event.location = ev.location;
  outEvents.push_back(event);
}
}  // namespace

bool IcsParser::parseFile(const std::string& path, const std::string& calendarId, const std::string& tag,
                          int timezoneOffsetMinutes, std::vector<CalendarEvent>& outEvents, std::string& error) {
  FsFile file;
  if (!SdMan.openFileForRead("CAL", path.c_str(), file)) {
    error = "Failed to open .ics file";
    return false;
  }

  std::string currentLine;
  currentLine.reserve(256);
  std::string pendingLine;
  pendingLine.reserve(256);

  ParsedEvent currentEvent;

  char buf[256];
  size_t bytesRead = 0;
  while ((bytesRead = file.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf))) > 0) {
    for (size_t i = 0; i < bytesRead; i++) {
      const char c = buf[i];
      if (c == '\n') {
        // Remove optional \r
        if (!currentLine.empty() && currentLine.back() == '\r') {
          currentLine.pop_back();
        }

        if (!currentLine.empty() && (currentLine[0] == ' ' || currentLine[0] == '\t')) {
          pendingLine += currentLine.substr(1);
        } else {
          if (!pendingLine.empty()) {
            const std::string line = pendingLine;
            pendingLine.clear();

            std::string trimmed = trim(line);
            if (trimmed == "BEGIN:VEVENT") {
              currentEvent = ParsedEvent();
              currentEvent.inEvent = true;
            } else if (trimmed == "END:VEVENT") {
              if (currentEvent.inEvent) {
                finalizeEvent(currentEvent, calendarId, tag, outEvents);
              }
              currentEvent = ParsedEvent();
            } else if (currentEvent.inEvent && !trimmed.empty()) {
              const size_t colon = trimmed.find(':');
              if (colon != std::string::npos) {
                const std::string left = trimmed.substr(0, colon);
                const std::string value = trimmed.substr(colon + 1);
                const std::string name = toUpper(left.substr(0, left.find(';')));

                if (name == "DTSTART") {
                  bool allDay = false;
                  time_t epoch = 0;
                  if (parseDateTime(value, timezoneOffsetMinutes, epoch, allDay)) {
                    currentEvent.hasStart = true;
                    currentEvent.startEpoch = epoch;
                    currentEvent.allDay = allDay;
                  }
                } else if (name == "DTEND") {
                  bool allDay = false;
                  time_t epoch = 0;
                  if (parseDateTime(value, timezoneOffsetMinutes, epoch, allDay)) {
                    currentEvent.hasEnd = true;
                    currentEvent.endEpoch = epoch;
                    currentEvent.allDay = currentEvent.allDay || allDay;
                  }
                } else if (name == "SUMMARY") {
                  currentEvent.summary = value;
                } else if (name == "LOCATION") {
                  currentEvent.location = value;
                } else if (name == "RRULE") {
                  currentEvent.hasRecurrence = true;
                }
              }
            }
          }
          pendingLine = currentLine;
        }

        currentLine.clear();
      } else {
        currentLine.push_back(c);
        if (currentLine.size() > MAX_LINE_LENGTH) {
          currentLine.clear();
        }
      }
    }
  }

  // Process any remaining line
  if (!currentLine.empty()) {
    if (!currentLine.empty() && currentLine.back() == '\r') {
      currentLine.pop_back();
    }
    if (!currentLine.empty() && (currentLine[0] == ' ' || currentLine[0] == '\t')) {
      pendingLine += currentLine.substr(1);
    } else {
      if (!pendingLine.empty()) {
        const std::string line = pendingLine;
        pendingLine.clear();
        // Process line
        std::string trimmed = trim(line);
        if (trimmed == "BEGIN:VEVENT") {
          currentEvent = ParsedEvent();
          currentEvent.inEvent = true;
        } else if (trimmed == "END:VEVENT") {
          if (currentEvent.inEvent) {
            finalizeEvent(currentEvent, calendarId, tag, outEvents);
          }
          currentEvent = ParsedEvent();
        }
      }
      pendingLine = currentLine;
    }
  }

  if (!pendingLine.empty()) {
    std::string trimmed = trim(pendingLine);
    if (trimmed == "BEGIN:VEVENT") {
      currentEvent = ParsedEvent();
      currentEvent.inEvent = true;
    } else if (trimmed == "END:VEVENT") {
      if (currentEvent.inEvent) {
        finalizeEvent(currentEvent, calendarId, tag, outEvents);
      }
    } else if (currentEvent.inEvent && !trimmed.empty()) {
      const size_t colon = trimmed.find(':');
      if (colon != std::string::npos) {
        const std::string left = trimmed.substr(0, colon);
        const std::string value = trimmed.substr(colon + 1);
        const std::string name = toUpper(left.substr(0, left.find(';')));

        if (name == "DTSTART") {
          bool allDay = false;
          time_t epoch = 0;
          if (parseDateTime(value, timezoneOffsetMinutes, epoch, allDay)) {
            currentEvent.hasStart = true;
            currentEvent.startEpoch = epoch;
            currentEvent.allDay = allDay;
          }
        } else if (name == "DTEND") {
          bool allDay = false;
          time_t epoch = 0;
          if (parseDateTime(value, timezoneOffsetMinutes, epoch, allDay)) {
            currentEvent.hasEnd = true;
            currentEvent.endEpoch = epoch;
            currentEvent.allDay = currentEvent.allDay || allDay;
          }
        } else if (name == "SUMMARY") {
          currentEvent.summary = value;
        } else if (name == "LOCATION") {
          currentEvent.location = value;
        } else if (name == "RRULE") {
          currentEvent.hasRecurrence = true;
        }
      }
    }
  }

  file.close();
  return true;
}
