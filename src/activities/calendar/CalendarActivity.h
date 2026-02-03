#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "activities/ActivityWithSubactivity.h"
#include "calendar/CalendarStore.h"

class CalendarActivity final : public ActivityWithSubactivity {
 public:
  explicit CalendarActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("Calendar", renderer, mappedInput), onGoHome(onGoHome) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  enum class State { LOADING_CACHE, CHECK_WIFI, WIFI_SELECTION, SYNCING, BROWSING, ERROR };

  struct DisplayItem {
    bool isHeader = false;
    std::string text;
    const CalendarEvent* event = nullptr;
  };

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;

  State state = State::LOADING_CACHE;
  std::string statusMessage;
  std::string errorMessage;

  CalendarConfig config;
  CalendarCache cache;
  std::vector<DisplayItem> displayItems;

  int selectorIndex = 0;

  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void renderBrowsing() const;
  void renderStatus() const;

  void loadCacheAndConfig();
  void buildDisplayItems();
  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void performSync();
};
