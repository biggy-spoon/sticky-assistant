#include "ui.h"

#include <sys/time.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <functional>
#include <utility>

#include "board_config.h"
#include "epaper_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "power_service.h"

// Liberation Sans (SIL Open Font License, see fonts/OFL-LiberationSans.txt),
// 1 bit per pixel, with LVGL's arrow and check symbols.
LV_FONT_DECLARE(sticky_sans_16)
LV_FONT_DECLARE(sticky_sans_18)
LV_FONT_DECLARE(sticky_sans_24)
LV_FONT_DECLARE(sticky_bold_18)
LV_FONT_DECLARE(sticky_bold_24)
LV_FONT_DECLARE(sticky_bold_34)
LV_FONT_DECLARE(sticky_bold_52)

namespace ui {
namespace {

constexpr const char* kTag = "ui";
constexpr uint32_t kTickPeriodMs = 5;
constexpr uint32_t kClockCheckMs = 5000;
constexpr uint32_t kUiTaskStackBytes = 12288;
constexpr lv_coord_t kW = BOARD_EPD_WIDTH;
constexpr lv_coord_t kH = BOARD_EPD_HEIGHT;
constexpr lv_coord_t kBarH = 44;  // the layout below follows Sticky OS's measurements
constexpr lv_coord_t kGutter = 24;
constexpr lv_coord_t kFooterH = 34;
constexpr lv_coord_t kPageOverlap = 30;  // text kept on screen when paging through it
constexpr int kPageDotCount = 3;

// Partial refreshes leave faint ghosts. Each one a person caused costs 5, a
// clock tick costs 1, and at 60 the next refresh is a full one (Sticky OS's
// budget): about every 12 screens in use, or hourly when left alone.
constexpr int kGhostBudget = 60;
constexpr int kGhostCostUser = 5;
constexpr int kGhostCostTick = 1;

const lv_font_t* const kBarFont = &sticky_sans_24;
const lv_font_t* const kBarBold = &sticky_bold_24;

struct Rect {
    lv_coord_t x1, y1, x2, y2;
};

struct Target {
    Rect rect;
    HitKind kind;
    int index;
};

// Objects that become hit targets once the layout has settled.
struct PendingTarget {
    lv_obj_t* obj;
    HitKind kind;
    int index;
};

enum class ScrollKind { kNone, kRows, kText };

lv_disp_draw_buf_t s_draw_buf;
lv_disp_drv_t s_disp_drv;
esp_timer_handle_t s_tick_timer = nullptr;
esp_timer_handle_t s_clock_timer = nullptr;

TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_lock = nullptr;  // guards the block below
std::function<void()> s_pending;
bool s_rendering = false;
bool s_tick_pending = false;
std::vector<Target> s_targets;

std::atomic<bool> s_time_known{false};
std::atomic<int> s_utc_offset{0};
std::atomic<bool> s_sleeping{false};
int64_t s_last_minute = -1;

// UI task only.
int s_ghost = 0;
bool s_cheap_refresh = false;
lv_obj_t* s_screen = nullptr;
std::vector<void*> s_screen_buffers;    // canvas buffers of the screen on the glass
std::vector<void*> s_building_buffers;  // ... and of the one being built
std::vector<PendingTarget> s_pending_targets;
int s_total_pages = 0;

struct Bar {
    lv_obj_t* time = nullptr;
    lv_obj_t* right = nullptr;  // a canvas: dots, moon, WiFi, power and battery
    PagePos page;
};
Bar s_bar;

// The home screen's clock, updated each minute while it's showing.
lv_obj_t* s_home_time = nullptr;
lv_obj_t* s_home_ampm = nullptr;
lv_obj_t* s_home_date = nullptr;

lv_obj_t* s_scroll_area = nullptr;
ScrollKind s_scroll_kind = ScrollKind::kNone;
lv_obj_t* s_footer = nullptr;
// Scroll positions kept for redrawing the same screen, by its title.
std::string s_scroll_key;
lv_coord_t s_saved_scroll = 0;

const char* const kWeekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                 "Thursday", "Friday", "Saturday"};
const char* const kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                               "July",    "August",   "September", "October", "November", "December"};

// --- text ---------------------------------------------------------------

void AppendCodepoint(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool InFont(uint32_t cp) {
    return (cp >= 0x20 && cp < 0x7F) || cp == '\n' || cp == 0xB0 || cp == 0x2022 ||
           cp == 0xF00C || cp == 0xF053 || cp == 0xF054 || cp == 0xF077 || cp == 0xF078;
}

// The fonts cover ASCII, the degree sign, the bullet and a few symbols. Anything
// else would draw as an empty box, so calendar titles and Claude's replies get
// mapped onto what the font can draw.
std::string SanitizeText(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        size_t len = 0;
        uint32_t cp = 0;
        if (lead < 0x80) {
            len = 1;
            cp = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            len = 2;
            cp = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            len = 3;
            cp = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            len = 4;
            cp = lead & 0x07;
        } else {
            ++i;
            continue;
        }
        if (i + len > text.size()) {
            break;
        }
        for (size_t j = 1; j < len; ++j) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + j]) & 0x3F);
        }
        i += len;

        switch (cp) {
            case 0x2192: case 0x2794: case 0x279C: case 0x27A1: case 0x27A4: case 0x25B6:
                out += LV_SYMBOL_RIGHT;
                break;
            case 0x2190: case 0x2B05: case 0x25C0:
                out += LV_SYMBOL_LEFT;
                break;
            case 0x2013: case 0x2014: case 0x2212:
                out += '-';
                break;
            case 0x2018: case 0x2019:
                out += '\'';
                break;
            case 0x201C: case 0x201D:
                out += '"';
                break;
            case 0x2026:
                out += "...";
                break;
            case 0x00A0:
                out += ' ';
                break;
            case '\t':
                out += ' ';
                break;
            default:
                if (InFont(cp)) {
                    AppendCodepoint(&out, cp);
                } else if (cp >= 0x80) {
                    out += '?';
                }
                break;
        }
    }
    return out;
}

// --- time ---------------------------------------------------------------

int64_t Now() { return static_cast<int64_t>(time(nullptr)); }

struct tm LocalTm(int64_t unix_time) {
    const time_t shifted = static_cast<time_t>(unix_time + s_utc_offset.load());
    struct tm out;
    gmtime_r(&shifted, &out);
    return out;
}

int64_t LocalDay(int64_t unix_time) {
    const int64_t shifted = unix_time + s_utc_offset.load();
    return shifted >= 0 ? shifted / 86400 : (shifted - 86399) / 86400;
}

// "9:30"
std::string ClockText(const struct tm& t) {
    char out[8];
    const int hour = t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12;
    snprintf(out, sizeof(out), "%d:%02d", hour, t.tm_min);
    return out;
}

// "9:30 AM"
std::string ClockText12(const struct tm& t) {
    return ClockText(t) + (t.tm_hour < 12 ? " AM" : " PM");
}

std::string ShortDate(const struct tm& t) {
    char out[24];
    snprintf(out, sizeof(out), "%.3s %.3s %d", kWeekdays[t.tm_wday], kMonths[t.tm_mon], t.tm_mday);
    return out;
}

