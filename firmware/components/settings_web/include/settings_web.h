#pragma once

#include "esp_err.h"

// The settings page, served by the device on port 80 while it's awake and on
// WiFi: the old backend dashboard, now living on the device. Its own settings
// (backend address, sleep timeout, WiFi) are kept here under /device; the
// page calls the backend's API directly for everything else. Any request to
// it counts as using the device, so it stays awake while the page is open.
namespace settings_web {

esp_err_t Start();

}  // namespace settings_web
