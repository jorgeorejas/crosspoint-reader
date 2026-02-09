#include "CalendarActivity.h"

#include <GfxRenderer.h>
#include <WiFi.h>

#include <ctime>
#include <algorithm>
#include <cctype>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "activities/network/WifiSelectionActivity.h"
#include "calendar/CalendarStore.h"
#include "calendar/CalendarSync.h"
#include "fontIds.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "util/TimeUtils.h"

namespace {
constexpr int CONTENT_TOP = 70;
constexpr int LEFT_MARGIN = 20;
constexpr int RIGHT_MARGIN = 20;
constexpr int LINE_HEIGHT = 28;
constexpr int LONG_PRESS_MS = 1000;

inline std::string trimText(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
  return s.substr(start, end - start);
}
}  // namespace

void CalendarActivity::taskTrampoline(void* param) {
  auto* self = static_cast<CalendarActivity*>(param);
  self->displayTaskLoop();
}

void CalendarActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  state = State::LOADING_CACHE;
  selectorIndex = 0;
  statusMessage = "Loading calendar...";
  errorMessage.clear();
  filterText.clear();
  updateRequired = true;
  lastAutoSyncEpoch = 0;
  confirmLongPressHandled = false;

  xTaskCreate(&CalendarActivity::taskTrampoline, "CalendarActivityTask", 4096, this, 1, &displayTaskHandle);

  loadCacheAndConfig();
  checkAndConnectWifi();
}

void CalendarActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  WiFi.mode(WIFI_OFF);
}

void CalendarActivity::loadCacheAndConfig() {
  CalendarStore::loadConfig(config);
  CalendarStore::loadCache(cache);
  TimeUtils::applyTimezoneOffset(config.timezoneOffsetMinutes);
  if (cache.generatedAtEpoch > 0) {
    lastAutoSyncEpoch = cache.generatedAtEpoch;
  }
  buildDisplayItems();
}

bool CalendarActivity::matchesFilter(const CalendarEvent& ev) const {
  if (filterText.empty()) {
    return true;
  }

  // Case-insensitive search without creating temp strings
  auto caseInsensitiveFind = [](const std::string& haystack, const std::string& needle) -> bool {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;

    for (size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
      bool match = true;
      for (size_t j = 0; j < needle.size(); ++j) {
        if (std::toupper(static_cast<unsigned char>(haystack[i + j])) !=
            std::toupper(static_cast<unsigned char>(needle[j]))) {
          match = false;
          break;
        }
      }
      if (match) return true;
    }
    return false;
  };

  return caseInsensitiveFind(ev.summary, filterText) ||
         caseInsensitiveFind(ev.location, filterText) ||
         caseInsensitiveFind(ev.tag, filterText) ||
         caseInsensitiveFind(ev.description, filterText);
}

void CalendarActivity::buildDisplayItems() {
  displayItems.clear();
  detailEvent = nullptr;

  std::string lastDate;
  lastDate.reserve(32);
  char dateBuf[32];
  char timeBuf[16];

  for (const auto& ev : cache.events) {
    if (!matchesFilter(ev)) {
      continue;
    }

    const std::tm* tmVal = localtime(&ev.startEpoch);
    if (!tmVal) {
      continue;
    }

    strftime(dateBuf, sizeof(dateBuf), "%a %b %d", tmVal);

    if (lastDate != dateBuf) {
      DisplayItem header;
      header.isHeader = true;
      header.text = dateBuf;
      displayItems.push_back(std::move(header));
      lastDate = dateBuf;
    }

    DisplayItem item;
    item.isHeader = false;
    item.event = &ev;

    // Build text efficiently
    if (ev.allDay) {
      item.text = "All day ";
    } else {
      strftime(timeBuf, sizeof(timeBuf), "%H:%M", tmVal);
      item.text = timeBuf;
      item.text += " ";
    }

    item.text += ev.summary.empty() ? "(No title)" : ev.summary;
    if (!ev.tag.empty()) {
      item.text += " [";
      item.text += ev.tag;
      item.text += "]";
    }

    displayItems.push_back(std::move(item));
  }

  if (selectorIndex >= static_cast<int>(displayItems.size())) {
    selectorIndex = 0;
  }
  if (!displayItems.empty()) {
    for (size_t i = 0; i < displayItems.size() && displayItems[selectorIndex].isHeader; i++) {
      selectorIndex = (selectorIndex + 1) % displayItems.size();
    }
  }
}

