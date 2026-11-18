#include "device_nav.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "settings_service.h"
#include "ui.h"

namespace device_nav {
namespace {

constexpr const char* kTag = "device_nav";
constexpr int kFetchTimeoutMs = 15000;  // a CalDAV lookup behind this can take several seconds
constexpr int kActionTimeoutMs = 5000;
constexpr int64_t kRefreshMinIntervalUs = 30LL * 1000 * 1000;  // for page changes in quick succession
constexpr int64_t kBackgroundRefreshUs = 10LL * 60 * 1000 * 1000;
constexpr uint32_t kRedrawPeriodMs = 5 * 60 * 1000;  // keeps "now" and "next" current
constexpr uint32_t kTaskStackBytes = 6144;
constexpr size_t kMaxIdLen = 40;
constexpr size_t kQueueLength = 8;

constexpr int kHomePage = 0;
constexpr int kClaudePage = 1;
constexpr int kFirstViewPage = 2;

enum class RowAction { kNone, kTap, kChoose, kText };

struct Option {
    std::string value;
    std::string label;

    bool operator==(const Option&) const = default;
};

struct Item {
    std::string id;
    std::string text;
    std::string secondary;
    std::string detail;
    bool checkable = false;
    bool checked = false;
    int64_t start_ts = 0;
    int64_t end_ts = 0;
    bool all_day = false;
    RowAction action = RowAction::kNone;  // see DeviceViewItem.action in plugins/base.py
    std::vector<Option> options;          // kChoose
    std::string value;                    // kChoose: the one chosen; kText: the text

    bool operator==(const Item&) const = default;
};

struct View {
    std::string plugin_id;
    std::string view_type;
    std::string title;
    std::string body;
    std::vector<Item> items;

    bool operator==(const View&) const = default;
};

struct Weather {
    bool known = false;
    int temperature = 0;
    char unit = 'F';
    int code = 0;
    bool daytime = true;
    int64_t as_of = 0;

