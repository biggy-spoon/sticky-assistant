#include "settings_web.h"

#include <algorithm>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power_service.h"
#include "settings_service.h"

extern const char settings_html_start[] asm("_binary_settings_html_start");
extern const char settings_html_end[] asm("_binary_settings_html_end");

namespace settings_web {
namespace {

constexpr const char* kTag = "settings_web";
constexpr size_t kMaxBodyBytes = 1024;
constexpr const char* kConversePath = "/converse";

// backend_url is stored as the full /converse endpoint (that's what the setup
// portal has always written); the page works with the server's base address.
std::string BaseOf(const std::string& converse_url) {
    const size_t at = converse_url.rfind(kConversePath);
    return at != std::string::npos && at + strlen(kConversePath) == converse_url.size()
               ? converse_url.substr(0, at)
               : converse_url;
}

esp_err_t SendJson(httpd_req_t* req, cJSON* json) {
    char* text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(req, text != nullptr ? text : "{}");
    cJSON_free(text);
    return err;
}

std::string DeviceIp() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    if (netif == nullptr || esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        return "";
    }
    char text[16];
    snprintf(text, sizeof(text), IPSTR, IP2STR(&info.ip));
    return text;
}

esp_err_t HandlePage(httpd_req_t* req) {
    power_service::NoteActivity();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    // EMBED_TXTFILES adds a trailing NUL, which isn't part of the page.
    return httpd_resp_send(req, settings_html_start, settings_html_end - settings_html_start - 1);
}

esp_err_t HandleGetConfig(httpd_req_t* req) {
    power_service::NoteActivity();
    const settings_service::Settings settings = settings_service::Get();
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "backend", BaseOf(settings.backend_url).c_str());
    cJSON_AddStringToObject(json, "wifi_ssid", settings.wifi_ssid.c_str());
    cJSON_AddNumberToObject(json, "sleep_minutes", settings.sleep_minutes);
    cJSON_AddStringToObject(json, "ip", DeviceIp().c_str());
    return SendJson(req, json);
}

void RestartSoon(void*) {
    vTaskDelay(pdMS_TO_TICKS(1500));  // let the reply reach the browser first
    esp_restart();
}

esp_err_t HandlePostConfig(httpd_req_t* req) {
    power_service::NoteActivity();
    if (req->content_len > kMaxBodyBytes) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too large");
    }
    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int got = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (got <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete");
        }
        received += got;
    }
    cJSON* json = cJSON_ParseWithLength(body.c_str(), body.size());
    if (json == nullptr) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not JSON");
    }

    settings_service::Settings settings = settings_service::Get();
    const settings_service::Settings before = settings;
    const cJSON* field = cJSON_GetObjectItemCaseSensitive(json, "backend");
    std::string error;
    if (cJSON_IsString(field) && field->valuestring[0] != '\0') {
        std::string base = field->valuestring;
        while (!base.empty() && base.back() == '/') base.pop_back();
        if (base.rfind("http://", 0) != 0 && base.rfind("https://", 0) != 0) {
            error = "The backend address should start with http://";
        } else {
            settings.backend_url = base + kConversePath;
        }
    }
    field = cJSON_GetObjectItemCaseSensitive(json, "sleep_minutes");
    if (cJSON_IsNumber(field) && field->valueint >= 0) {
        settings.sleep_minutes = field->valueint;
    }
    field = cJSON_GetObjectItemCaseSensitive(json, "wifi_ssid");
    if (cJSON_IsString(field) && field->valuestring[0] != '\0') {
        settings.wifi_ssid = field->valuestring;
    }
    field = cJSON_GetObjectItemCaseSensitive(json, "wifi_password");
    if (cJSON_IsString(field)) {
        settings.wifi_password = field->valuestring;
    }
    cJSON_Delete(json);

    cJSON* reply = cJSON_CreateObject();
    if (!error.empty()) {
        cJSON_AddStringToObject(reply, "error", error.c_str());
        return SendJson(req, reply);
    }
    if (settings_service::Save(settings) != ESP_OK) {
        cJSON_AddStringToObject(reply, "error", "Couldn't save the settings");
        return SendJson(req, reply);
    }
    // WiFi and the backend are only read at startup.
    const bool restart = settings.wifi_ssid != before.wifi_ssid ||
                         settings.wifi_password != before.wifi_password ||
                         settings.backend_url != before.backend_url;
    cJSON_AddBoolToObject(reply, "restarting", restart);
    const esp_err_t err = SendJson(req, reply);
    if (restart) {
        ESP_LOGI(kTag, "settings changed from the web page, restarting");
        xTaskCreate(RestartSoon, "restart", 2048, nullptr, 5, nullptr);
    }
    return err;
}

// Nearby networks for the page's WiFi dropdown, strongest first, each name once.
esp_err_t HandleWifiScan(httpd_req_t* req) {
    power_service::NoteActivity();
    wifi_scan_config_t scan = {};
    scan.show_hidden = false;
    cJSON* json = cJSON_CreateObject();
    cJSON* list = cJSON_AddArrayToObject(json, "networks");
    if (esp_wifi_scan_start(&scan, true) == ESP_OK) {  // blocks for about two seconds
        uint16_t count = 20;
        wifi_ap_record_t records[20];
        if (esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
            std::vector<std::string> seen;
            for (uint16_t i = 0; i < count; ++i) {  // already sorted by signal
                const std::string ssid(reinterpret_cast<const char*>(records[i].ssid));
                if (ssid.empty() || std::find(seen.begin(), seen.end(), ssid) != seen.end()) {
                    continue;
                }
                seen.push_back(ssid);
                cJSON* network = cJSON_CreateObject();
                cJSON_AddStringToObject(network, "ssid", ssid.c_str());
                cJSON_AddNumberToObject(network, "rssi", records[i].rssi);
                cJSON_AddBoolToObject(network, "secure", records[i].authmode != WIFI_AUTH_OPEN);
                cJSON_AddItemToArray(list, network);
            }
        }
    }
    return SendJson(req, json);
}

esp_err_t HandlePing(httpd_req_t* req) {
    power_service::NoteActivity();  // the page is open: stay awake
    return SendJson(req, cJSON_CreateObject());
}

}  // namespace

esp_err_t Start() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    httpd_handle_t server = nullptr;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "server didn't start: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = HandlePage, .user_ctx = nullptr},
        {.uri = "/device/config", .method = HTTP_GET, .handler = HandleGetConfig, .user_ctx = nullptr},
        {.uri = "/device/config", .method = HTTP_POST, .handler = HandlePostConfig, .user_ctx = nullptr},
        {.uri = "/device/ping", .method = HTTP_GET, .handler = HandlePing, .user_ctx = nullptr},
        {.uri = "/device/wifi", .method = HTTP_GET, .handler = HandleWifiScan, .user_ctx = nullptr},
    };
    for (const httpd_uri_t& route : routes) {
        httpd_register_uri_handler(server, &route);
    }
    ESP_LOGI(kTag, "settings page at http://%s/", DeviceIp().c_str());
    return ESP_OK;
}

}  // namespace settings_web
