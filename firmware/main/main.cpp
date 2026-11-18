#include <cstdlib>

#include "assistant_service.h"
#include "board_config.h"
#include "button_service.h"
#include "captive_portal.h"
#include "device_nav.h"
#include "driver/gpio.h"
#include "epaper_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "power_service.h"
#include "recording_service.h"
#include "settings_service.h"
#include "settings_web.h"
#include "touch_service.h"
#include "ui.h"
#include "wifi_service.h"

namespace {

constexpr const char* kTag = "main";
constexpr uint32_t kMinHoldMs = 400;  // a shorter AI press is a tap, not a recording
constexpr uint32_t kAssistantTaskStackWords = 6144;
constexpr uint32_t kSleepCheckMs = 5000;
constexpr uint32_t kSleepDrawTimeoutMs = 10000;
// A second short AI press within this long is a double press. A single press
// waits this long before acting, to be sure no second one is coming.
constexpr uint32_t kDoublePressWindowMs = 350;

// Work for the power task, sent as notification bits. The button and timer
// tasks have small stacks, so they hand drawing off rather than doing it.
constexpr uint32_t kActionSinglePress = 1 << 0;
constexpr uint32_t kActionSleepNow = 1 << 1;

TaskHandle_t s_assistant_task = nullptr;
TaskHandle_t s_power_task = nullptr;
esp_timer_handle_t s_recording_screen_timer = nullptr;
esp_timer_handle_t s_single_press_timer = nullptr;

void SinglePressTimeout(void*) { xTaskNotify(s_power_task, kActionSinglePress, eSetBits); }

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
        if (esp_timer_is_active(s_single_press_timer)) {
            esp_timer_stop(s_single_press_timer);
            ui::CleanScreen();  // double press: a full refresh clears any ghosts
        } else {
            esp_timer_start_once(s_single_press_timer, kDoublePressWindowMs * 1000ULL);
        }
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

// Runs the AI button's single press, and sleep. After five minutes with no button
// press or touch, or a single press on home, the current screen is drawn once
// more with a moon and an "Asleep" banner and the board goes into deep sleep.
// The picture stays on the e-paper. Any button wakes it, back on the same page.
void PowerTask(void*) {
    while (true) {
        uint32_t actions = 0;
        xTaskNotifyWait(0, UINT32_MAX, &actions, pdMS_TO_TICKS(kSleepCheckMs));
        if (actions & kActionSinglePress) {
            if (!device_nav::OnBack()) {
                actions |= kActionSleepNow;  // already home: a single press sleeps
            }
        }
        const bool asked = (actions & kActionSleepNow) != 0;
        // The idle time is a device setting (0 = never), changeable on the
        // settings page, so it's read each time.
        const int64_t sleep_after_ms =
            static_cast<int64_t>(settings_service::Get().sleep_minutes) * 60 * 1000;
        if (!asked && (sleep_after_ms <= 0 || power_service::IdleMs() < sleep_after_ms)) {
            continue;
        }
        if (!device_nav::CanSleep() || (!asked && ui::IsBusy())) {
            continue;
        }
        ui::WaitIdle(kSleepDrawTimeoutMs);  // let a screen already drawing finish
        ESP_LOGI(kTag, "going to sleep: %s",
                 asked ? "asked to" : "idle");
        device_nav::PrepareForSleep();
        vTaskDelay(pdMS_TO_TICKS(50));  // let the redraw get queued
        if (!ui::WaitIdle(kSleepDrawTimeoutMs)) {
            ESP_LOGW(kTag, "screen still drawing, sleeping anyway");
        }
        epaper_display::Sleep();
        power_service::EnterDeepSleep();
    }
}

// Up and Down held together for two seconds at start-up reopen setup, for a new
// network or password. The buttons pull their pins low when pressed.
bool SetupRequested() {
    gpio_config_t buttons = {};
    buttons.pin_bit_mask = (1ULL << BOARD_BUTTON_UP_PIN) | (1ULL << BOARD_BUTTON_DOWN_PIN);
    buttons.mode = GPIO_MODE_INPUT;
    buttons.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&buttons);
    vTaskDelay(pdMS_TO_TICKS(5));
    for (int i = 0; i < 40; ++i) {
        if (gpio_get_level(BOARD_BUTTON_UP_PIN) != 0 || gpio_get_level(BOARD_BUTTON_DOWN_PIN) != 0) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

// While the setup network is up, the screen says what to do with it.
void ShowSetupScreen(bool first_time) {
    if (epaper_display::Init() != ESP_OK || ui::Init() != ESP_OK) {
        return;  // setup still works from a phone, just without instructions
    }
    ui::ShowDetail(first_time ? "Welcome to Sticky Assistant" : "WiFi setup",
                   "1. On your phone or computer, join the WiFi network\n"
                   "     StickyAssistant-Setup\n"
                   "2. A setup page opens. If it doesn't, browse to http://192.168.4.1\n"
                   "3. Enter your WiFi network, its password, and your backend's address, "
                   "such as http://192.168.1.50:8000/converse\n\n"
                   "The backend is a small server on a computer at home that does the speech "
                   "recognition and talks to the AI. To run it, see "
                   "github.com/biggy-spoon/sticky-assistant",
                   {0, 0}, false);
    ui::WaitIdle(10000);
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

    const bool configured = settings_service::IsConfigured();
    if (!configured || SetupRequested()) {
        ESP_LOGI(kTag, "%s, starting setup portal",
                 configured ? "setup requested" : "not configured yet");
        ShowSetupScreen(!configured);
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
    esp_timer_create_args_t press_args = {};
    press_args.callback = &SinglePressTimeout;
    press_args.name = "single_press";
    ESP_ERROR_CHECK(esp_timer_create(&press_args, &s_single_press_timer));
    xTaskCreate(PowerTask, "power_actions", 4096, nullptr, 3, &s_power_task);

    xTaskCreate(AssistantTask, "assistant", kAssistantTaskStackWords, nullptr, 5,
                &s_assistant_task);

    button_service::SetHandlers(&OnPressStart, &OnPressEnd, nullptr);

    touch_service::SetGestureHandler(&OnGesture, nullptr);
    err = touch_service::Init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "touch unavailable: %s", esp_err_to_name(err));
    }

    device_nav::ShowCached(woke);
    const settings_service::Settings settings = settings_service::Get();
    ESP_LOGI(kTag, "%s, connecting to WiFi...", woke ? "woke from sleep" : "powered on");
    const bool wifi_connected =
        wifi_service::ConnectStaBlocking(settings.wifi_ssid, settings.wifi_password) == ESP_OK;
    if (!wifi_connected) {
        ESP_LOGE(kTag, "WiFi connect failed; recording will work but uploads will fail");
    }
    device_nav::Start(wifi_connected, woke);
    if (wifi_connected && settings_web::Start() != ESP_OK) {
        ESP_LOGW(kTag, "settings page unavailable");
    }
    vTaskDelay(pdMS_TO_TICKS(8000));
    power_service::NoteActivity();  // the five minutes count from here

    ESP_LOGI(kTag, "ready: hold the AI button to talk; tap tiles, swipe or use Up/Down to move");
}