    bool operator==(const Weather&) const = default;
};

struct Clock {
    bool known = false;
    int64_t now = 0;
    int utc_offset = 0;
    bool custom_font = true;  // the Settings page's font choice
    Weather weather;
};

enum class CommandType { kRefresh, kPostAction, kRedraw };

struct Command {
    CommandType type;
    bool checked = false;
    bool tap = false;    // kPostAction for a tap row: no checked state, refresh after
    bool input = false;  // kPostAction with a picked or typed value, refresh after
    char value[96] = {};
    char plugin_id[kMaxIdLen] = {};
    char item_id[kMaxIdLen] = {};
};

// Survives deep sleep (not a power cut): where to come back to, and the last
// conversation for the Claude page.
RTC_DATA_ATTR char s_rtc_page[kMaxIdLen];
RTC_DATA_ATTR char s_rtc_transcript[512];
RTC_DATA_ATTR char s_rtc_reply[1536];

QueueHandle_t s_queue = nullptr;
TaskHandle_t s_task = nullptr;
TimerHandle_t s_redraw_timer = nullptr;

// s_state_mutex guards all of this. Buttons, touch and DeviceNavTask all change it.
SemaphoreHandle_t s_state_mutex = nullptr;
std::vector<View> s_views;
bool s_has_fetched = false;  // distinguishes "fetched, turned out empty" from "never fetched"
int s_page = kHomePage;
int s_detail_row = -1;  // row whose details are open, -1 = none
// A picker or keyboard open over the current view, by the row's id, so a
// background refresh can't swap the row under it. Empty = none.
std::string s_picker_id;
std::string s_keyboard_id;
std::string s_typed;
bool s_caps = false;
constexpr size_t kMaxTyped = 60;
std::string s_transcript;
std::string s_reply;
std::string s_resume_page;  // a page id to open once the views arrive
bool s_wifi_connected = false;
bool s_input_paused = false;
bool s_transient_shown = false;  // an error or loading screen is up
bool s_refresh_in_flight = false;
int s_pending_nav = 0;        // page change waiting on the first fetch
bool s_force_render = false;  // the next fetch has to draw, whatever it returns
int s_posts_pending = 0;
int64_t s_last_fetch_us = 0;  // 0 = the cache needs refreshing at the next chance
Weather s_weather;

void Lock() { xSemaphoreTake(s_state_mutex, portMAX_DELAY); }
void Unlock() { xSemaphoreGive(s_state_mutex); }

void CopyBounded(char* dest, size_t dest_len, const std::string& src) {
    strncpy(dest, src.c_str(), dest_len - 1);
    dest[dest_len - 1] = '\0';
}

int TotalPages() { return kFirstViewPage + static_cast<int>(s_views.size()); }

const View* CurrentView() {
    const int index = s_page - kFirstViewPage;
    return index >= 0 && index < static_cast<int>(s_views.size()) ? &s_views[index] : nullptr;
}

std::string PageId() {
    if (s_page == kHomePage) return "home";
    if (s_page == kClaudePage) return "claude";
    const View* view = CurrentView();
    return view != nullptr ? view->plugin_id : "home";
}

// Sets the page from an id saved by PageId(). Unknown ids leave it at home.
void SetPageById(const std::string& id) {
    s_page = kHomePage;
    if (id == "claude") {
        s_page = kClaudePage;
    }
    for (size_t i = 0; i < s_views.size(); ++i) {
        if (s_views[i].plugin_id == id) {
            s_page = kFirstViewPage + static_cast<int>(i);
        }
    }
}

std::string BaseUrl() {
    // settings_service's backend_url is the full /converse endpoint
    // (e.g. "http://192.168.1.50:8000/converse"), typed into the captive
    // portal; every other endpoint lives on that same host/port.
    const std::string converse_url = settings_service::Get().backend_url;
    const size_t scheme_end = converse_url.find("://");
    if (scheme_end == std::string::npos) {
        return converse_url;
    }
    const size_t host_end = converse_url.find('/', scheme_end + 3);
    return host_end == std::string::npos ? converse_url : converse_url.substr(0, host_end);
}

struct ResponseBuffer {
    std::string body;
};

esp_err_t HttpEventHandler(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data != nullptr) {
        auto* buffer = static_cast<ResponseBuffer*>(event->user_data);
        buffer->body.append(static_cast<const char*>(event->data),
                            static_cast<size_t>(event->data_len));
    }
    return ESP_OK;
}

std::string JsonString(const cJSON* object, const char* key) {
    const cJSON* field = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(field) ? field->valuestring : "";
}

int64_t JsonInt(const cJSON* object, const char* key) {
    const cJSON* field = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(field) ? static_cast<int64_t>(field->valuedouble) : 0;
}

// Fetches and parses GET /api/device/views. Only ever called from
// DeviceNavTask -- this is a blocking network call.
bool FetchViews(std::vector<View>* out_views, Clock* out_clock) {
    // The backend only offers a font choice to firmware that has one.
    std::string url = BaseUrl() + "/api/device/views";
    if (ui::HasCustomFont()) {
        url += "?fonts=custom&font_name=";
        for (const char c : std::string(ui::CustomFontName())) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                url += c;
            } else if (c == ' ') {
                url += "%20";
            }
        }
    }
    ResponseBuffer response_buffer;

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = kFetchTimeoutMs;
    config.event_handler = &HttpEventHandler;
    config.user_data = &response_buffer;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGW(kTag, "http client init failed");
        return false;
    }
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(kTag, "GET /api/device/views failed: err=%s http=%d", esp_err_to_name(err), status);
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(response_buffer.body.c_str(), response_buffer.body.size());
    if (root == nullptr) {
        ESP_LOGW(kTag, "invalid JSON from /api/device/views");
        return false;
    }

    std::vector<View> views;
    cJSON* view_json = nullptr;
    cJSON_ArrayForEach(view_json, cJSON_GetObjectItemCaseSensitive(root, "views")) {
        View view;
        view.plugin_id = JsonString(view_json, "plugin_id");
        view.view_type = JsonString(view_json, "view_type");
        view.title = JsonString(view_json, "title");
        view.body = JsonString(view_json, "body");

        cJSON* item_json = nullptr;
        cJSON_ArrayForEach(item_json, cJSON_GetObjectItemCaseSensitive(view_json, "items")) {
            Item item;
            item.id = JsonString(item_json, "id");
            item.text = JsonString(item_json, "text");
            item.secondary = JsonString(item_json, "secondary");
            item.detail = JsonString(item_json, "detail");
            const cJSON* checked = cJSON_GetObjectItemCaseSensitive(item_json, "checked");
            item.checkable = cJSON_IsBool(checked);
            item.checked = item.checkable && cJSON_IsTrue(checked);
            item.start_ts = JsonInt(item_json, "start_ts");
            item.end_ts = JsonInt(item_json, "end_ts");
            item.all_day = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item_json, "all_day"));
            const std::string action = JsonString(item_json, "action");
            item.action = action == "tap"      ? RowAction::kTap
                          : action == "choose" ? RowAction::kChoose
                          : action == "text"   ? RowAction::kText
                                               : RowAction::kNone;
            item.value = JsonString(item_json, "value");
            cJSON* option_json = nullptr;
            cJSON_ArrayForEach(option_json, cJSON_GetObjectItemCaseSensitive(item_json, "options")) {
                item.options.push_back(
                    {JsonString(option_json, "value"), JsonString(option_json, "label")});
            }
            view.items.push_back(std::move(item));
        }
        views.push_back(std::move(view));
    }

    const cJSON* clock = cJSON_GetObjectItemCaseSensitive(root, "clock");
    if (cJSON_IsObject(clock) && JsonInt(clock, "now") > 0) {
        out_clock->known = true;
        out_clock->now = JsonInt(clock, "now");
        out_clock->utc_offset = static_cast<int>(JsonInt(clock, "utc_offset"));
    }
    const cJSON* weather = cJSON_GetObjectItemCaseSensitive(root, "weather");
    if (cJSON_IsObject(weather)) {
        Weather& now = out_clock->weather;
        now.known = true;
        now.temperature = static_cast<int>(JsonInt(weather, "temp"));
        const std::string unit = JsonString(weather, "unit");
        now.unit = unit.empty() ? 'F' : unit[0];
        now.code = static_cast<int>(JsonInt(weather, "code"));
        now.daytime = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(weather, "is_day"));
        now.as_of = JsonInt(weather, "as_of");
    }
    const cJSON* device = cJSON_GetObjectItemCaseSensitive(root, "device");
    if (cJSON_IsObject(device)) {
        out_clock->custom_font = JsonString(device, "font") != "default";
    }
    cJSON_Delete(root);

    *out_views = std::move(views);
    return true;
}

