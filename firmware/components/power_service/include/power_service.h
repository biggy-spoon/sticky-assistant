#pragma once

#include <cstdint>

#include "esp_err.h"

// The board's power: the latch that keeps it on, the battery, and deep sleep.
// The sleep sequence follows the Sticky OS firmware's
// (https://github.com/Sticky-OS/sticky-os, MIT).
namespace power_service {

// Call first thing at boot. Drives the power latch high so the board stays on
// when running from the battery, and releases the pin holds deep sleep set.
void HoldPowerOn();

// Enables charging and starts reading the battery. A missing fuel gauge isn't
// an error: the status just reports the battery as unknown.
esp_err_t Init();

// True if this boot is a wake from EnterDeepSleep() rather than a power-on.
bool WokeFromSleep();

struct Status {
    bool battery_known = false;
    int percent = 0;
    bool usb_power = false;
    bool charging = false;  // current flowing into the battery
};

// Read every 30 seconds in the background, so this never blocks.
Status GetStatus();

// A button press or touch. Resets the countdown to sleep.
void NoteActivity();
int64_t IdleMs();

// Doesn't return. Waits for every button to be released, holds the power latch
// and the charger through sleep, and wakes on any button. The caller draws the
// final screen and puts the panel to sleep first. Waking restarts the firmware.
[[noreturn]] void EnterDeepSleep();

}  // namespace power_service
