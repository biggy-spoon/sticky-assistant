#pragma once

#include <string>

#include "esp_err.h"

// The device's pages and what's on them: Home (a grid of tiles), Claude (the
// last conversation), then one page per plugin view fetched from the backend's
// GET /api/device/views (see backend/plugins/base.py's DeviceView contract).
//
// Buttons and horizontal swipes change page, wrapping past either end. Vertical
// swipes scroll. Tapping a home tile opens its page, tapping the status bar goes
// home, and tapping a row toggles it if it's checkable or opens its details.
//
// Pages draw from a cached copy of the views and refresh in the background, so
// changing page doesn't wait on the network. Network calls run on this
// component's own task, so every function here is quick enough for the button
// and touch tasks.
namespace device_nav {

esp_err_t Init();

// Called once at boot, before WiFi: draws the pages saved in flash from the last
// time the backend answered, back on the page that was showing before sleep,
// so there's something real on screen while the network comes up.
void ShowCached(bool woke_from_sleep);

// Called once after the first WiFi connect attempt. With WiFi it fetches the
// views and then draws; without it, it draws home straight away. After a wake
// from sleep it returns to the page that was showing.
void Start(bool wifi_connected, bool woke_from_sleep);

// While the AI button is down, and for the whole conversation after a real
// recording, other input is ignored and background refreshes don't draw, so
// nothing replaces the conversation screens.
void PauseInput();
void ResumeInput();

// A recording is going ahead. Input stays paused until FinishConversation.
void BeginConversation();
// Shows the conversation on the Claude page and resumes input.
void FinishConversation(const std::string& transcript, const std::string& reply);
void FinishConversationWithError(const std::string& message);

// +1 for the next page, -1 for the previous.
void Navigate(int direction);

// +1 towards later content, -1 towards earlier.
void OnScroll(int direction);

// A tap at screen coordinates. Ignored while the screen is refreshing, since
// what's on the glass may not match the state yet.
void OnTap(int x, int y);

// Back: details go back to their list, and any other page goes home. False if
// already home with nothing to go back from.
bool OnBack();

// True when nothing is in progress that sleeping would cut short.
bool CanSleep();
// Stops input, remembers the page for after waking, and redraws it with the
// sleep moon. The caller waits for the UI to finish, then sleeps the board.
void PrepareForSleep();

}  // namespace device_nav
