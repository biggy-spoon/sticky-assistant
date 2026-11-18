#include <cstdlib>

#include "assistant_service.h"
#include "board_config.h"
#include "button_service.h"
#include "captive_portal.h"
#include "device_nav.h"
#include "epaper_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "power_service.h"
#include "recording_service.h"
#include "settings_service.h"
#include "touch_service.h"
#include "ui.h"
#include "wifi_service.h"

namespace {

constexpr const char* kTag = "main";
constexpr uint32_t kMinHoldMs = 400;  // a shorter AI press is a tap, not a recording
constexpr uint32_t kAssistantTaskStackWords = 6144;
constexpr int64_t kSleepAfterIdleMs = 5 * 60 * 1000;
constexpr uint32_t kSleepCheckMs = 5000;
constexpr uint32_t kSleepDrawTimeoutMs = 10000;

TaskHandle_t s_assistant_task = nullptr;
esp_timer_handle_t s_recording_screen_timer = nullptr;

void ShowRecordingScreen(void*) { ui::ShowRecording(); }

void OnPressStart(button_service::ButtonId id, void*) {
    power_service::NoteActivity();
    if (id != button_service::ButtonId::kAi) {
        return;  // Up/Down only act on release -- see OnPressEnd
    }
    device_nav::PauseInput();
    // Capture starts now so the first words aren't lost, but the screen waits:
    // a tap shouldn't trigger a screen refresh.
    recording_service::StartCapture();
    esp_timer_start_once(s_recording_screen_timer, kMinHoldMs * 1000ULL);
}

void OnGesture(const touch_service::Gesture& gesture, void*) {
    power_service::NoteActivity();
    switch (gesture.type) {
        case touch_service::GestureType::kTap:
            device_nav::OnTap(gesture.x, gesture.y);
            break;
        case touch_service::GestureType::kSwipeLeft:
            device_nav::Navigate(1);
            break;
        case touch_service::GestureType::kSwipeRight:
            device_nav::Navigate(-1);
            break;
        case touch_service::GestureType::kSwipeUp:
            device_nav::OnScroll(1);  // the content follows the finger up
            break;
        case touch_service::GestureType::kSwipeDown:
            device_nav::OnScroll(-1);
            break;
    }
}

void OnPressEnd(button_service::ButtonId id, uint32_t held_ms, void*) {
    power_service::NoteActivity();
    if (id == button_service::ButtonId::kUp) {
        device_nav::Navigate(-1);
        return;
    }
    if (id == button_service::ButtonId::kDown) {
        device_nav::Navigate(1);
        return;
    }

    esp_timer_stop(s_recording_screen_timer);
    size_t sample_count = 0;
    recording_service::StopCapture(&sample_count);

    if (held_ms < kMinHoldMs || sample_count == 0) {
        device_nav::ResumeInput();
        device_nav::OnBack();
        return;
    }

    device_nav::BeginConversation();
    xTaskNotifyGive(s_assistant_task);
}

void AssistantTask(void*) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint8_t* wav = nullptr;
        size_t wav_len = 0;
        if (recording_service::BuildWav(&wav, &wav_len) != ESP_OK) {
            ESP_LOGW(kTag, "BuildWav failed");
            device_nav::FinishConversationWithError("Couldn't prepare that recording");
            continue;
        }

        ESP_LOGI(kTag, "sending %u byte clip to backend", static_cast<unsigned>(wav_len));
        ui::ShowThinking();

        const assistant_service::ConverseResult result = assistant_service::Converse(wav, wav_len);
        free(wav);
        power_service::NoteActivity();  // the answer counts as fresh: give time to read it

        if (result.success) {
            ESP_LOGI(kTag, "transcript=%s", result.transcript.c_str());
            ESP_LOGI(kTag, "reply=%s", result.reply.c_str());
            device_nav::FinishConversation(result.transcript, result.reply);
        } else {
            ESP_LOGW(kTag, "converse failed: http=%d error=%s", result.http_status,
                     result.error_message.c_str());
            device_nav::FinishConversationWithError(
                result.error_message.empty() ? "Unknown error" : result.error_message);
        }
    }
}

// After five minutes with no button press or touch, the current screen is drawn
// once more with a moon in the status bar and the board goes into deep sleep.
// The picture stays on the e-paper. Any button wakes it, back on the same page.
void SleepTask(void*) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kSleepCheckMs));
        if (power_service::IdleMs() < kSleepAfterIdleMs || !device_nav::CanSleep() ||
            ui::IsBusy()) {
            continue;
        }
        ESP_LOGI(kTag, "idle for %lld s, going to sleep", power_service::IdleMs() / 1000);
        device_nav::PrepareForSleep();
        vTaskDelay(pdMS_TO_TICKS(50));  // let the redraw get queued
        if (!ui::WaitIdle(kSleepDrawTimeoutMs)) {
            ESP_LOGW(kTag, "screen still drawing, sleeping anyway");
        }
        epaper_display::Sleep();
        power_service::EnterDeepSleep();
    }
}

}  // namespace

extern "C" void app_main() {
    power_service::HoldPowerOn();
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(settings_service::Init());

    if (!settings_service::IsConfigured()) {
        ESP_LOGI(kTag, "not configured yet, starting setup portal");
        ESP_ERROR_CHECK(captive_portal::Run());
        // Run() only returns on failure to start; the portal itself reboots
        // the device once setup completes. Nothing else to do here.
        return;
    }

    const bool woke = power_service::WokeFromSleep();
    if (power_service::Init() != ESP_OK) {
        ESP_LOGW(kTag, "power service failed to start: no battery status");
    }
    ESP_ERROR_CHECK(epaper_display::Init());
    ESP_ERROR_CHECK(ui::Init());
    ESP_ERROR_CHECK(recording_service::Init());
    ESP_ERROR_CHECK(button_service::Init());
    ESP_ERROR_CHECK(device_nav::Init());

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &ShowRecordingScreen;
    timer_args.name = "rec_screen";
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_recording_screen_timer));

    xTaskCreate(AssistantTask, "assistant", kAssistantTaskStackWords, nullptr, 5,
                &s_assistant_task);

    button_service::SetHandlers(&OnPressStart, &OnPressEnd, nullptr);

    touch_service::SetGestureHandler(&OnGesture, nullptr);
    err = touch_service::Init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "touch unavailable: %s", esp_err_to_name(err));
    }

    const settings_service::Settings settings = settings_service::Get();
    ESP_LOGI(kTag, "%s, connecting to WiFi...", woke ? "woke from sleep" : "powered on");
    const bool wifi_connected =
        wifi_service::ConnectStaBlocking(settings.wifi_ssid, settings.wifi_password) == ESP_OK;
    if (!wifi_connected) {
        ESP_LOGE(kTag, "WiFi connect failed; recording will work but uploads will fail");
    }
    device_nav::Start(wifi_connected, woke);
    power_service::NoteActivity();  // the five minutes count from here

    xTaskCreate(SleepTask, "sleep", 4096, nullptr, 2, nullptr);
    ESP_LOGI(kTag, "ready: hold the AI button to talk; tap tiles, swipe or use Up/Down to move");
}
