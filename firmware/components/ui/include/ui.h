#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

// LVGL screens on the ePaper panel, in the style of the Sticky OS firmware
// (https://github.com/Sticky-OS/sticky-os, MIT): a status bar on every screen,
// a home screen of app tiles, and calm black-on-white layouts.
//
// Every Show* call queues the screen and returns at once: a dedicated task owns
// LVGL and the panel, and if several requests arrive while one is drawing only
// the newest is drawn. Screens use the panel's partial refresh, with a full
// refresh mixed in now and then to clear ghosting. Safe to call from any task.
namespace ui {

// Where a screen sits in the page rotation, for the dots in the status bar.
// index 0 is home. A total of 1 or less hides them.
struct PagePos {
    int index = 0;
    int total = 0;
};

enum class TileIcon { kClaude, kCalendar, kChecklist, kGeneric };

struct Tile {
    std::string title;
    TileIcon icon = TileIcon::kGeneric;
    int badge = 0;  // a count in the corner, 0 for none
};

struct HomeInfo {
    std::vector<Tile> tiles;  // in page order, starting from page 1
    bool wifi_connected = false;
    bool has_calendar = false;
    bool has_next_event = false;
    std::string next_title;
    int64_t next_start = 0;  // Unix time
    bool next_all_day = false;
    int open_todos = -1;  // -1: no todo list
};

struct ListRow {
    std::string text;
    std::string secondary;  // optional smaller second line
    bool checkable = false;
    bool checked = false;  // ticked box and struck-through text
    bool has_detail = false;  // a chevron: tapping opens more
};

struct AgendaItem {
    std::string text;
    int64_t start = 0;  // Unix time; local midnight for an all-day item
    int64_t end = 0;
    bool all_day = false;
    bool has_detail = false;
};

esp_err_t Init();

// The time now and the user's offset from UTC, from the backend. Until this is
// called the screens show no clock.
void SetClock(int64_t unix_now, int utc_offset_s);
bool TimeKnown();

// A small moon in the status bar from the next screen drawn on: the board is
// asleep and the picture is frozen.
void SetSleeping(bool sleeping);

void ShowHome(const HomeInfo& info, PagePos page);
// An empty transcript shows how to start a conversation.
void ShowClaude(const std::string& transcript, const std::string& reply, PagePos page);
void ShowRecording();
void ShowThinking();
void ShowError(const std::string& message);
void ShowLoading();
// keep_scroll holds the list where it is, for redrawing after a row changes.
void ShowList(const std::string& title, const std::vector<ListRow>& rows, PagePos page,
              bool keep_scroll);
void ShowAgenda(const std::string& title, const std::vector<AgendaItem>& items, PagePos page,
                bool keep_scroll);
// with_back shows a back arrow and makes a tap anywhere below the bar go back.
void ShowDetail(const std::string& title, const std::string& body, PagePos page, bool with_back);

// Moves a scrolling screen by about a page: +1 towards later content, -1
// towards earlier. Ignored while a refresh is pending or running, and at either
// end, so it never costs a refresh that changes nothing.
void Scroll(int direction);

// True while a refresh is pending or running: what's on the glass may not match
// the current state, so a tap can't be trusted to mean what it looks like.
bool IsBusy();
// Waits until nothing is pending or drawing. False on timeout.
bool WaitIdle(uint32_t timeout_ms);

enum class HitKind { kNone, kStatusBar, kTile, kRow, kBody };
struct Hit {
    HitKind kind = HitKind::kNone;
    int index = -1;  // the tile's or row's index
};

// What's at this screen position. kNone while busy.
Hit HitTest(int x, int y);

}  // namespace ui