// Sends the state the row should now have, rather than asking for a toggle, so
// a repeated request can't flip it back. Only ever called from DeviceNavTask.
bool PostDeviceAction(const Command& command) {
    const char* plugin_id = command.plugin_id;
    const char* item_id = command.item_id;
    cJSON* json = cJSON_CreateObject();
    if (json == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(json, "item_id", item_id);
    if (command.input) {
        cJSON_AddStringToObject(json, "value", command.value);
    } else if (!command.tap) {
        cJSON_AddBoolToObject(json, "checked", command.checked);
    }
    char* body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (body == nullptr) {
        return false;
    }

    const std::string url = BaseUrl() + "/api/plugins/" + plugin_id + "/device-action";
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kActionTimeoutMs;

    bool ok = false;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client != nullptr) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));
        const esp_err_t err = esp_http_client_perform(client);
        const int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        ok = err == ESP_OK && status >= 200 && status < 300;
        if (!ok) {
            ESP_LOGW(kTag, "POST device-action failed: plugin=%s item=%s err=%s http=%d", plugin_id,
                     item_id, esp_err_to_name(err), status);
        }
    }
    cJSON_free(body);
    return ok;
}

ui::TileIcon IconFor(const View& view) {
    if (view.view_type == "agenda") return ui::TileIcon::kCalendar;
    if (view.plugin_id == "weather") return ui::TileIcon::kWeather;
    if (view.plugin_id == "settings") return ui::TileIcon::kSettings;
    for (const Item& item : view.items) {
        if (item.checkable) return ui::TileIcon::kChecklist;
    }
    if (view.plugin_id == "todo") return ui::TileIcon::kChecklist;
    return ui::TileIcon::kGeneric;
}

