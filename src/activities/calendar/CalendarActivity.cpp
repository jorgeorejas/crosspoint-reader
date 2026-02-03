#include "CalendarActivity.h"

#include <GfxRenderer.h>
#include <WiFi.h>

#include <ctime>

#include "MappedInputManager.h"
#include "ScreenComponents.h"
#include "activities/network/WifiSelectionActivity.h"
#include "calendar/CalendarStore.h"
#include "calendar/CalendarSync.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

namespace {
constexpr int CONTENT_TOP = 70;
constexpr int LEFT_MARGIN = 20;
constexpr int RIGHT_MARGIN = 20;
constexpr int LINE_HEIGHT = 28;
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
  updateRequired = true;

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
  buildDisplayItems();
}

void CalendarActivity::buildDisplayItems() {
  displayItems.clear();

  std::string lastDate;
  for (const auto& ev : cache.events) {
    std::tm* tmVal = localtime(&ev.startEpoch);
    if (!tmVal) {
      continue;
    }
    char dateBuf[32] = {0};
    strftime(dateBuf, sizeof(dateBuf), "%a %b %d", tmVal);
    std::string dateStr = dateBuf;

    if (dateStr != lastDate) {
      DisplayItem header;
      header.isHeader = true;
      header.text = dateStr;
      displayItems.push_back(header);
      lastDate = dateStr;
    }

    char timeBuf[16] = {0};
    std::string timeStr;
    if (ev.allDay) {
      timeStr = "All day";
    } else {
      strftime(timeBuf, sizeof(timeBuf), "%H:%M", tmVal);
      timeStr = timeBuf;
    }

    DisplayItem item;
    item.isHeader = false;
    std::string summary = ev.summary.empty() ? "(No title)" : ev.summary;
    if (!ev.tag.empty()) {
      summary += " [" + ev.tag + "]";
    }
    item.text = timeStr + " " + summary;
    item.event = &ev;
    displayItems.push_back(item);
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
    performSync();
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
  buildDisplayItems();

  state = State::BROWSING;
  statusMessage = "Synced";
  updateRequired = true;
}

void CalendarActivity::loop() {
  if (state == State::WIFI_SELECTION) {
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

  if (state == State::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        performSync();
      } else {
        launchWifiSelection();
      }
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

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Calendar", true, EpdFontFamily::BOLD);

  if (state == State::BROWSING) {
    renderBrowsing();
  } else {
    renderStatus();
  }

  renderer.displayBuffer();
}

void CalendarActivity::renderStatus() const {
  const std::string message = (state == State::ERROR && !errorMessage.empty()) ? errorMessage : statusMessage;
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, message.c_str(), true);
}

void CalendarActivity::renderBrowsing() const {
  const auto pageWidth = renderer.getScreenWidth();
  const int itemCount = static_cast<int>(displayItems.size());
  int pageItems = (renderer.getScreenHeight() - CONTENT_TOP - 60) / LINE_HEIGHT;
  if (pageItems < 1) pageItems = 1;

  if (itemCount == 0) {
    renderer.drawText(UI_10_FONT_ID, LEFT_MARGIN, CONTENT_TOP, "No events found");
    renderer.drawText(UI_10_FONT_ID, LEFT_MARGIN, CONTENT_TOP + 30, statusMessage.c_str());
    return;
  }

  const int pageStartIndex = selectorIndex / pageItems * pageItems;
  const int displayCount = std::min(pageItems, itemCount - pageStartIndex);

  for (int i = 0; i < displayCount; i++) {
    const int index = pageStartIndex + i;
    const auto& item = displayItems[index];
    const int y = CONTENT_TOP + i * LINE_HEIGHT;

    if (index == selectorIndex && !item.isHeader) {
      renderer.fillRect(0, y - 2, pageWidth - RIGHT_MARGIN, LINE_HEIGHT);
    }

    if (item.isHeader) {
      renderer.drawText(UI_12_FONT_ID, LEFT_MARGIN, y, item.text.c_str());
    } else {
      auto truncated = renderer.truncatedText(UI_10_FONT_ID, item.text.c_str(), pageWidth - LEFT_MARGIN - RIGHT_MARGIN);
      renderer.drawText(UI_10_FONT_ID, LEFT_MARGIN, y, truncated.c_str(), index != selectorIndex);
    }
  }

  const int contentHeight = renderer.getScreenHeight() - CONTENT_TOP - 60;
  ScreenComponents::drawScrollIndicator(renderer, selectorIndex / pageItems + 1, (itemCount + pageItems - 1) / pageItems,
                                        CONTENT_TOP, contentHeight);

  const auto labels = mappedInput.mapLabels("Back", "Sync", "<", ">");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
