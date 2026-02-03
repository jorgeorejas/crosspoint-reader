#include "IcsParser.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include "util/TimeUtils.h"

namespace {
constexpr size_t MAX_LINE_LENGTH = 1024;
constexpr int MAX_OCCURRENCES_PER_EVENT = 1000;

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

std::vector<std::string> split(const std::string& s, char delim) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t pos = s.find(delim, start);
    if (pos == std::string::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

std::string unescapeText(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); i++) {
    if (value[i] == '\\' && i + 1 < value.size()) {
      const char next = value[i + 1];
      if (next == 'n' || next == 'N') {
        out.push_back('\n');
        i++;
        continue;
      }
      if (next == '\\' || next == ',' || next == ';') {
        out.push_back(next);
        i++;
        continue;
      }
    }
    out.push_back(value[i]);
  }
  return out;
}

struct ContentLine {
  std::string name;
  std::map<std::string, std::vector<std::string>> params;
  std::string value;
};

bool parseContentLine(const std::string& line, ContentLine& out) {
  const size_t colon = line.find(':');
  if (colon == std::string::npos) {
    return false;
  }
  const std::string left = line.substr(0, colon);
  out.value = line.substr(colon + 1);

  const size_t semi = left.find(';');
  out.name = toUpper(left.substr(0, semi));

  if (semi == std::string::npos) {
    return true;
  }

  const std::string paramPart = left.substr(semi + 1);
  const auto paramTokens = split(paramPart, ';');
  for (const auto& token : paramTokens) {
    if (token.empty()) continue;
    const size_t eq = token.find('=');
    std::string key = eq == std::string::npos ? token : token.substr(0, eq);
    std::string val = eq == std::string::npos ? "" : token.substr(eq + 1);
    key = toUpper(key);
    auto values = split(val, ',');
    for (auto& v : values) {
      v = trim(v);
    }
    out.params[key] = values;
  }
  return true;
}

