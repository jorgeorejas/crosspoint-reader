#include "TimeUtils.h"

#include <HardwareSerial.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdlib>

namespace TimeUtils {
void syncTimeWithNtp() {
  // Stop SNTP if already running (can't reconfigure while running)
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }

  // Configure SNTP
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  // Wait for time to sync (with timeout)
  int retry = 0;
  const int maxRetries = 50;  // 5 seconds max
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < maxRetries) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    retry++;
  }

  if (retry < maxRetries) {
    Serial.printf("[%lu] [TIME] NTP time synced\n", millis());
  } else {
    Serial.printf("[%lu] [TIME] NTP sync timeout, using fallback\n", millis());
  }
}

void applyTimezoneOffset(int minutesOffset) {
  // Fixed offset, no DST. POSIX TZ expects reversed sign.
  // Example: UTC-5 (EST) => minutesOffset=-300 => TZ="UTC+5".
  const int hours = minutesOffset / 60;
  const int minutes = abs(minutesOffset % 60);

  char tzBuf[32] = {0};
  if (minutes == 0) {
    snprintf(tzBuf, sizeof(tzBuf), "UTC%+d", -hours);
  } else {
    snprintf(tzBuf, sizeof(tzBuf), "UTC%+d:%02d", -hours, minutes);
  }

  setenv("TZ", tzBuf, 1);
  tzset();
  Serial.printf("[%lu] [TIME] TZ set to %s\n", millis(), tzBuf);
}

time_t utcToEpoch(const std::tm& tmUtc, int minutesOffset) {
  std::tm tmp = tmUtc;
  time_t localEpoch = mktime(&tmp);
  const int offsetSeconds = minutesOffset * 60;
  return localEpoch - offsetSeconds;
}
}  // namespace TimeUtils