std::string Duration(int64_t seconds) {
    const int64_t minutes = (seconds + 59) / 60;
    char out[32];
    if (minutes < 60) {
        snprintf(out, sizeof(out), "%lld min", static_cast<long long>(minutes));
    } else if (minutes % 60 == 0) {
        snprintf(out, sizeof(out), "%lld h", static_cast<long long>(minutes / 60));
    } else {
        snprintf(out, sizeof(out), "%lld h %lld min", static_cast<long long>(minutes / 60),
                 static_cast<long long>(minutes % 60));
    }
    return out;
}

// --- objects ------------------------------------------------------------

lv_obj_t* Plain(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

lv_obj_t* Label(lv_obj_t* parent, const lv_font_t* font, const std::string& text,
                lv_label_long_mode_t mode = LV_LABEL_LONG_WRAP, lv_coord_t width = 0) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_label_set_long_mode(label, mode);
    if (width > 0) {
        lv_obj_set_width(label, width);
    }
    lv_label_set_text(label, text.c_str());
    return label;
}

lv_obj_t* Rule(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    lv_obj_t* rule = Plain(parent);
    lv_obj_set_pos(rule, x, y);
    lv_obj_set_size(rule, w, h);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    return rule;
}

lv_obj_t* NewScreen() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    s_building_buffers.clear();
    s_pending_targets.clear();
    s_home_time = s_home_ampm = s_home_date = nullptr;
    s_scroll_area = nullptr;
    s_scroll_kind = ScrollKind::kNone;
    s_footer = nullptr;
    return screen;
}

// --- canvas drawing -------------------------------------------------------

// A canvas whose buffer lives in PSRAM and is freed with its screen.
lv_obj_t* Canvas(lv_obj_t* parent, lv_coord_t w, lv_coord_t h) {
    void* buffer = heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(w, h), MALLOC_CAP_SPIRAM);
    lv_obj_t* canvas = lv_canvas_create(parent);
    if (buffer == nullptr) {
        ESP_LOGE(kTag, "no memory for a %dx%d canvas", w, h);
        return canvas;
    }
    s_building_buffers.push_back(buffer);
    lv_canvas_set_buffer(canvas, buffer, w, h, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);
    return canvas;
}

lv_color_t Ink(bool ink) { return ink ? lv_color_black() : lv_color_white(); }

void FillRect(lv_obj_t* c, int x, int y, int w, int h, int radius, bool ink = true) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_color = Ink(ink);
    dsc.bg_opa = LV_OPA_COVER;
    lv_canvas_draw_rect(c, x, y, w, h, &dsc);
}

void StrokeRect(lv_obj_t* c, int x, int y, int w, int h, int radius, int width) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_color = lv_color_black();
    dsc.border_width = width;
    dsc.border_opa = LV_OPA_COVER;
    lv_canvas_draw_rect(c, x, y, w, h, &dsc);
}

void Circle(lv_obj_t* c, int cx, int cy, int r, bool ink = true) {
    FillRect(c, cx - r, cy - r, 2 * r + 1, 2 * r + 1, LV_RADIUS_CIRCLE, ink);
}

void Polygon(lv_obj_t* c, std::initializer_list<lv_point_t> points, bool ink = true) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = Ink(ink);
    dsc.bg_opa = LV_OPA_COVER;
    lv_canvas_draw_polygon(c, points.begin(), static_cast<uint32_t>(points.size()), &dsc);
}

void Line(lv_obj_t* c, int x1, int y1, int x2, int y2, int width, bool ink = true) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = Ink(ink);
    dsc.width = width;
    dsc.round_start = 1;
    dsc.round_end = 1;
    const lv_point_t points[] = {{static_cast<lv_coord_t>(x1), static_cast<lv_coord_t>(y1)},
                                 {static_cast<lv_coord_t>(x2), static_cast<lv_coord_t>(y2)}};
    lv_canvas_draw_line(c, points, 2, &dsc);
}

void Arc(lv_obj_t* c, int cx, int cy, int r, int start, int end, int width) {
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = lv_color_black();
    dsc.width = width;
    lv_canvas_draw_arc(c, cx, cy, r, start, end, &dsc);
}