int OpenCount(const View& view) {
    int open = 0;
    for (const Item& item : view.items) {
        open += item.checkable && !item.checked ? 1 : 0;
    }
    return open;
}

ui::HomeInfo BuildHome() {
    ui::HomeInfo info;
    info.wifi_connected = s_wifi_connected;
    info.has_weather = s_weather.known;
    info.temperature = s_weather.temperature;
    info.temperature_unit = s_weather.unit;
    info.weather_code = s_weather.code;
    info.daytime = s_weather.daytime;
    info.weather_as_of = s_weather.as_of;
    info.tiles.push_back({"Claude", ui::TileIcon::kClaude, 0});
    const int64_t now = static_cast<int64_t>(time(nullptr));
    for (const View& view : s_views) {
        const ui::TileIcon icon = IconFor(view);
        const int badge = icon == ui::TileIcon::kChecklist ? OpenCount(view) : 0;
        // A title like "Weather, Seattle" is too long for a tile: keep the part before the comma.
        const std::string tile_title = view.title.substr(0, view.title.find(','));
        info.tiles.push_back({tile_title, icon, badge});
        if (icon == ui::TileIcon::kChecklist && info.open_todos < 0) {
            info.open_todos = OpenCount(view);
        }
        if (view.view_type == "agenda" && !info.has_calendar) {
            info.has_calendar = true;
            for (const Item& item : view.items) {
                if (item.end_ts > now) {
                    info.has_next_event = true;
                    info.next_title = item.text;
                    info.next_start = item.start_ts;
                    info.next_all_day = item.all_day;
                    break;
                }
            }
        }
    }
    return info;
}

// Draws the current page. Caller must hold the lock, as for every *Locked
// function here.
void RenderCurrentLocked(bool keep_scroll) {
    s_transient_shown = false;
    const ui::PagePos page = {s_page, TotalPages()};
    if (s_page == kHomePage) {
        ui::ShowHome(BuildHome(), page);
        return;
    }
    if (s_page == kClaudePage) {
        ui::ShowClaude(s_transcript, s_reply, page);
        return;
    }
    const View* view = CurrentView();
    if (view == nullptr) {
        s_page = kHomePage;
        ui::ShowHome(BuildHome(), {kHomePage, TotalPages()});
        return;
    }
    if (s_detail_row >= 0 && s_detail_row < static_cast<int>(view->items.size())) {
        const Item& item = view->items[s_detail_row];
        ui::ShowDetail(item.text, item.detail, page, true);
        return;
    }
    if (!s_picker_id.empty() || !s_keyboard_id.empty()) {
        const std::string& id = s_picker_id.empty() ? s_keyboard_id : s_picker_id;
        const auto found = std::find_if(view->items.begin(), view->items.end(),
                                        [&](const Item& item) { return item.id == id; });
        if (found != view->items.end() && !s_picker_id.empty()) {
            std::vector<ui::ListRow> rows;
            for (const Option& option : found->options) {
                ui::ListRow row;
                row.text = option.label;
                row.radio = true;
                row.selected = option.value == found->value;
                rows.push_back(std::move(row));
            }
            ui::ShowList(found->text, rows, page, false);
            return;
        }
        if (found != view->items.end()) {
            ui::ShowKeyboard({found->text, s_typed, s_caps}, page);
            return;
        }
        s_picker_id.clear();  // the row's gone
        s_keyboard_id.clear();
    }
    if (view->view_type == "text") {
        ui::ShowDetail(view->title, view->body, page, false);
        return;
    }
    if (view->view_type == "agenda") {
        std::vector<ui::AgendaItem> items;
        items.reserve(view->items.size());
        for (const Item& item : view->items) {
            items.push_back({item.text, item.start_ts, item.end_ts, item.all_day, !item.detail.empty()});
        }
        ui::ShowAgenda(view->title, items, page, keep_scroll);
        return;
    }
    std::vector<ui::ListRow> rows;
    rows.reserve(view->items.size());
    for (const Item& item : view->items) {
        ui::ListRow row;
        row.text = item.text;
        row.secondary = item.secondary;
        row.checkable = item.checkable;
        row.checked = item.checked;
        row.has_detail = !item.detail.empty() || item.action != RowAction::kNone;
        rows.push_back(std::move(row));
    }
    ui::ShowList(view->title, rows, page, keep_scroll);
}