void CalendarActivity::checkAndConnectWifi() {
  state = State::CHECK_WIFI;
  statusMessage = "Checking WiFi...";
  updateRequired = true;

  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    if (config.autoSyncOnOpen) {
      performSync();
    } else {
      state = State::BROWSING;
      statusMessage = "Press Confirm to sync";
      updateRequired = true;
    }
  } else {
    state = State::BROWSING;
    statusMessage = "Press Confirm to sync";
    updateRequired = true;
  }
}

void CalendarActivity::launchWifiSelection() {
  state = State::WIFI_SELECTION;
  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput, [this](bool connected) {
    this->onWifiSelectionComplete(connected);
  }));
}

void CalendarActivity::onWifiSelectionComplete(bool connected) {
  exitActivity();

  if (!connected) {
    state = State::ERROR;
    errorMessage = "WiFi connection failed";
    updateRequired = true;
    return;
  }

  performSync();
}

void CalendarActivity::performSync() {
  state = State::SYNCING;
  statusMessage = "Syncing calendar...";
  updateRequired = true;

  CalendarSyncResult syncResult = CalendarSync::syncCalendars(config);
  if (!syncResult.ok) {
    state = State::ERROR;
    errorMessage = "Sync failed";
    updateRequired = true;
    return;
  }

  cache = syncResult.cache;
  CalendarStore::saveCache(cache);
  CalendarStore::saveConfig(config);
  lastAutoSyncEpoch = cache.generatedAtEpoch;
  buildDisplayItems();

  state = State::BROWSING;
  statusMessage = "Synced";
  updateRequired = true;
}

void CalendarActivity::loop() {
  if (subActivity) {
    ActivityWithSubactivity::loop();
    return;
  }

  if (state == State::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      checkAndConnectWifi();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == State::SYNCING || state == State::CHECK_WIFI || state == State::LOADING_CACHE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == State::DETAIL) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = State::BROWSING;
      detailEvent = nullptr;
      updateRequired = true;
    }
    return;
  }

  if (state == State::BROWSING) {
    if (config.autoSyncHourly && WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
      const time_t now = time(nullptr);
      if (now > 0 && (lastAutoSyncEpoch == 0 || now - lastAutoSyncEpoch >= config.autoSyncIntervalMinutes * 60)) {
        performSync();
        return;
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
      return;
    }

    if (mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        mappedInput.getHeldTime() >= LONG_PRESS_MS && !confirmLongPressHandled) {
      confirmLongPressHandled = true;
      state = State::FILTER_ENTRY;
      enterNewActivity(new KeyboardEntryActivity(
          renderer, mappedInput, "Search", filterText, 10, 64, false,
          [this](const std::string& text) {
            this->exitActivity();
            this->filterText = text;
            this->buildDisplayItems();
            this->confirmLongPressHandled = false;
            this->state = State::BROWSING;
            this->updateRequired = true;
          },
          [this]() {
            this->exitActivity();
            this->confirmLongPressHandled = false;
            this->state = State::BROWSING;
            this->updateRequired = true;
          }));
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      confirmLongPressHandled = false;
      if (displayItems.empty()) {
        if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
          performSync();
        } else {
          launchWifiSelection();
        }
        return;
      }
      if (displayItems[selectorIndex].isHeader) {
        if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
          performSync();
        } else {
          launchWifiSelection();
        }
        return;
      }
      detailEvent = displayItems[selectorIndex].event;
      state = State::DETAIL;
      updateRequired = true;
      return;
    }

    if (displayItems.empty()) {
      return;
    }

    const bool prevPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                             mappedInput.wasPressed(MappedInputManager::Button::Left);
    const bool nextPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                             mappedInput.wasPressed(MappedInputManager::Button::Right);

    if (prevPressed) {
      selectorIndex = (selectorIndex + displayItems.size() - 1) % displayItems.size();
      for (size_t i = 0; i < displayItems.size() && displayItems[selectorIndex].isHeader; i++) {
        selectorIndex = (selectorIndex + displayItems.size() - 1) % displayItems.size();
      }
      updateRequired = true;
    } else if (nextPressed) {
      selectorIndex = (selectorIndex + 1) % displayItems.size();
      for (size_t i = 0; i < displayItems.size() && displayItems[selectorIndex].isHeader; i++) {
        selectorIndex = (selectorIndex + 1) % displayItems.size();
      }
      updateRequired = true;
    }
  }
}

void CalendarActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void CalendarActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Calendar");

  if (state == State::BROWSING) {
    renderBrowsing();
  } else if (state == State::DETAIL) {
    renderDetail();
  } else {
    renderStatus();
  }

  renderer.displayBuffer();
}

void CalendarActivity::renderStatus() const {
  auto metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  const std::string message = (state == State::ERROR && !errorMessage.empty()) ? errorMessage : statusMessage;
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, message.c_str());

  const auto labels = mappedInput.mapLabels("Back", "Retry", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CalendarActivity::renderBrowsing() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // Filter to only event items (skip headers for GUI.drawList)
  std::vector<int> eventIndices;
  for (int i = 0; i < static_cast<int>(displayItems.size()); i++) {
    if (!displayItems[i].isHeader) {
      eventIndices.push_back(i);
    }
  }

  if (eventIndices.empty()) {
    const std::string emptyText = filterText.empty() ? "No events found" : "No matching events";
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, emptyText.c_str());
    if (!statusMessage.empty()) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 50, statusMessage.c_str());
    }
  } else {
    // Find selected event index in filtered list
    int selectedEventIndex = 0;
    for (size_t i = 0; i < eventIndices.size(); i++) {
      if (eventIndices[i] == selectorIndex) {
        selectedEventIndex = i;
        break;
      }
    }

    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, eventIndices.size(), selectedEventIndex,
        [this, &eventIndices](int index) { return displayItems[eventIndices[index]].text; },
        nullptr,  // No subtitle
        nullptr,  // No icon
        nullptr   // No value
    );
  }

  const char* confirmLabel = eventIndices.empty() ? "Sync" : "Details";
  const auto labels = mappedInput.mapLabels("Back", confirmLabel, "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CalendarActivity::renderDetail() const {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (!detailEvent) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop, "No event selected");
    const auto labels = mappedInput.mapLabels("Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }

  const int maxWidth = pageWidth - metrics.contentSidePadding * 2;
  int y = contentTop;

  auto drawWrapped = [&, maxWidth](const char* label, const std::string& value) {
    if (value.empty()) {
      return;
    }
    // Draw label + value efficiently
    std::string line;
    line.reserve(maxWidth);
    line = label;
    line += value;

    size_t pos = 0;
    while (pos < line.size()) {
      std::string segment = line.substr(pos);
      while (!segment.empty() && renderer.getTextWidth(UI_10_FONT_ID, segment.c_str()) > maxWidth) {
        size_t cut = segment.find_last_of(' ');
        if (cut == std::string::npos || cut == 0) {
          segment = renderer.truncatedText(UI_10_FONT_ID, segment.c_str(), maxWidth);
          break;
        }
        segment = segment.substr(0, cut);
      }
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, segment.c_str());
      y += LINE_HEIGHT;
      pos += segment.size();
      // Skip spaces at break point
      while (pos < line.size() && line[pos] == ' ') ++pos;
      if (segment.size() == 0 || pos >= line.size()) break;
    }
  };

  drawWrapped("Title: ", detailEvent->summary.empty() ? "(No title)" : detailEvent->summary);

  const std::tm* tmVal = localtime(&detailEvent->startEpoch);
  if (tmVal) {
    char buf[64];
    if (detailEvent->allDay) {
      strftime(buf, sizeof(buf), "%a %b %d (All day)", tmVal);
      drawWrapped("When: ", buf);
    } else {
      const std::tm* endTm = localtime(&detailEvent->endEpoch);
      strftime(buf, sizeof(buf), "%a %b %d %H:%M", tmVal);
      if (endTm) {
        char endBuf[32];
        strftime(endBuf, sizeof(endBuf), " - %H:%M", endTm);
        std::string range = buf;
        range += endBuf;
        drawWrapped("When: ", range);
      } else {
        drawWrapped("When: ", buf);
      }
    }
  }

  if (!detailEvent->tag.empty()) {
    drawWrapped("Tag: ", detailEvent->tag);
  }
  if (!detailEvent->location.empty()) {
    drawWrapped("Location: ", detailEvent->location);
  }
  if (!detailEvent->description.empty()) {
    drawWrapped("Notes: ", detailEvent->description);
  }

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