bool parseDateTimeValue(const std::string& value, const std::map<std::string, std::vector<std::string>>& params,
                        int timezoneOffsetMinutes, time_t& outEpoch, bool& outAllDay) {
  std::string v = value;
  if (v.size() < 8) {
    return false;
  }

  outAllDay = false;
  bool forceDate = false;

  const auto it = params.find("VALUE");
  if (it != params.end() && !it->second.empty()) {
    const std::string val = toUpper(it->second.front());
    if (val == "DATE") {
      forceDate = true;
    }
  }

  auto parseInt = [](const std::string& s, size_t pos, size_t len) -> int {
    char buf[8] = {0};
    if (pos + len > s.size() || len >= sizeof(buf)) {
      return 0;
    }
    memcpy(buf, s.c_str() + pos, len);
    buf[len] = '\0';
    return atoi(buf);
  };

  const bool isDateOnly = forceDate || v.size() == 8;
  if (isDateOnly) {
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
  if (!v.empty() && v.back() == 'Z') {
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

int parseDurationSeconds(const std::string& value) {
  if (value.empty() || value[0] != 'P') {
    return 0;
  }
  int total = 0;
  int num = 0;
  bool inTime = false;
  for (size_t i = 1; i < value.size(); i++) {
    const char c = value[i];
    if (c == 'T') {
      inTime = true;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      num = num * 10 + (c - '0');
      continue;
    }
    if (c == 'W') {
      total += num * 7 * 24 * 60 * 60;
    } else if (c == 'D') {
      total += num * 24 * 60 * 60;
    } else if (c == 'H') {
      total += num * 60 * 60;
    } else if (c == 'M') {
      if (inTime) {
        total += num * 60;
      }
    } else if (c == 'S') {
      total += num;
    }
    num = 0;
  }
  return total;
}

struct IcsEvent {
  bool hasStart = false;
  bool hasEnd = false;
  bool allDay = false;
  time_t startEpoch = 0;
  time_t endEpoch = 0;
  int durationSeconds = 0;
  std::string uid;
  std::string summary;
  std::string location;
  std::string description;
  std::string rrule;
  std::vector<time_t> rdates;
  std::vector<time_t> exdates;
  bool hasRecurrenceId = false;
  time_t recurrenceIdEpoch = 0;
  bool recurrenceIdAllDay = false;
  std::string status;
};

struct RRule {
  enum class Freq { None, Daily, Weekly, Monthly, Yearly };
  Freq freq = Freq::None;
  int interval = 1;
  int count = 0;
  bool hasUntil = false;
  time_t untilEpoch = 0;
  std::vector<int> byday;       // 0=Sun..6=Sat
  std::vector<int> bymonthday;  // 1..31
  std::vector<int> bymonth;     // 1..12
};

int weekdayIndex(const std::string& token) {
  const std::string day = toUpper(token);
  if (day.size() < 2) return -1;
  const std::string suffix = day.substr(day.size() - 2);
  if (suffix == "SU") return 0;
  if (suffix == "MO") return 1;
  if (suffix == "TU") return 2;
  if (suffix == "WE") return 3;
  if (suffix == "TH") return 4;
  if (suffix == "FR") return 5;
  if (suffix == "SA") return 6;
  return -1;
}

RRule parseRRule(const std::string& value, int timezoneOffsetMinutes) {
  RRule rule;
  const auto parts = split(value, ';');
  for (const auto& part : parts) {
    const size_t eq = part.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = toUpper(part.substr(0, eq));
    const std::string val = part.substr(eq + 1);
    if (key == "FREQ") {
      const std::string up = toUpper(val);
      if (up == "DAILY") rule.freq = RRule::Freq::Daily;
      else if (up == "WEEKLY") rule.freq = RRule::Freq::Weekly;
      else if (up == "MONTHLY") rule.freq = RRule::Freq::Monthly;
      else if (up == "YEARLY") rule.freq = RRule::Freq::Yearly;
    } else if (key == "INTERVAL") {
      rule.interval = std::max(1, atoi(val.c_str()));
    } else if (key == "COUNT") {
      rule.count = std::max(0, atoi(val.c_str()));
    } else if (key == "UNTIL") {
      time_t epoch = 0;
      bool allDay = false;
      if (parseDateTimeValue(val, {}, timezoneOffsetMinutes, epoch, allDay)) {
        rule.hasUntil = true;
        rule.untilEpoch = epoch;
      }
    } else if (key == "BYDAY") {
      const auto days = split(val, ',');
      for (const auto& d : days) {
        const int idx = weekdayIndex(d);
        if (idx >= 0) rule.byday.push_back(idx);
      }
    } else if (key == "BYMONTHDAY") {
      const auto days = split(val, ',');
      for (const auto& d : days) {
        const int day = atoi(d.c_str());
        if (day > 0 && day <= 31) rule.bymonthday.push_back(day);
      }
    } else if (key == "BYMONTH") {
      const auto months = split(val, ',');
      for (const auto& m : months) {
        const int month = atoi(m.c_str());
        if (month > 0 && month <= 12) rule.bymonth.push_back(month);
      }
    }
  }
  return rule;
}

int daysInMonth(int year, int month) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 30;
  int days = kDays[month - 1];
  if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (leap) days = 29;
  }
  return days;
}

bool matchesFilters(const std::tm& tmVal, const RRule& rule) {
  if (!rule.bymonth.empty()) {
    const int month = tmVal.tm_mon + 1;
    if (std::find(rule.bymonth.begin(), rule.bymonth.end(), month) == rule.bymonth.end()) {
      return false;
    }
  }
  if (!rule.bymonthday.empty()) {
    const int mday = tmVal.tm_mday;
    if (std::find(rule.bymonthday.begin(), rule.bymonthday.end(), mday) == rule.bymonthday.end()) {
      return false;
    }
  }
  if (!rule.byday.empty()) {
    const int wday = tmVal.tm_wday;
    if (std::find(rule.byday.begin(), rule.byday.end(), wday) == rule.byday.end()) {
      return false;
    }
  }
  return true;
}

time_t makeEpoch(int year, int month, int day, int hour, int min, int sec) {
  std::tm tmVal = {};
  tmVal.tm_year = year - 1900;
  tmVal.tm_mon = month - 1;
  tmVal.tm_mday = day;
  tmVal.tm_hour = hour;
  tmVal.tm_min = min;
  tmVal.tm_sec = sec;
  return mktime(&tmVal);
}

std::vector<time_t> expandRecurrence(const IcsEvent& ev, const RRule& rule, time_t rangeStart, time_t rangeEnd) {
  std::vector<time_t> out;
  if (!ev.hasStart || rule.freq == RRule::Freq::None) {
    return out;
  }

  const bool countLimited = rule.count > 0;
  int occurrenceCount = 0;

  const std::tm startTm = *localtime(&ev.startEpoch);
  const int baseHour = ev.allDay ? 0 : startTm.tm_hour;
  const int baseMin = ev.allDay ? 0 : startTm.tm_min;
  const int baseSec = ev.allDay ? 0 : startTm.tm_sec;

  auto pushOccurrence = [&](time_t epoch) {
    if (rule.hasUntil && epoch > rule.untilEpoch) {
      return false;
    }
    if (epoch < ev.startEpoch) {
      return true;
    }
    if (!matchesFilters(*localtime(&epoch), rule)) {
      return true;
    }
    occurrenceCount++;
    if (countLimited && occurrenceCount > rule.count) {
      return false;
    }
    if (epoch >= rangeStart && epoch <= rangeEnd) {
      out.push_back(epoch);
      if (static_cast<int>(out.size()) >= MAX_OCCURRENCES_PER_EVENT) {
        return false;
      }
    }
    return true;
  };

  if (rule.freq == RRule::Freq::Daily) {
    time_t current = ev.startEpoch;
    if (!countLimited && rangeStart > current) {
      const int daysBetween = static_cast<int>((rangeStart - current) / (24 * 60 * 60));
      const int steps = daysBetween / rule.interval;
      current += static_cast<time_t>(steps) * rule.interval * 24 * 60 * 60;
    }
    for (;;) {
      if (!pushOccurrence(current)) break;
      current += static_cast<time_t>(rule.interval) * 24 * 60 * 60;
      if (!countLimited && current > rangeEnd && !rule.hasUntil) break;
    }
  } else if (rule.freq == RRule::Freq::Weekly) {
    const int weekStartOffset = (startTm.tm_wday + 6) % 7;  // Monday=0
    time_t weekStart = ev.startEpoch - weekStartOffset * 24 * 60 * 60;

    if (!countLimited && rangeStart > weekStart) {
      const int weeksBetween = static_cast<int>((rangeStart - weekStart) / (7 * 24 * 60 * 60));
      const int steps = weeksBetween / rule.interval;
      weekStart += static_cast<time_t>(steps) * rule.interval * 7 * 24 * 60 * 60;
    }

    const std::vector<int> days = !rule.byday.empty() ? rule.byday : std::vector<int>{startTm.tm_wday};

    for (;;) {
      for (int day : days) {
        const int offset = (day + 6) % 7;  // convert to Monday=0
        time_t dayEpoch = weekStart + offset * 24 * 60 * 60;
        std::tm tmDay = *localtime(&dayEpoch);
        time_t startEpoch = makeEpoch(tmDay.tm_year + 1900, tmDay.tm_mon + 1, tmDay.tm_mday, baseHour, baseMin, baseSec);
        if (!pushOccurrence(startEpoch)) return out;
      }
      weekStart += static_cast<time_t>(rule.interval) * 7 * 24 * 60 * 60;
      if (!countLimited && weekStart > rangeEnd && !rule.hasUntil) break;
    }
  } else if (rule.freq == RRule::Freq::Monthly) {
    int year = startTm.tm_year + 1900;
    int month = startTm.tm_mon + 1;
    if (!countLimited && rangeStart > ev.startEpoch) {
      std::tm rangeTm = *localtime(&rangeStart);
      int rangeYear = rangeTm.tm_year + 1900;
      int rangeMonth = rangeTm.tm_mon + 1;
      int monthsBetween = (rangeYear - year) * 12 + (rangeMonth - month);
      if (monthsBetween > 0) {
        int steps = monthsBetween / rule.interval;
        month += steps * rule.interval;
        year += (month - 1) / 12;
        month = (month - 1) % 12 + 1;
      }
    }

    for (;;) {
      const int daysIn = daysInMonth(year, month);
      std::vector<int> days;
      if (!rule.bymonth.empty() &&
          std::find(rule.bymonth.begin(), rule.bymonth.end(), month) == rule.bymonth.end()) {
        // skip month
      } else if (!rule.bymonthday.empty()) {
        for (int d : rule.bymonthday) {
          if (d >= 1 && d <= daysIn) days.push_back(d);
        }
      } else if (!rule.byday.empty()) {
        for (int d = 1; d <= daysIn; d++) {
          time_t epoch = makeEpoch(year, month, d, baseHour, baseMin, baseSec);
          std::tm tmVal = *localtime(&epoch);
          if (std::find(rule.byday.begin(), rule.byday.end(), tmVal.tm_wday) != rule.byday.end()) {
            days.push_back(d);
          }
        }
      } else {
        days.push_back(std::min(startTm.tm_mday, daysIn));
      }

      for (int day : days) {
        time_t epoch = makeEpoch(year, month, day, baseHour, baseMin, baseSec);
        if (!pushOccurrence(epoch)) return out;
      }

      month += rule.interval;
      year += (month - 1) / 12;
      month = (month - 1) % 12 + 1;
      if (!countLimited && makeEpoch(year, month, 1, 0, 0, 0) > rangeEnd && !rule.hasUntil) break;
    }
  } else if (rule.freq == RRule::Freq::Yearly) {
    int year = startTm.tm_year + 1900;
    if (!countLimited && rangeStart > ev.startEpoch) {
      std::tm rangeTm = *localtime(&rangeStart);
      int rangeYear = rangeTm.tm_year + 1900;
      int yearsBetween = rangeYear - year;
      if (yearsBetween > 0) {
        int steps = yearsBetween / rule.interval;
        year += steps * rule.interval;
      }
    }

    for (;;) {
      std::vector<int> months = rule.bymonth.empty() ? std::vector<int>{startTm.tm_mon + 1} : rule.bymonth;
      for (int month : months) {
        int daysIn = daysInMonth(year, month);
        std::vector<int> days;
        if (!rule.bymonthday.empty()) {
          for (int d : rule.bymonthday) {
            if (d >= 1 && d <= daysIn) days.push_back(d);
          }
        } else if (!rule.byday.empty()) {
          for (int d = 1; d <= daysIn; d++) {
            time_t epoch = makeEpoch(year, month, d, baseHour, baseMin, baseSec);
            std::tm tmVal = *localtime(&epoch);
            if (std::find(rule.byday.begin(), rule.byday.end(), tmVal.tm_wday) != rule.byday.end()) {
              days.push_back(d);
            }
          }
        } else {
          days.push_back(std::min(startTm.tm_mday, daysIn));
        }

        for (int day : days) {
          time_t epoch = makeEpoch(year, month, day, baseHour, baseMin, baseSec);
          if (!pushOccurrence(epoch)) return out;
        }
      }

      year += rule.interval;
      if (!countLimited && makeEpoch(year, 1, 1, 0, 0, 0) > rangeEnd && !rule.hasUntil) break;
    }
  }

  return out;
}

int getEventDurationSeconds(const IcsEvent& ev) {
  if (ev.hasEnd) {
    const int diff = static_cast<int>(ev.endEpoch - ev.startEpoch);
    return diff > 0 ? diff : 0;
  }
  if (ev.durationSeconds > 0) {
    return ev.durationSeconds;
  }
  return ev.allDay ? 24 * 60 * 60 : 60 * 60;
}

void parseDateList(const std::string& value, const std::map<std::string, std::vector<std::string>>& params,
                   int timezoneOffsetMinutes, std::vector<time_t>& out) {
  const auto parts = split(value, ',');
  for (const auto& part : parts) {
    time_t epoch = 0;
    bool allDay = false;
    if (parseDateTimeValue(part, params, timezoneOffsetMinutes, epoch, allDay)) {
      out.push_back(epoch);
    }
  }
}
}  // namespace