void EnqueueRefreshLocked() {
    if (s_refresh_in_flight) {
        return;  // already fetching; let that one land rather than piling up
    }
    Command command = {};
    command.type = CommandType::kRefresh;
    if (xQueueSend(s_queue, &command, 0) == pdTRUE) {
        s_refresh_in_flight = true;
    } else {
        ESP_LOGW(kTag, "queue full, refresh dropped");
    }
}

void CloseOverlaysLocked() {
    s_detail_row = -1;
    s_picker_id.clear();
    s_keyboard_id.clear();
}

void MovePageLocked(int direction) {
    const int total = TotalPages();
    s_page = (s_page + direction + total) % total;
    CloseOverlaysLocked();
}

// Sends what was picked or typed for a row, shows it at once, and goes back
// to the list. The refresh after the post shows what the plugin made of it.
void SubmitInputLocked(View& view, Item& item, const std::string& value, const std::string& label) {
    Command command = {};
    command.type = CommandType::kPostAction;
    command.input = true;
    CopyBounded(command.plugin_id, sizeof(command.plugin_id), view.plugin_id);
    CopyBounded(command.item_id, sizeof(command.item_id), item.id);
    CopyBounded(command.value, sizeof(command.value), value);
    if (xQueueSend(s_queue, &command, 0) != pdTRUE) {
        ESP_LOGW(kTag, "queue full, input dropped");
        return;
    }
    ++s_posts_pending;
    item.value = value;
    item.secondary = label;
    s_picker_id.clear();
    s_keyboard_id.clear();
}

void HandleKeyLocked(const std::string& key) {
    View* view = s_page >= kFirstViewPage ? &s_views[s_page - kFirstViewPage] : nullptr;
    auto found = view == nullptr ? std::vector<Item>::iterator()
                                 : std::find_if(view->items.begin(), view->items.end(),
                                                [](const Item& item) { return item.id == s_keyboard_id; });
    if (view == nullptr || found == view->items.end()) {
        s_keyboard_id.clear();
        return;
    }
    if (key == "shift") {
        s_caps = !s_caps;
    } else if (key == "back") {
        if (!s_typed.empty()) s_typed.pop_back();
    } else if (key == "done") {
        if (s_typed.empty()) {
            s_keyboard_id.clear();  // nothing typed: leave it as it was
        } else {
            SubmitInputLocked(*view, *found, s_typed, s_typed);
        }
        RenderCurrentLocked(true);
        return;
    } else if (s_typed.size() < kMaxTyped) {
        char c = key == "space" ? ' ' : key[0];
        // Capitals with shift, and for the first letter of the text.
        if (c >= 'a' && c <= 'z' && (s_caps || s_typed.empty())) {
            c = static_cast<char>(c - 'a' + 'A');
        }
        s_typed.push_back(c);
    }
    RenderCurrentLocked(true);
}

// Moves a toggled row to where it belongs: done rows at the bottom, in the order
// they were finished, and reopened rows just above them.
void ToggleRowLocked(View* view, int row) {
    Item item = std::move(view->items[row]);
    view->items.erase(view->items.begin() + row);
    item.checked = !item.checked;
    auto position = view->items.end();
    if (!item.checked) {
        position = std::find_if(view->items.begin(), view->items.end(),
                                [](const Item& other) { return other.checked; });
    }
    view->items.insert(position, std::move(item));
}