lv_coord_t TextWidth(const lv_font_t* font, const char* text) {
    lv_point_t size;
    lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

void Text(lv_obj_t* c, int x, int y, const lv_font_t* font, const char* text, bool ink = true) {
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = font;
    dsc.color = Ink(ink);
    lv_canvas_draw_text(c, x, y, TextWidth(font, text) + 2, &dsc, text);
}

// --- icons (shapes and proportions from Sticky OS) ------------------------

void HouseIcon(lv_obj_t* c, int x, int y) {  // 22 x 20
    Polygon(c, {{static_cast<lv_coord_t>(x), static_cast<lv_coord_t>(y + 9)},
                {static_cast<lv_coord_t>(x + 11), static_cast<lv_coord_t>(y)},
                {static_cast<lv_coord_t>(x + 22), static_cast<lv_coord_t>(y + 9)}});
    FillRect(c, x + 3, y + 9, 16, 11, 0);
    FillRect(c, x + 9, y + 13, 5, 7, 0, false);
}

void WifiIcon(lv_obj_t* c, int x, int y, int bars) {  // 30 x 20
    for (int i = 0; i < 4; ++i) {
        const int h = 5 + i * 5;
        const int bx = x + i * 8;
        if (i < bars) {
            FillRect(c, bx, y + 20 - h, 6, h, 0);
        } else {
            StrokeRect(c, bx, y + 20 - h, 6, h, 0, 1);
        }
    }
    if (bars == 0) {
        Line(c, x - 2, y + 20, x + 30, y, 2);
    }
}

void BatteryIcon(lv_obj_t* c, int x, int y, bool known, int percent) {  // 40 x 18
    StrokeRect(c, x, y, 36, 18, 3, 2);
    FillRect(c, x + 36, y + 5, 3, 8, 0);
    if (known) {
        FillRect(c, x + 4, y + 4, LV_CLAMP(0, percent, 100) * 28 / 100, 10, 0);
    }
}

void BoltIcon(lv_obj_t* c, int x, int y) {  // 12 x 20
    Polygon(c, {{static_cast<lv_coord_t>(x + 8), static_cast<lv_coord_t>(y)},
                {static_cast<lv_coord_t>(x), static_cast<lv_coord_t>(y + 11)},
                {static_cast<lv_coord_t>(x + 6), static_cast<lv_coord_t>(y + 11)}});
    Polygon(c, {{static_cast<lv_coord_t>(x + 4), static_cast<lv_coord_t>(y + 20)},
                {static_cast<lv_coord_t>(x + 12), static_cast<lv_coord_t>(y + 9)},
                {static_cast<lv_coord_t>(x + 6), static_cast<lv_coord_t>(y + 9)}});
}

void MoonIcon(lv_obj_t* c, int x, int y) {  // about 18 x 18
    Circle(c, x + 9, y + 9, 9);
    Circle(c, x + 14, y + 5, 8, false);
}

void CalendarIcon(lv_obj_t* c, int cx, int cy, int s) {
    const int r = s / 2;
    const int t = LV_MAX(3, s / 16);
    const int x = cx - r;
    const int y = cy - r + 4;
    const int w = s;
    const int h = s - 4;
    FillRect(c, x, y, w, h, 8);
    FillRect(c, x + t, y + h / 4, w - 2 * t, h * 3 / 4 - t, 4, false);
    FillRect(c, x + w / 4 - 3, y - 6, 7, 14, 3);
    FillRect(c, x + w * 3 / 4 - 3, y - 6, 7, 14, 3);
    char day[4];
    snprintf(day, sizeof(day), "%d", s_time_known ? LocalTm(Now()).tm_mday : 1);
    const lv_font_t* font = &sticky_bold_24;
    const int text_h = lv_font_get_line_height(font);
    const int box_top = y + h / 4;
    const int box_h = h * 3 / 4 - t;
    Text(c, cx - TextWidth(font, day) / 2, box_top + (box_h - text_h) / 2 + 1, font, day);
}

void ChecklistIcon(lv_obj_t* c, int cx, int cy, int s) {
    const int r = s / 2;
    const int t = LV_MAX(3, s / 16);
    const int x = cx - r + 4;
    const int y = cy - r;
    FillRect(c, x, y, s - 8, s, 7);
    FillRect(c, x + t, y + t, s - 8 - 2 * t, s - 2 * t, 4, false);
    for (int i = 0; i < 3; ++i) {
        const int ry = y + s * (22 + i * 26) / 100;
        StrokeRect(c, x + 8, ry, 11, 11, 0, 2);
        if (i == 0) {
            Line(c, x + 10, ry + 5, x + 13, ry + 9, 2);
            Line(c, x + 13, ry + 9, x + 19, ry - 1, 2);
        }
        FillRect(c, x + 25, ry + 4, s - 8 - 34, 4, 0);
    }
}

void ClaudeIcon(lv_obj_t* c, int cx, int cy, int s) {  // a microphone: talk to it
    const int r = s / 2;
    const int t = LV_MAX(3, s / 16);
    FillRect(c, cx - 9, cy - r, 18, 32, 9);
    Arc(c, cx, cy - 2, 17, 0, 180, t);
    Line(c, cx, cy + 15, cx, cy + r - 3, t);
    Line(c, cx - 11, cy + r - 2, cx + 11, cy + r - 2, t);
}

void GenericIcon(lv_obj_t* c, int cx, int cy, int s) {
    const int r = s / 2;
    StrokeRect(c, cx - r, cy - r, s, s, 8, 3);
    for (int i = 0; i < 3; ++i) {
        FillRect(c, cx - r + 10, cy - r + 14 + i * 12, s - 20, 4, 0);
    }
}

// A count in a black pill with a white outline (Sticky OS's badge).
void Badge(lv_obj_t* c, int cx, int cy, int number) {
    char text[6];
    snprintf(text, sizeof(text), number > 99 ? "99+" : "%d", number);
    const lv_font_t* font = &sticky_bold_18;
    const int w = LV_MAX(30, TextWidth(font, text) + 16);
    FillRect(c, cx - w / 2 - 2, cy - 17, w + 4, 34, 17, false);
    FillRect(c, cx - w / 2, cy - 15, w, 30, 15);
    Text(c, cx - TextWidth(font, text) / 2, cy - lv_font_get_line_height(font) / 2, font, text,
         false);
}

// --- status bar -------------------------------------------------------------

int WifiBars() {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    if (ap.rssi > -55) return 4;
    if (ap.rssi > -65) return 3;
    if (ap.rssi > -75) return 2;
    return 1;
}

void DrawPageDots(lv_obj_t* c, int right, int y, PagePos page) {
    if (page.total <= 1) {
        return;
    }
    const int max_start = page.total > kPageDotCount ? page.total - kPageDotCount : 0;
    const int start = LV_CLAMP(0, page.index - 1, max_start);
    const int count = LV_MIN(page.total, kPageDotCount);
    const lv_font_t* arrows = &sticky_bold_18;
    const bool more_left = start > 0;
    const bool more_right = start + kPageDotCount < page.total;
    const int arrow_w = TextWidth(arrows, LV_SYMBOL_RIGHT) + 6;
    int x = right - count * 20 - (more_left ? arrow_w : 0) - (more_right ? arrow_w : 0);
    const int text_y = y - lv_font_get_line_height(arrows) / 2;
    if (more_left) {
        Text(c, x, text_y, arrows, LV_SYMBOL_LEFT);
        x += arrow_w;
    }
    for (int i = 0; i < count; ++i) {
        const int cx = x + 8;
        Circle(c, cx, y, 6);
        if (start + i != page.index) {
            Circle(c, cx, y, 4, false);
        }
        x += 20;
    }
    if (more_right) {
        Text(c, x + 2, text_y, arrows, LV_SYMBOL_RIGHT);
    }
}

constexpr lv_coord_t kBarRightW = 330;

void DrawBarRight() {
    lv_obj_t* c = s_bar.right;
    if (c == nullptr) {
        return;
    }
    lv_canvas_fill_bg(c, lv_color_white(), LV_OPA_COVER);
    const power_service::Status power = power_service::GetStatus();
    int right = kBarRightW - 16;

    BatteryIcon(c, right - 40, 12, power.battery_known, power.percent);
    right -= 48;
    char percent[8];
    if (power.battery_known) {
        snprintf(percent, sizeof(percent), "%d%%", power.percent);
    } else {
        snprintf(percent, sizeof(percent), "--");
    }
    const int percent_w = TextWidth(kBarBold, percent);
    Text(c, right - percent_w, (kBarH - 2 - lv_font_get_line_height(kBarBold)) / 2, kBarBold,
         percent);
    right -= percent_w + 12;
    if (power.usb_power) {
        BoltIcon(c, right - 12, 11);
        right -= 26;
    }
    WifiIcon(c, right - 30, 11, WifiBars());
    right -= 46;
    if (s_sleeping) {
        MoonIcon(c, right - 20, 11);
        right -= 34;
    }
    DrawPageDots(c, right - 4, 21, s_bar.page);
    lv_obj_invalidate(c);
}

void UpdateBarTime() {
    if (s_bar.time == nullptr) {
        return;
    }
    if (s_time_known) {
        lv_label_set_text(s_bar.time, ClockText(LocalTm(Now())).c_str());
        lv_obj_clear_flag(s_bar.time, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_bar.time, LV_OBJ_FLAG_HIDDEN);
    }
}

// The bar every screen has: home, the time and the screen's name on the left,
// page dots, WiFi and the battery on the right. Tapping it goes home.
void BuildBar(lv_obj_t* screen, const std::string& title, bool show_house, PagePos page) {
    lv_obj_t* bar = Plain(screen);
    lv_obj_set_size(bar, kW, kBarH);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
    Rule(bar, 0, kBarH - 2, kW, 2);

    lv_obj_t* left = Plain(bar);
    lv_obj_set_size(left, kW - kBarRightW - 8, kBarH - 2);
    lv_obj_set_pos(left, 16, 0);
    lv_obj_set_layout(left, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 16, 0);
    if (show_house) {
        lv_obj_t* house = Canvas(left, 24, 22);
        HouseIcon(house, 1, 1);
    }
    s_bar.time = Label(left, kBarBold, "", LV_LABEL_LONG_CLIP);
    UpdateBarTime();
    lv_obj_t* name = Label(left, kBarFont, SanitizeText(title), LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(name, 300, 0);

    s_bar.page = page;
    s_bar.right = Canvas(bar, kBarRightW, kBarH - 2);
    lv_obj_set_pos(s_bar.right, kW - kBarRightW, 0);
    DrawBarRight();
}

// --- scrolling ------------------------------------------------------------

// A scrolling area below the bar, with room for the footer's page count.
lv_obj_t* ScrollArea(lv_obj_t* screen, lv_coord_t x, lv_coord_t y, lv_coord_t w, ScrollKind kind) {
    lv_obj_t* area = Plain(screen);
    lv_obj_add_flag(area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(area, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(area, x, y);
    lv_obj_set_size(area, w, kH - y - kFooterH);
    lv_obj_set_layout(area, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
    s_scroll_area = area;
    s_scroll_kind = kind;
    s_footer = Label(screen, &sticky_sans_18, "", LV_LABEL_LONG_CLIP);
    lv_obj_align(s_footer, LV_ALIGN_BOTTOM_MID, 0, -6);
    return area;
}

// "2 / 3   swipe up for more" under a screen that doesn't fit.
void UpdateFooter() {
    if (s_footer == nullptr || s_scroll_area == nullptr) {
        return;
    }
    const lv_coord_t view = lv_obj_get_content_height(s_scroll_area);
    const lv_coord_t top = lv_obj_get_scroll_y(s_scroll_area);
    const lv_coord_t bottom = LV_MAX(lv_obj_get_scroll_bottom(s_scroll_area), 0);
    if (view <= 0 || top + bottom <= 0) {
        lv_label_set_text(s_footer, "");
        return;
    }
    const int pages = (top + view + bottom + view - 1) / view;
    const int current = bottom == 0 ? pages : LV_MIN(pages, top / view + 1);
    char text[48];
    if (current < pages) {
        snprintf(text, sizeof(text), "%d / %d   swipe up for more", current, pages);
    } else {
        snprintf(text, sizeof(text), "%d / %d", current, pages);
    }
    lv_label_set_text(s_footer, text);
    lv_obj_align(s_footer, LV_ALIGN_BOTTOM_MID, 0, -6);
}

// Where the area should move to. Rows snap so none is cut off at the top:
// forward to the first row not fully visible, back by about a screen. Text
// moves by a screen, keeping a line or so of overlap.
lv_coord_t NextScroll(int direction) {
    lv_obj_t* area = s_scroll_area;
    const lv_coord_t current = lv_obj_get_scroll_y(area);
    const lv_coord_t height = lv_obj_get_content_height(area);
    const lv_coord_t limit = current + LV_MAX(lv_obj_get_scroll_bottom(area), 0);

    if (s_scroll_kind == ScrollKind::kText) {
        const lv_coord_t step = height - kPageOverlap;
        return direction > 0 ? LV_MIN(current + step, limit) : LV_MAX(current - step, 0);
    }

    const uint32_t count = lv_obj_get_child_cnt(area);
    if (direction > 0) {
        for (uint32_t i = 0; i < count; ++i) {
            lv_obj_t* child = lv_obj_get_child(area, i);
            const lv_coord_t top = lv_obj_get_y(child);
            if (top + lv_obj_get_height(child) > current + height) {
                // A row taller than the screen still has to make progress.
                return LV_MIN(top > current ? top : current + height, limit);
            }
        }
        return limit;
    }
    const lv_coord_t wanted = LV_MAX(current - height, 0);
    for (uint32_t i = 0; i < count; ++i) {
        const lv_coord_t top = lv_obj_get_y(lv_obj_get_child(area, i));
        if (top >= wanted) {
            return top < current ? top : wanted;
        }
    }
    return wanted;
}

// --- hit targets ------------------------------------------------------------

void AddTarget(lv_obj_t* obj, HitKind kind, int index) { s_pending_targets.push_back({obj, kind, index}); }

// Publishes where each target is on the glass. Targets scrolled out of view
// get an empty rectangle, which never matches.
void UpdateTargets() {
    std::vector<Target> targets;
    targets.push_back({{0, 0, kW - 1, kBarH - 1}, HitKind::kStatusBar, -1});
    lv_area_t clip = {0, kBarH, kW - 1, kH - 1};
    if (s_scroll_area != nullptr) {
        lv_obj_get_coords(s_scroll_area, &clip);
    }
    for (const PendingTarget& pending : s_pending_targets) {
        lv_area_t area;
        lv_obj_get_coords(pending.obj, &area);
        const bool scrolls = s_scroll_area != nullptr && lv_obj_get_parent(pending.obj) == s_scroll_area;
        if (scrolls) {
            area.y1 = LV_MAX(area.y1, clip.y1);
            area.y2 = LV_MIN(area.y2, clip.y2);
        }
        targets.push_back({{area.x1, area.y1, area.x2, area.y2}, pending.kind, pending.index});
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_targets = std::move(targets);
    xSemaphoreGive(s_lock);
}

// --- presenting -------------------------------------------------------------

// Loads a finished screen, frees the one it replaces, and refreshes the panel.
void Present(lv_obj_t* screen) {
    lv_obj_update_layout(screen);
    UpdateFooter();
    UpdateTargets();

    lv_obj_t* old = s_screen;
    std::vector<void*> old_buffers = std::move(s_screen_buffers);
    s_screen_buffers = std::move(s_building_buffers);
    s_building_buffers.clear();
    s_screen = screen;
    lv_scr_load(screen);
    if (old != nullptr) {
        lv_obj_del(old);
    }
    for (void* buffer : old_buffers) {
        heap_caps_free(buffer);
    }
    lv_refr_now(nullptr);

    lv_mem_monitor_t mem;
    lv_mem_monitor(&mem);
    ESP_LOGI(kTag, "screen drawn: lvgl_free=%u", static_cast<unsigned>(mem.free_size));
}

void RestoreScroll(const std::string& key, bool keep_scroll) {
    if (s_scroll_area == nullptr) {
        return;
    }
    lv_obj_update_layout(lv_obj_get_screen(s_scroll_area));
    if (keep_scroll && key == s_scroll_key && s_saved_scroll > 0) {
        const lv_coord_t max = LV_MAX(lv_obj_get_scroll_bottom(s_scroll_area), 0);
        lv_obj_scroll_to_y(s_scroll_area, LV_MIN(s_saved_scroll, max), LV_ANIM_OFF);
    }
    s_scroll_key = key;
    s_saved_scroll = lv_obj_get_scroll_y(s_scroll_area);
}

// A big message in the middle of the body, for screens with nothing else.
void Message(lv_obj_t* screen, const std::string& title, const std::string& detail) {
    lv_obj_t* box = Plain(screen);
    lv_obj_set_size(box, kW - 2 * 80, LV_SIZE_CONTENT);
    lv_obj_set_layout(box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 16, 0);
    lv_obj_t* big = Label(box, &sticky_bold_34, SanitizeText(title), LV_LABEL_LONG_WRAP, kW - 160);
    lv_obj_set_style_text_align(big, LV_TEXT_ALIGN_CENTER, 0);
    if (!detail.empty()) {
        lv_obj_t* small = Label(box, &sticky_sans_24, SanitizeText(detail), LV_LABEL_LONG_WRAP, kW - 160);
        lv_obj_set_style_text_align(small, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_align(box, LV_ALIGN_CENTER, 0, kBarH / 2);
}

PagePos ClaudePage() { return {1, s_total_pages}; }

// --- home -------------------------------------------------------------------

void UpdateHomeClock() {
    if (s_home_time == nullptr) {
        return;
    }
    if (!s_time_known) {
        lv_label_set_text(s_home_time, "Hello");
        lv_label_set_text(s_home_ampm, "");
        lv_label_set_text(s_home_date, "Setting the time...");
        return;
    }
    const struct tm now = LocalTm(Now());
    lv_label_set_text(s_home_time, ClockText(now).c_str());
    lv_label_set_text(s_home_ampm, now.tm_hour < 12 ? "AM" : "PM");
    char date[48];
    snprintf(date, sizeof(date), "%s, %s %d", kWeekdays[now.tm_wday], kMonths[now.tm_mon], now.tm_mday);
    lv_label_set_text(s_home_date, date);
}

void DrawTile(lv_obj_t* screen, const Tile& tile, int slot, lv_coord_t grid_top) {
    constexpr lv_coord_t kCellW = (kW - 32) / 4;
    constexpr lv_coord_t kCellH = 150;
    constexpr int kTile = 92;
    const int col = slot % 4;
    const int row = slot / 4;

    lv_obj_t* cell = Plain(screen);
    lv_obj_set_pos(cell, 16 + col * kCellW, grid_top + row * kCellH);
    lv_obj_set_size(cell, kCellW, kCellH);
    AddTarget(cell, HitKind::kTile, slot);

    // The tile and its badge on one canvas: the badge overhangs the corner.
    lv_obj_t* art = Canvas(cell, 116, 110);
    lv_obj_align(art, LV_ALIGN_TOP_MID, 0, 2);
    StrokeRect(art, 8, 10, kTile, kTile, 22, 2);
    const int cx = 8 + kTile / 2;
    const int cy = 10 + kTile / 2;
    switch (tile.icon) {
        case TileIcon::kClaude: ClaudeIcon(art, cx, cy, 54); break;
        case TileIcon::kCalendar: CalendarIcon(art, cx, cy, 54); break;
        case TileIcon::kChecklist: ChecklistIcon(art, cx, cy, 54); break;
        case TileIcon::kGeneric: GenericIcon(art, cx, cy, 50); break;
    }
    if (tile.badge > 0) {
        Badge(art, 8 + kTile - 6, 16, tile.badge);
    }
    lv_obj_t* name = Label(cell, &sticky_sans_24, SanitizeText(tile.title), LV_LABEL_LONG_DOT, kCellW - 8);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 112);
}

void RenderHome(const HomeInfo& info, PagePos page) {
    s_total_pages = page.total;
    lv_obj_t* screen = NewScreen();
    BuildBar(screen, "Home", false, page);

    // The widget: the time and date, then what's next (Sticky OS's home).
    // A row, so AM/PM moves along when the time gets wider.
    lv_obj_t* clock_row = Plain(screen);
    lv_obj_set_pos(clock_row, kGutter, kBarH + 8);
    lv_obj_set_size(clock_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(clock_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(clock_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clock_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(clock_row, 8, 0);
    s_home_time = Label(clock_row, &sticky_bold_52, "", LV_LABEL_LONG_CLIP);
    s_home_ampm = Label(clock_row, &sticky_sans_24, "", LV_LABEL_LONG_CLIP);
    lv_obj_set_style_pad_bottom(s_home_ampm, 10, 0);
    s_home_date = Label(screen, &sticky_sans_24, "", LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(s_home_date, kGutter, kBarH + 74);
    UpdateHomeClock();

    constexpr lv_coord_t kRightX = kW / 2 + 30;
    constexpr lv_coord_t kRightW = kW - kRightX - kGutter;
    std::string when;
    std::string what;
    if (info.has_next_event) {
        const struct tm start = LocalTm(info.next_start);
        const int64_t days = LocalDay(info.next_start) - LocalDay(Now());
        if (info.next_all_day) {
            when = days <= 0 ? "Today" : days == 1 ? "Tomorrow" : ShortDate(start);
        } else if (days <= 0 && info.next_start <= Now()) {
            when = "Now";
        } else {
            when = days <= 0 ? ClockText12(start)
                   : days == 1 ? "Tomorrow, " + ClockText12(start)
                               : ShortDate(start) + ", " + ClockText12(start);
        }
        what = info.next_title;
    } else if (info.has_calendar) {
        what = "Nothing coming up";
    }
    if (!what.empty()) {
        if (!when.empty()) {
            lv_obj_t* label = Label(screen, &sticky_bold_18, "NEXT  " + when, LV_LABEL_LONG_DOT, kRightW);
            lv_obj_set_pos(label, kRightX, kBarH + 16);
        }
        lv_obj_t* title = Label(screen, &sticky_bold_24, SanitizeText(what), LV_LABEL_LONG_DOT, kRightW);
        lv_obj_set_pos(title, kRightX, kBarH + 42);
    }
    if (info.open_todos >= 0) {
        char todos[40];
        if (info.open_todos == 0) {
            snprintf(todos, sizeof(todos), "Todo list all done");
        } else {
            snprintf(todos, sizeof(todos), "%d todo%s to do", info.open_todos,
                     info.open_todos == 1 ? "" : "s");
        }
        lv_obj_t* label = Label(screen, &sticky_sans_18, todos, LV_LABEL_LONG_DOT, kRightW);
        lv_obj_set_pos(label, kRightX, kBarH + 80);
    }
    if (!info.wifi_connected) {
        lv_obj_t* label = Label(screen, &sticky_sans_18, "No WiFi: Claude and plugins can't load",
                                LV_LABEL_LONG_DOT, kRightW);
        lv_obj_set_pos(label, kRightX, kBarH + 80);
    }

    constexpr lv_coord_t kGridTop = kBarH + 120;
    Rule(screen, 16, kGridTop - 6, kW - 32, 1);
    for (size_t i = 0; i < info.tiles.size() && i < 8; ++i) {
        DrawTile(screen, info.tiles[i], static_cast<int>(i), kGridTop + 8);
    }
    Present(screen);
}

// --- Claude -------------------------------------------------------------------

void RenderClaude(const std::string& transcript, const std::string& reply, PagePos page) {
    s_total_pages = page.total;
    lv_obj_t* screen = NewScreen();
    BuildBar(screen, "Claude", true, page);
    if (transcript.empty()) {
        Message(screen, "Ask Claude anything",
                "Hold the top button while you speak, then let go.");
        Present(screen);
        return;
    }
    lv_obj_t* area = ScrollArea(screen, kGutter, kBarH + 14, kW - 2 * kGutter, ScrollKind::kText);
    lv_obj_set_style_pad_row(area, 8, 0);
    Label(area, &sticky_bold_18, "YOU SAID", LV_LABEL_LONG_CLIP);
    Label(area, &sticky_sans_24, transcript, LV_LABEL_LONG_WRAP, kW - 2 * kGutter);
    lv_obj_t* gap = Plain(area);
    lv_obj_set_size(gap, kW - 2 * kGutter, 14);
    Rule(gap, 0, 6, kW - 2 * kGutter, 1);
    Label(area, &sticky_bold_18, "CLAUDE", LV_LABEL_LONG_CLIP);
    Label(area, &sticky_sans_24, reply, LV_LABEL_LONG_WRAP, kW - 2 * kGutter);
    RestoreScroll("claude", false);
    Present(screen);
}

void RenderMessage(const std::string& bar_title, const std::string& title, const std::string& detail,
                   PagePos page) {
    lv_obj_t* screen = NewScreen();
    BuildBar(screen, bar_title, true, page);
    Message(screen, title, detail);
    Present(screen);
}

// --- lists (Sticky OS's Notes) ----------------------------------------------------

void AddListRow(lv_obj_t* area, const ListRow& row, int index) {
    lv_obj_t* obj = Plain(area);
    lv_obj_set_width(obj, lv_pct(100));
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(obj, 52, 0);
    lv_obj_set_style_pad_ver(obj, 8, 0);
    lv_obj_set_style_pad_column(obj, 16, 0);
    lv_obj_set_layout(obj, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_black(), 0);
    AddTarget(obj, HitKind::kRow, index);

    if (row.checkable) {
        lv_obj_t* box = Plain(obj);
        lv_obj_set_size(box, 28, 28);
        lv_obj_set_style_radius(box, 4, 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_border_color(box, lv_color_black(), 0);
        if (row.checked) {
            lv_obj_t* mark = Label(box, &sticky_bold_18, LV_SYMBOL_OK, LV_LABEL_LONG_CLIP);
            lv_obj_center(mark);
        }
    } else {
        lv_obj_t* dot = Plain(obj);
        lv_obj_set_size(dot, 28, 28);
        lv_obj_t* bullet = Plain(dot);
        lv_obj_set_size(bullet, 8, 8);
        lv_obj_set_style_radius(bullet, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(bullet, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bullet, lv_color_black(), 0);
        lv_obj_center(bullet);
    }

    lv_obj_t* column = Plain(obj);
    lv_obj_set_flex_grow(column, 1);
    lv_obj_set_height(column, LV_SIZE_CONTENT);
    lv_obj_set_layout(column, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, 2, 0);
    // Open items stand out in bold; finished ones go plain and struck through.
    const bool bold = row.checkable && !row.checked;
    lv_obj_t* text = Label(column, bold ? &sticky_bold_24 : &sticky_sans_24, row.text);
    lv_obj_set_width(text, lv_pct(100));
    if (row.checked) {
        lv_obj_set_style_text_decor(text, LV_TEXT_DECOR_STRIKETHROUGH, 0);
    }
    if (!row.secondary.empty()) {
        lv_obj_t* secondary = Label(column, &sticky_sans_18, row.secondary);
        lv_obj_set_width(secondary, lv_pct(100));
    }
    if (row.has_detail) {
        Label(obj, &sticky_sans_18, LV_SYMBOL_RIGHT, LV_LABEL_LONG_CLIP);
    }
}

void RenderList(const std::string& title, const std::vector<ListRow>& rows, PagePos page,
                bool keep_scroll) {
    s_total_pages = page.total;
    lv_obj_t* screen = NewScreen();
    BuildBar(screen, title, true, page);
    lv_obj_t* area = ScrollArea(screen, kGutter, kBarH + 6, kW - 2 * kGutter, ScrollKind::kRows);
    for (size_t i = 0; i < rows.size(); ++i) {
        AddListRow(area, rows[i], static_cast<int>(i));
    }
    if (rows.empty()) {
        lv_obj_t* empty = Label(area, &sticky_sans_24, "Nothing here yet.", LV_LABEL_LONG_WRAP,
                                kW - 2 * kGutter);
        lv_obj_set_style_pad_top(empty, 24, 0);
    }
    RestoreScroll("list:" + title, keep_scroll);
    Present(screen);
}

// --- agenda (Sticky OS's Calendar) ------------------------------------------------

constexpr lv_coord_t kAgendaLeftW = 290;
constexpr lv_coord_t kAgendaRightX = 330;

std::string DayHeading(int64_t day, int64_t today, const struct tm& t) {
    char out[48];
    if (day == today || day == today + 1) {
        snprintf(out, sizeof(out), "%s, %.3s %.3s %d", day == today ? "Today" : "Tomorrow",
                 kWeekdays[t.tm_wday], kMonths[t.tm_mon], t.tm_mday);
    } else {
        snprintf(out, sizeof(out), "%s, %s %d", kWeekdays[t.tm_wday], kMonths[t.tm_mon], t.tm_mday);
    }
    return out;
}

// The black card: what's on now, or what's next.
void AgendaCard(lv_obj_t* screen, const std::vector<AgendaItem>& items) {
    const int64_t now = Now();
    const AgendaItem* current = nullptr;
    const AgendaItem* next = nullptr;
    for (const AgendaItem& item : items) {
        if (!item.all_day && item.start <= now && item.end > now) {
            if (current == nullptr) current = &item;
        } else if (item.start > now || (item.all_day && LocalDay(item.start) >= LocalDay(now))) {
            if (next == nullptr) next = &item;
        }
    }

    lv_obj_t* card = Plain(screen);
    lv_obj_set_pos(card, 12, kBarH + 12);
    lv_obj_set_size(card, kAgendaLeftW, 178);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_pad_all(card, 16, 0);

    std::string tag;
    std::string corner;
    std::string title;
    std::string sub;
    if (current != nullptr) {
        tag = "NOW";
        corner = "until " + ClockText12(LocalTm(current->end));
        title = current->text;
        sub = Duration(current->end - now) + " left";
        if (next != nullptr && !next->all_day && LocalDay(next->start) == LocalDay(now)) {
            sub += ", then " + ClockText12(LocalTm(next->start));
        }
    } else if (next != nullptr) {
        const struct tm start = LocalTm(next->start);
        const int64_t days = LocalDay(next->start) - LocalDay(now);
        tag = "NEXT";
        corner = days <= 0 ? (next->all_day ? "today" : "at " + ClockText12(start))
                 : days == 1 ? "tomorrow"
                             : ShortDate(start);
        title = next->text;
        if (next->all_day) {
            sub = "All day";
        } else if (days <= 0) {
            sub = "in " + Duration(next->start - now);
        } else {
            sub = ClockText12(start) + " - " + ClockText12(LocalTm(next->end));
        }
    } else {
        tag = "NOTHING PLANNED";
        title = "A free calendar";
        sub = "Nothing in the next 30 days";
    }

    lv_obj_t* tag_label = Label(card, &sticky_bold_18, tag, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(tag_label, lv_color_white(), 0);
    lv_obj_align(tag_label, LV_ALIGN_TOP_LEFT, 0, 0);
    if (!corner.empty()) {
        lv_obj_t* corner_label = Label(card, &sticky_bold_18, corner, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(corner_label, lv_color_white(), 0);
        lv_obj_align(corner_label, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
    lv_obj_t* title_label = Label(card, &sticky_bold_34, SanitizeText(title), LV_LABEL_LONG_DOT,
                                  kAgendaLeftW - 32);
    lv_obj_set_height(title_label, 2 * lv_font_get_line_height(&sticky_bold_34));
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_obj_t* sub_label = Label(card, &sticky_sans_18, sub, LV_LABEL_LONG_DOT, kAgendaLeftW - 32);
    lv_obj_set_style_text_color(sub_label, lv_color_white(), 0);
    lv_obj_align(sub_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

// This week, Sunday first, with today circled and a dot under days with events.
void WeekStrip(lv_obj_t* screen, const std::vector<AgendaItem>& items) {
    const int64_t today = LocalDay(Now());
    const struct tm now = LocalTm(Now());
    const int64_t week_start = today - now.tm_wday;
    constexpr lv_coord_t kTop = kBarH + 204;
    constexpr lv_coord_t kColW = kAgendaLeftW / 7;
    static const char* const kLetters[] = {"S", "M", "T", "W", "T", "F", "S"};
    for (int i = 0; i < 7; ++i) {
        const int64_t day = week_start + i;
        const lv_coord_t x = 12 + i * kColW;
        lv_obj_t* letter = Label(screen, &sticky_sans_18, kLetters[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(letter, kColW);
        lv_obj_set_style_text_align(letter, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(letter, x, kTop);

        const struct tm date = LocalTm(Now() + (day - today) * 86400);
        char number[4];
        snprintf(number, sizeof(number), "%d", date.tm_mday);
        lv_obj_t* holder = Plain(screen);
        lv_obj_set_size(holder, 34, 34);
        lv_obj_set_pos(holder, x + (kColW - 34) / 2, kTop + 24);
        if (day == today) {
            lv_obj_set_style_radius(holder, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(holder, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(holder, lv_color_black(), 0);
        }
        lv_obj_t* number_label = Label(holder, &sticky_bold_18, number, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(number_label, day == today ? lv_color_white() : lv_color_black(), 0);
        lv_obj_center(number_label);

        bool busy = false;
        for (const AgendaItem& item : items) {
            busy = busy || (LocalDay(item.start) <= day && LocalDay(item.end - 1) >= day);
        }
        if (busy && day >= today) {
            lv_obj_t* dot = Plain(screen);
            lv_obj_set_size(dot, 6, 6);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
            lv_obj_set_pos(dot, x + kColW / 2 - 3, kTop + 64);
        }
    }
}

void AgendaHeading(lv_obj_t* area, const std::string& text) {
    lv_obj_t* heading = Plain(area);
    lv_obj_set_width(heading, lv_pct(100));
    lv_obj_set_height(heading, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(heading, 10, 0);
    lv_obj_set_style_pad_bottom(heading, 4, 0);
    lv_obj_set_style_border_side(heading, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(heading, 2, 0);
    lv_obj_set_style_border_color(heading, lv_color_black(), 0);
    lv_obj_t* label = Label(heading, &sticky_bold_24, text, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, lv_pct(100));
}

void AgendaRow(lv_obj_t* area, const AgendaItem& item, int index, bool is_now) {
    lv_obj_t* row = Plain(area);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 46);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    AddTarget(row, HitKind::kRow, index);

    lv_obj_t* marker = Plain(row);  // a bar beside what's on now
    lv_obj_set_size(marker, 4, 28);
    if (is_now) {
        lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(marker, lv_color_black(), 0);
    }
    lv_obj_t* when = item.all_day
                         ? Label(row, &sticky_sans_18, "All day", LV_LABEL_LONG_CLIP, 112)
                         : Label(row, &sticky_sans_24, ClockText12(LocalTm(item.start)),
                                 LV_LABEL_LONG_CLIP, 112);
    (void)when;
    lv_obj_t* title = Label(row, &sticky_bold_24, item.text, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(title, 1);
    if (item.has_detail) {
        Label(row, &sticky_sans_18, LV_SYMBOL_RIGHT, LV_LABEL_LONG_CLIP);
    }
}

void RenderAgenda(const std::string& title, const std::vector<AgendaItem>& items, PagePos page,
                  bool keep_scroll) {
    s_total_pages = page.total;
    lv_obj_t* screen = NewScreen();
    BuildBar(screen, title, true, page);
    AgendaCard(screen, items);
    WeekStrip(screen, items);
    Rule(screen, kAgendaRightX - 16, kBarH + 10, 1, kH - kBarH - 20);

    lv_obj_t* area = ScrollArea(screen, kAgendaRightX, kBarH + 4, kW - kAgendaRightX - 16,
                                ScrollKind::kRows);
    const int64_t now = Now();
    const int64_t today = LocalDay(now);
    int64_t shown_day = -1;
    bool today_shown = false;
    for (size_t i = 0; i < items.size(); ++i) {
        const AgendaItem& item = items[i];
        if (item.end <= now && !item.all_day) {
            continue;  // finished
        }
        // Something that started before today and is still on shows under today.
        const int64_t row_day = LV_MAX(LocalDay(item.start), today);
        if (row_day != shown_day) {
            if (!today_shown && row_day > today) {
                AgendaHeading(area, DayHeading(today, today, LocalTm(now)));
                lv_obj_t* none = Label(area, &sticky_sans_24, "Nothing else today", LV_LABEL_LONG_CLIP);
                lv_obj_set_style_pad_ver(none, 8, 0);
                today_shown = true;
            }
            AgendaHeading(area, DayHeading(row_day, today, LocalTm(now + (row_day - today) * 86400)));
            today_shown = today_shown || row_day == today;
            shown_day = row_day;
        }
        const bool is_now = !item.all_day && item.start <= now && item.end > now;
        AgendaRow(area, item, static_cast<int>(i), is_now);
    }
    if (shown_day < 0) {
        AgendaHeading(area, DayHeading(today, today, LocalTm(now)));
        Label(area, &sticky_sans_24, "Nothing in the next 30 days", LV_LABEL_LONG_CLIP);
    }
    RestoreScroll("agenda:" + title, keep_scroll);
    Present(screen);
}

// --- details ------------------------------------------------------------------

void RenderDetail(const std::string& title, const std::string& body, PagePos page, bool with_back) {
    s_total_pages = page.total;
    lv_obj_t* screen = NewScreen();
    BuildBar(screen, title, true, page);

    lv_obj_t* header = Plain(screen);
    lv_obj_set_pos(header, kGutter, kBarH + 10);
    lv_obj_set_size(header, kW - 2 * kGutter, 52);
    lv_obj_set_layout(header, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 12, 0);
    if (with_back) {
        Label(header, &sticky_bold_24, LV_SYMBOL_LEFT, LV_LABEL_LONG_CLIP);
    }
    lv_obj_t* heading = Label(header, &sticky_bold_34, SanitizeText(title), LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(heading, 1);
    Rule(screen, kGutter, kBarH + 64, kW - 2 * kGutter, 2);

    lv_obj_t* area = ScrollArea(screen, kGutter, kBarH + 76, kW - 2 * kGutter, ScrollKind::kText);
    Label(area, &sticky_sans_24, SanitizeText(body), LV_LABEL_LONG_WRAP, kW - 2 * kGutter);
    if (with_back) {
        lv_obj_t* body_target = Plain(screen);  // a tap anywhere below the bar goes back
        lv_obj_set_pos(body_target, 0, kBarH);
        lv_obj_set_size(body_target, kW, kH - kBarH);
        AddTarget(body_target, HitKind::kBody, -1);
    }
    RestoreScroll("detail:" + title, false);
    Present(screen);
}

// --- the UI task ----------------------------------------------------------------

void TickTimerCallback(void*) { lv_tick_inc(kTickPeriodMs); }

// Checks the minute and asks the UI task to update the clocks when it turns.
void ClockTimerCallback(void*) {
    if (!s_time_known) {
        return;
    }
    const int64_t minute = (Now() + s_utc_offset.load()) / 60;
    if (minute == s_last_minute) {
        return;
    }
    s_last_minute = minute;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tick_pending = true;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

// LVGL renders in color; each pixel's brightness is thresholded into the 1bpp
// framebuffer. full_refresh=1 means this is called once per redraw covering the
// whole screen, so the panel refresh happens at the end of each call.
void FlushCb(lv_disp_drv_t* disp_drv, const lv_area_t* area, lv_color_t* color_p) {
    for (lv_coord_t y = area->y1; y <= area->y2; ++y) {
        for (lv_coord_t x = area->x1; x <= area->x2; ++x) {
            const lv_color_t color = *color_p++;
            epaper_display::SetPixel(x, y, lv_color_brightness(color) < 128);
        }
    }
    s_ghost += s_cheap_refresh ? kGhostCostTick : kGhostCostUser;
    const bool full = s_ghost > kGhostBudget;
    const int64_t started = esp_timer_get_time();
    const esp_err_t err = epaper_display::Refresh(full);
    if (full) {
        s_ghost = 0;
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "refresh failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(kTag, "%s refresh: %lldms", full ? "full" : "partial",
                 (esp_timer_get_time() - started) / 1000);
    }
    lv_disp_flush_ready(disp_drv);
}

// Brings the clocks and status icons up to date on the screen showing.
void RenderTick() {
    if (s_screen == nullptr) {
        return;
    }
    UpdateBarTime();
    DrawBarRight();
    UpdateHomeClock();
    s_cheap_refresh = true;
    lv_refr_now(nullptr);
    s_cheap_refresh = false;
}

void UiTask(void*) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (true) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            std::function<void()> work = std::move(s_pending);
            s_pending = nullptr;
            const bool tick = !work && s_tick_pending;
            if (tick || work) {
                s_tick_pending = false;  // a full screen redraw brings the clock up to date too
            }
            s_rendering = static_cast<bool>(work) || tick;
            xSemaphoreGive(s_lock);
            if (work) {
                work();
            } else if (tick) {
                RenderTick();
            } else {
                break;
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_rendering = false;
            xSemaphoreGive(s_lock);
        }
    }
}

void Post(std::function<void()> work) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pending = std::move(work);
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

}  // namespace

esp_err_t Init() {
    lv_init();

    const size_t buf_pixels = static_cast<size_t>(kW) * kH;
    auto* draw_buf = static_cast<lv_color_t*>(
        heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM));
    if (draw_buf == nullptr) {
        ESP_LOGE(kTag, "failed to allocate %u pixel draw buffer", static_cast<unsigned>(buf_pixels));
        return ESP_ERR_NO_MEM;
    }
    lv_disp_draw_buf_init(&s_draw_buf, draw_buf, nullptr, buf_pixels);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = kW;
    s_disp_drv.ver_res = kH;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.flush_cb = &FlushCb;
    s_disp_drv.full_refresh = 1;  // the panel always gets a whole frame
    lv_disp_drv_register(&s_disp_drv);

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_timer_create_args_t tick_args = {};
    tick_args.callback = &TickTimerCallback;
    tick_args.name = "lv_tick";
    tick_args.skip_unhandled_events = true;
    esp_err_t err = esp_timer_create(&tick_args, &s_tick_timer);
    if (err == ESP_OK) err = esp_timer_start_periodic(s_tick_timer, kTickPeriodMs * 1000ULL);
    if (err != ESP_OK) return err;

    if (xTaskCreate(UiTask, "ui", kUiTaskStackBytes, nullptr, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    esp_timer_create_args_t clock_args = {};
    clock_args.callback = &ClockTimerCallback;
    clock_args.name = "ui_clock";
    clock_args.skip_unhandled_events = true;
    err = esp_timer_create(&clock_args, &s_clock_timer);
    if (err == ESP_OK) err = esp_timer_start_periodic(s_clock_timer, kClockCheckMs * 1000ULL);
    if (err != ESP_OK) return err;

    lv_mem_monitor_t mem;
    lv_mem_monitor(&mem);
    ESP_LOGI(kTag, "initialized: %dx%d, lvgl heap %u bytes free", kW, kH,
             static_cast<unsigned>(mem.free_size));
    return ESP_OK;
}

void SetClock(int64_t unix_now, int utc_offset_s) {
    struct timeval now = {};
    now.tv_sec = static_cast<time_t>(unix_now);
    settimeofday(&now, nullptr);
    s_utc_offset = utc_offset_s;
    s_last_minute = (unix_now + utc_offset_s) / 60;  // the screen being drawn is current
    s_time_known = true;
}

bool TimeKnown() { return s_time_known; }

void SetSleeping(bool sleeping) { s_sleeping = sleeping; }

void ShowHome(const HomeInfo& info, PagePos page) {
    Post([info, page] { RenderHome(info, page); });
}

void ShowClaude(const std::string& transcript, const std::string& reply, PagePos page) {
    Post([transcript = SanitizeText(transcript), reply = SanitizeText(reply), page] {
        RenderClaude(transcript, reply, page);
    });
}

void ShowRecording() {
    Post([] { RenderMessage("Claude", "Listening...", "Let go of the button when you're done.", ClaudePage()); });
}

void ShowThinking() {
    Post([] { RenderMessage("Claude", "Thinking...", "", ClaudePage()); });
}

void ShowError(const std::string& message) {
    Post([message] { RenderMessage("Claude", "Couldn't send that", message, ClaudePage()); });
}

void ShowLoading() {
    Post([] { RenderMessage("Loading", "Loading...", "", {0, 0}); });
}

void ShowList(const std::string& title, const std::vector<ListRow>& rows, PagePos page,
              bool keep_scroll) {
    std::vector<ListRow> clean = rows;
    for (ListRow& row : clean) {
        row.text = SanitizeText(row.text);
        row.secondary = SanitizeText(row.secondary);
    }
    Post([title, clean = std::move(clean), page, keep_scroll] {
        RenderList(title, clean, page, keep_scroll);
    });
}

void ShowAgenda(const std::string& title, const std::vector<AgendaItem>& items, PagePos page,
                bool keep_scroll) {
    std::vector<AgendaItem> clean = items;
    for (AgendaItem& item : clean) {
        item.text = SanitizeText(item.text);
    }
    Post([title, clean = std::move(clean), page, keep_scroll] {
        RenderAgenda(title, clean, page, keep_scroll);
    });
}

void ShowDetail(const std::string& title, const std::string& body, PagePos page, bool with_back) {
    Post([title, body, page, with_back] { RenderDetail(title, body, page, with_back); });
}

void Scroll(int direction) {
    if (IsBusy()) {
        return;
    }
    Post([direction] {
        if (s_scroll_area == nullptr) {
            return;
        }
        const lv_coord_t target = NextScroll(direction);
        if (target == lv_obj_get_scroll_y(s_scroll_area)) {
            return;  // already at this end
        }
        lv_obj_scroll_to_y(s_scroll_area, target, LV_ANIM_OFF);
        lv_obj_update_layout(s_screen);
        s_saved_scroll = target;
        UpdateFooter();
        UpdateTargets();
        lv_refr_now(nullptr);
    });
}

bool IsBusy() {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool busy = s_rendering || static_cast<bool>(s_pending);
    xSemaphoreGive(s_lock);
    return busy;
}

bool WaitIdle(uint32_t timeout_ms) {
    for (uint32_t waited = 0; waited < timeout_ms; waited += 20) {
        if (!IsBusy()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return !IsBusy();
}

Hit HitTest(int x, int y) {
    Hit hit;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_rendering && !s_pending) {
        // Later targets sit on top of earlier ones, except the bar, which wins.
        for (auto it = s_targets.rbegin(); it != s_targets.rend(); ++it) {
            const Rect& r = it->rect;
            if (x >= r.x1 && x <= r.x2 && y >= r.y1 && y <= r.y2) {
                hit = {it->kind, it->index};
                if (it->kind != HitKind::kBody) {
                    break;
                }
            }
        }
        if (y < kBarH) {
            hit = {HitKind::kStatusBar, -1};
        }
    }
    xSemaphoreGive(s_lock);
    return hit;
}

}  // namespace ui