bool IcsParser::parseFile(const std::string& path, const std::string& calendarId, const std::string& tag,
                          int timezoneOffsetMinutes, time_t rangeStartEpoch, time_t rangeEndEpoch,
                          std::vector<CalendarEvent>& outEvents, std::string& error) {
  FsFile file;
  if (!SdMan.openFileForRead("CAL", path.c_str(), file)) {
    error = "Failed to open .ics file";
    return false;
  }

  std::vector<IcsEvent> events;
  IcsEvent currentEvent;
  bool inEvent = false;

  std::string currentLine;
  currentLine.reserve(256);
  std::string pendingLine;
  pendingLine.reserve(256);

  auto processLine = [&](const std::string& line) {
    const std::string trimmed = trim(line);
    if (trimmed == "BEGIN:VEVENT") {
      currentEvent = IcsEvent();
      inEvent = true;
      return;
    }
    if (trimmed == "END:VEVENT") {
      if (inEvent) {
        events.push_back(currentEvent);
      }
      inEvent = false;
      return;
    }
    if (!inEvent || trimmed.empty()) {
      return;
    }

    ContentLine content;
    if (!parseContentLine(trimmed, content)) {
      return;
    }

    if (content.name == "UID") {
      currentEvent.uid = trim(content.value);
    } else if (content.name == "SUMMARY") {
      currentEvent.summary = unescapeText(content.value);
    } else if (content.name == "LOCATION") {
      currentEvent.location = unescapeText(content.value);
    } else if (content.name == "DESCRIPTION") {
      currentEvent.description = unescapeText(content.value);
    } else if (content.name == "DTSTART") {
      time_t epoch = 0;
      bool allDay = false;
      if (parseDateTimeValue(content.value, content.params, timezoneOffsetMinutes, epoch, allDay)) {
        currentEvent.hasStart = true;
        currentEvent.startEpoch = epoch;
        currentEvent.allDay = allDay;
      }
    } else if (content.name == "DTEND") {
      time_t epoch = 0;
      bool allDay = false;
      if (parseDateTimeValue(content.value, content.params, timezoneOffsetMinutes, epoch, allDay)) {
        currentEvent.hasEnd = true;
        currentEvent.endEpoch = epoch;
        currentEvent.allDay = currentEvent.allDay || allDay;
      }
    } else if (content.name == "DURATION") {
      currentEvent.durationSeconds = parseDurationSeconds(content.value);
    } else if (content.name == "RRULE") {
      currentEvent.rrule = content.value;
    } else if (content.name == "RDATE") {
      parseDateList(content.value, content.params, timezoneOffsetMinutes, currentEvent.rdates);
    } else if (content.name == "EXDATE") {
      parseDateList(content.value, content.params, timezoneOffsetMinutes, currentEvent.exdates);
    } else if (content.name == "RECURRENCE-ID") {
      time_t epoch = 0;
      bool allDay = false;
      if (parseDateTimeValue(content.value, content.params, timezoneOffsetMinutes, epoch, allDay)) {
        currentEvent.hasRecurrenceId = true;
        currentEvent.recurrenceIdEpoch = epoch;
        currentEvent.recurrenceIdAllDay = allDay;
      }
    } else if (content.name == "STATUS") {
      currentEvent.status = toUpper(trim(content.value));
    }
  };

  char buf[256];
  size_t bytesRead = 0;
  while ((bytesRead = file.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf))) > 0) {
    for (size_t i = 0; i < bytesRead; i++) {
      const char c = buf[i];
      if (c == '\n') {
        if (!currentLine.empty() && currentLine.back() == '\r') {
          currentLine.pop_back();
        }
        if (!currentLine.empty() && (currentLine[0] == ' ' || currentLine[0] == '\t')) {
          pendingLine += currentLine.substr(1);
        } else {
          if (!pendingLine.empty()) {
            processLine(pendingLine);
            pendingLine.clear();
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

  if (!currentLine.empty()) {
    if (!currentLine.empty() && currentLine.back() == '\r') {
      currentLine.pop_back();
    }
    if (!currentLine.empty() && (currentLine[0] == ' ' || currentLine[0] == '\t')) {
      pendingLine += currentLine.substr(1);
    } else {
      if (!pendingLine.empty()) {
        processLine(pendingLine);
        pendingLine.clear();
      }
      pendingLine = currentLine;
    }
  }
  if (!pendingLine.empty()) {
    processLine(pendingLine);
  }

  file.close();

  std::map<std::string, std::map<time_t, IcsEvent>> overrides;
  int uidFallback = 1;
  for (auto& ev : events) {
    if (ev.uid.empty()) {
      ev.uid = "uid-" + std::to_string(uidFallback++);
    }
    if (ev.hasRecurrenceId) {
      overrides[ev.uid][ev.recurrenceIdEpoch] = ev;
    }
  }

  for (const auto& ev : events) {
    if (!ev.hasStart) {
      continue;
    }
    if (ev.hasRecurrenceId) {
      continue;
    }
    if (ev.status == "CANCELLED") {
      continue;
    }

    const int durationSeconds = getEventDurationSeconds(ev);
    std::set<time_t> occurrenceSet;
    std::vector<time_t> occurrences;

    if (!ev.rrule.empty()) {
      const RRule rule = parseRRule(ev.rrule, timezoneOffsetMinutes);
      const auto expanded = expandRecurrence(ev, rule, rangeStartEpoch, rangeEndEpoch);
      occurrences.insert(occurrences.end(), expanded.begin(), expanded.end());
    } else {
      occurrences.push_back(ev.startEpoch);
    }

    for (time_t rdate : ev.rdates) {
      occurrences.push_back(rdate);
    }

    for (time_t occ : occurrences) {
      occurrenceSet.insert(occ);
      if (static_cast<int>(occurrenceSet.size()) >= MAX_OCCURRENCES_PER_EVENT) {
        Serial.printf("[%lu] [CAL] Occurrence cap hit for uid=%s\n", millis(), ev.uid.c_str());
        break;
      }
    }

    for (time_t ex : ev.exdates) {
      occurrenceSet.erase(ex);
    }

    for (time_t occ : occurrenceSet) {
      if (occ < rangeStartEpoch || occ > rangeEndEpoch) {
        continue;
      }

      const auto overrideIt = overrides.find(ev.uid);
      const IcsEvent* overrideEv = nullptr;
      if (overrideIt != overrides.end()) {
        const auto occIt = overrideIt->second.find(occ);
        if (occIt != overrideIt->second.end()) {
          overrideEv = &occIt->second;
        }
      }

      if (overrideEv && overrideEv->status == "CANCELLED") {
        continue;
      }

      const IcsEvent& src = overrideEv ? *overrideEv : ev;
      const int overrideDuration = getEventDurationSeconds(src);
      const time_t startEpoch = overrideEv && overrideEv->hasStart ? overrideEv->startEpoch : occ;
      const time_t endEpoch = startEpoch + (overrideDuration > 0 ? overrideDuration : durationSeconds);

      CalendarEvent out;
      out.calendarId = calendarId;
      out.tag = tag;
      out.uid = ev.uid;
      out.startEpoch = startEpoch;
      out.endEpoch = endEpoch;
      out.allDay = src.allDay;
      out.summary = src.summary;
      out.location = src.location;
      out.description = src.description;
      outEvents.push_back(out);
    }
  }

  return true;
}