// Swaps in freshly fetched views, keeping the current plugin's page if it still
// exists. Returns whether anything on the current screen changed.
bool ApplyViewsLocked(std::vector<View> views) {
    const View* current = CurrentView();
    const std::string current_id = current != nullptr ? current->plugin_id : "";
    bool changed = !s_has_fetched || views.size() != s_views.size();
    if (current != nullptr) {
        auto found = std::find_if(views.begin(), views.end(),
                                  [&](const View& view) { return view.plugin_id == current_id; });
        if (found == views.end() || !(*found == *current)) {
            changed = true;
            s_detail_row = -1;  // the row it was showing may no longer exist
        }
    } else if (s_page == kHomePage) {
        changed = changed || views != s_views;  // the tiles and "next" come from every view
    }

    s_views = std::move(views);
    s_has_fetched = true;
    if (!current_id.empty()) {
        SetPageById(current_id);
    }
    return changed;
}

void HandleRefresh() {
    std::vector<View> views;
    Clock clock;
    const bool ok = FetchViews(&views, &clock);
    if (clock.known) {
        ui::SetClock(clock.now, clock.utc_offset);
    }
    const bool font_changed = ok && ui::UseCustomFont(clock.custom_font);

    Lock();
    s_refresh_in_flight = false;
    // Forced: something (boot, or Loading after a page change) waits on this
    // fetch and has to be replaced. Otherwise an error message stays up.
    const bool forced = s_force_render || font_changed;
    bool render = forced;
    s_force_render = false;
    const int pending_nav = s_pending_nav;
    s_pending_nav = 0;

    // A toggle that hasn't reached the backend yet would be undone by this
    // result, which predates it. The next page change refreshes again.
    if (ok && s_posts_pending == 0) {
        s_last_fetch_us = esp_timer_get_time();
        if (ApplyViewsLocked(std::move(views))) {
            render = true;
        }
        if (!(clock.weather == s_weather)) {
            s_weather = clock.weather;
            render = render || s_page == kHomePage;
        }
    }
    bool keep_scroll = true;
    if (!s_resume_page.empty()) {
        SetPageById(s_resume_page);
        s_resume_page.clear();
        keep_scroll = false;
        render = true;
    }
    if (pending_nav != 0) {
        MovePageLocked(pending_nav);
        keep_scroll = false;
        render = true;
    }
    if (render && !s_input_paused && (forced || !s_transient_shown)) {
        RenderCurrentLocked(keep_scroll);
    }
    Unlock();
}

void HandleRedraw();

void HandlePostAction(const Command& command) {
    const bool ok = PostDeviceAction(command);
    Lock();
    --s_posts_pending;
    if (!ok || command.tap || command.input) {
        // A failed toggle goes back to what the backend has; a tap, pick or
        // typed value shows what the plugin made of it.
        s_force_render = s_force_render || command.tap || command.input;
        EnqueueRefreshLocked();
    }
    Unlock();
}

