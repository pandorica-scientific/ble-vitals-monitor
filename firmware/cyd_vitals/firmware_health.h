#pragma once

#include <Arduino.h>
#include <esp_ota_ops.h>

// Rollback protection for over-the-air updates.
//
// The bootloader boots a freshly installed image on probation. The Arduino core would mark it good
// during startup; overriding verifyRollbackLater() defers that to the sketch, which calls
// markFirmwareHealthy() once the monitor is demonstrably running, and before any deliberate reboot.
// A crash or watchdog reset while still on probation makes the bootloader fall back to the previous
// slot on the next boot.
//
// Defined here rather than in the sketch because the core declares the hook with C linkage and the
// Arduino preprocessor would generate a conflicting C++ prototype for a definition in the .ino.

extern "C" bool verifyRollbackLater() { return true; }

// "app0" or "app1": which of the two firmware slots is running.
inline const char* runningPartitionLabel() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  return running ? running->label : "?";
}

// Confirms a probationary image. True only when there was one to confirm.
inline bool markFirmwareHealthy() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running == nullptr || esp_ota_get_state_partition(running, &state) != ESP_OK) return false;
  if (state != ESP_OTA_IMG_PENDING_VERIFY) return false;
  const bool ok = esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
  Serial.printf("[ota] firmware in %s marked healthy%s\n", running->label, ok ? "" : " (failed)");
  return ok;
}