void DeviceNavTask(void*) {
    Command command;
    while (true) {
        if (xQueueReceive(s_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (command.type) {
            case CommandType::kRefresh:
                HandleRefresh();
                break;
            case CommandType::kPostAction:
                HandlePostAction(command);
                break;
            case CommandType::kRedraw:
                HandleRedraw();
                break;
        }
    }
}

// Every few minutes: redraw home or the calendar, so "now" and "next" don't go
// stale, and fetch fresh views now and then. Runs on DeviceNavTask.
void HandleRedraw() {
    Lock();
    if (!s_input_paused && !s_transient_shown && s_detail_row < 0) {
        const View* view = CurrentView();
        if (s_page == kHomePage || (view != nullptr && view->view_type == "agenda")) {
            RenderCurrentLocked(true);
        }
        if (s_wifi_connected && esp_timer_get_time() - s_last_fetch_us >= kBackgroundRefreshUs) {
            EnqueueRefreshLocked();
        }
    }
    Unlock();
}

// The timer task's stack is small, so this only hands the work on.
void RedrawTimerCallback(TimerHandle_t) {
    Command command = {};
    command.type = CommandType::kRedraw;
    xQueueSend(s_queue, &command, 0);
}

void SaveConversation() {
    CopyBounded(s_rtc_transcript, sizeof(s_rtc_transcript), s_transcript);
    CopyBounded(s_rtc_reply, sizeof(s_rtc_reply), s_reply);
}

}  // namespace

esp_err_t Init() {
    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    s_queue = xQueueCreate(kQueueLength, sizeof(Command));
    if (s_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(DeviceNavTask, "device_nav", kTaskStackBytes, nullptr, 4, &s_task) != pdPASS) {
        s_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    s_redraw_timer = xTimerCreate("nav_redraw", pdMS_TO_TICKS(kRedrawPeriodMs), pdTRUE, nullptr,
                                  &RedrawTimerCallback);
    if (s_redraw_timer == nullptr || xTimerStart(s_redraw_timer, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag, "initialized");
    return ESP_OK;
}

void Start(bool wifi_connected, bool woke_from_sleep) {
    Lock();
    s_wifi_connected = wifi_connected;
    if (woke_from_sleep) {
        s_transcript = s_rtc_transcript;
        s_reply = s_rtc_reply;
        s_resume_page = s_rtc_page;
    } else {
        s_rtc_page[0] = s_rtc_transcript[0] = s_rtc_reply[0] = '\0';
    }
    if (wifi_connected) {
        s_force_render = true;
        EnqueueRefreshLocked();
    } else {
        SetPageById(s_resume_page);  // only home and Claude exist without views
        s_resume_page.clear();
        RenderCurrentLocked(false);
    }
    Unlock();
}

void PauseInput() {
    Lock();
    s_input_paused = true;
    s_pending_nav = 0;
    s_force_render = false;
    Unlock();
}

void ResumeInput() {
    Lock();
    s_input_paused = false;
    Unlock();
}

void BeginConversation() {
    Lock();
    s_detail_row = -1;
    Unlock();
}

void FinishConversation(const std::string& transcript, const std::string& reply) {
    Lock();
    s_input_paused = false;
    s_transcript = transcript;
    s_reply = reply;
    SaveConversation();
    s_page = kClaudePage;
    s_detail_row = -1;
    RenderCurrentLocked(false);
    s_last_fetch_us = 0;
    if (s_wifi_connected) {
        EnqueueRefreshLocked();  // Claude may have changed the calendar or todos
    }
    Unlock();
}

void FinishConversationWithError(const std::string& message) {
    Lock();
    s_input_paused = false;
    s_page = kClaudePage;
    s_detail_row = -1;
    s_transient_shown = true;
    ui::ShowError(message);
    Unlock();
}

void Navigate(int direction) {
    Lock();
    if (s_input_paused) {
        Unlock();
        return;
    }
    if (!s_has_fetched && s_wifi_connected) {
        if (s_pending_nav == 0) {
            ui::ShowLoading();
            s_transient_shown = true;
        }
        s_pending_nav = direction;
        s_force_render = true;  // Loading has to be replaced even if the fetch fails
        EnqueueRefreshLocked();
        Unlock();
        return;
    }

    MovePageLocked(direction);
    RenderCurrentLocked(false);
    if (s_wifi_connected && esp_timer_get_time() - s_last_fetch_us >= kRefreshMinIntervalUs) {
        EnqueueRefreshLocked();
    }
    Unlock();
}

void OnScroll(int direction) {
    Lock();
    if (!s_input_paused && s_page != kHomePage) {
        ui::Scroll(direction);
    }
    Unlock();
}

void OnTap(int x, int y) {
    Lock();
    if (!s_input_paused && !s_keyboard_id.empty()) {
        // Keys never move, so typing is taken even while the screen catches up.
        const int key = ui::HitTestKey(x, y);
        if (key >= 0) {
            HandleKeyLocked(ui::KeyAt(key));
            Unlock();
            return;
        }
    }
    if (s_input_paused || ui::IsBusy()) {
        Unlock();
        return;
    }
    const ui::Hit hit = ui::HitTest(x, y);
    switch (hit.kind) {
        case ui::HitKind::kStatusBar:
            if (s_page != kHomePage || s_transient_shown) {
                s_page = kHomePage;
                CloseOverlaysLocked();
                RenderCurrentLocked(false);
            }
            break;
        case ui::HitKind::kTile:
            if (hit.index >= 0 && hit.index + 1 < TotalPages()) {
                s_page = hit.index + 1;  // tile 0 is Claude, page 1
                CloseOverlaysLocked();
                RenderCurrentLocked(false);
            }
            break;
        case ui::HitKind::kBody:
            if (s_detail_row >= 0) {
                s_detail_row = -1;
                RenderCurrentLocked(true);
            }
            break;
        case ui::HitKind::kRow: {
            const int view_index = s_page - kFirstViewPage;
            if (view_index < 0 || view_index >= static_cast<int>(s_views.size()) ||
                hit.index < 0 || hit.index >= static_cast<int>(s_views[view_index].items.size())) {
                break;
            }
            View& view = s_views[view_index];
            if (!s_picker_id.empty()) {  // a pick from the open list
                auto found = std::find_if(view.items.begin(), view.items.end(),
                                          [](const Item& item) { return item.id == s_picker_id; });
                if (found != view.items.end() && hit.index < static_cast<int>(found->options.size())) {
                    const Option option = found->options[hit.index];
                    SubmitInputLocked(view, *found, option.value, option.label);
                    RenderCurrentLocked(true);
                }
                break;
            }
            const Item& item = view.items[hit.index];
            if (item.action == RowAction::kChoose && !item.options.empty()) {
                s_picker_id = item.id;
                RenderCurrentLocked(false);
            } else if (item.action == RowAction::kText) {
                s_keyboard_id = item.id;
                s_typed = item.value;
                s_caps = false;
                RenderCurrentLocked(false);
            } else if (item.action == RowAction::kTap) {
                Command command = {};
                command.type = CommandType::kPostAction;
                command.tap = true;
                CopyBounded(command.plugin_id, sizeof(command.plugin_id), view.plugin_id);
                CopyBounded(command.item_id, sizeof(command.item_id), item.id);
                if (xQueueSend(s_queue, &command, 0) == pdTRUE) {
                    ++s_posts_pending;  // the screen redraws when the refresh lands
                }
            } else if (item.checkable) {
                Command command = {};
                command.type = CommandType::kPostAction;
                command.checked = !item.checked;
                CopyBounded(command.plugin_id, sizeof(command.plugin_id), view.plugin_id);
                CopyBounded(command.item_id, sizeof(command.item_id), item.id);
                if (xQueueSend(s_queue, &command, 0) == pdTRUE) {
                    ++s_posts_pending;
                    ToggleRowLocked(&view, hit.index);
                    RenderCurrentLocked(true);
                } else {
                    ESP_LOGW(kTag, "queue full, tap dropped");
                }
            } else if (!item.detail.empty()) {
                s_detail_row = hit.index;
                RenderCurrentLocked(true);
            }
            break;
        }
        case ui::HitKind::kKey:  // handled before the busy check, above
        case ui::HitKind::kNone:
            break;
    }
    Unlock();
}

bool OnBack() {
    Lock();
    bool moved = true;
    if (!s_picker_id.empty() || !s_keyboard_id.empty()) {
        s_picker_id.clear();  // back out without changing anything
        s_keyboard_id.clear();
        RenderCurrentLocked(true);
    } else if (s_detail_row >= 0) {
        s_detail_row = -1;
        RenderCurrentLocked(true);
    } else if (s_page != kHomePage || s_transient_shown) {
        s_page = kHomePage;
        RenderCurrentLocked(false);
    } else {
        moved = false;
    }
    Unlock();
    return moved;
}

bool CanSleep() {
    Lock();
    const bool can = !s_input_paused && !s_refresh_in_flight && s_posts_pending == 0;
    Unlock();
    return can;
}

void PrepareForSleep() {
    Lock();
    s_input_paused = true;
    CopyBounded(s_rtc_page, sizeof(s_rtc_page), PageId());
    SaveConversation();
    ui::SetSleeping(true);
    RenderCurrentLocked(true);
    Unlock();
}

}  // namespace device_nav
