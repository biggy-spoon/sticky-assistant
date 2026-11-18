#include "power_service.h"

#include <atomic>
#include <initializer_list>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace power_service {
namespace {

constexpr const char* kTag = "power_service";
constexpr uint32_t kTaskStackBytes = 3072;
constexpr uint32_t kReadIntervalMs = 30 * 1000;
constexpr uint32_t kI2cHz = 100000;
constexpr int kI2cTimeoutMs = 50;
constexpr uint8_t kGaugeStateOfCharge = 0x2C;  // BQ27220 standard command, percent

constexpr gpio_num_t kButtons[] = {BOARD_BUTTON_AI_PIN, BOARD_BUTTON_UP_PIN,
                                   BOARD_BUTTON_DOWN_PIN};

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_gauge = nullptr;
SemaphoreHandle_t s_lock = nullptr;
Status s_status;
std::atomic<int64_t> s_last_activity_us{0};

void HoldAt(gpio_num_t pin, int level) {
    gpio_hold_dis(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, level);
    gpio_hold_en(pin);
}

bool ReadPercent(int* percent) {
    if (s_gauge == nullptr) {
        return false;
    }
    uint8_t data[2] = {};
    if (i2c_master_transmit_receive(s_gauge, &kGaugeStateOfCharge, 1, data, sizeof(data),
                                    kI2cTimeoutMs) != ESP_OK) {
        return false;
    }
    const int value = data[0] | (data[1] << 8);
    if (value > 100) {
        return false;
    }
    *percent = value;
    return true;
}

void ReadStatus() {
    Status status;
    status.usb_power = gpio_get_level(BOARD_USB_POWER_PIN) == 1;
    status.battery_known = ReadPercent(&status.percent);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status = status;
    xSemaphoreGive(s_lock);
}

void PowerTask(void*) {
    while (true) {
        ReadStatus();
        vTaskDelay(pdMS_TO_TICKS(kReadIntervalMs));
    }
}

}  // namespace

void HoldPowerOn() {
    // After deep sleep the latch pins are held: set the level before letting go.
    for (gpio_num_t pin : {BOARD_POWER_HOLD_PIN, BOARD_POWER_LOCK_PIN}) {
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pin, 1);
    }
    gpio_deep_sleep_hold_dis();
    for (gpio_num_t pin : {BOARD_POWER_HOLD_PIN, BOARD_POWER_LOCK_PIN, BOARD_CHARGER_EN_PIN,
                           BOARD_EPD_POWER_EN_PIN, BOARD_TP_PWR_EN_PIN, BOARD_TP_RST_PIN,
                           BOARD_PDM_POWER_EN_PIN, BOARD_BUTTON_AI_PIN, BOARD_BUTTON_UP_PIN,
                           BOARD_BUTTON_DOWN_PIN}) {
        gpio_hold_dis(pin);
    }
}

esp_err_t Init() {
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    s_last_activity_us = esp_timer_get_time();

    gpio_set_direction(BOARD_CHARGER_EN_PIN, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(BOARD_CHARGER_EN_PIN, 0);  // active low: charge whenever USB is present

    gpio_config_t usb_cfg = {};
    usb_cfg.pin_bit_mask = 1ULL << BOARD_USB_POWER_PIN;
    usb_cfg.mode = GPIO_MODE_INPUT;
    gpio_config(&usb_cfg);

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_1;  // the touch panel has I2C_NUM_0
    bus_cfg.sda_io_num = BOARD_SENSOR_I2C_SDA_PIN;
    bus_cfg.scl_io_num = BOARD_SENSOR_I2C_SCL_PIN;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = 1;
    if (i2c_new_master_bus(&bus_cfg, &s_bus) == ESP_OK) {
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = BOARD_FUEL_GAUGE_ADDR;
        dev_cfg.scl_speed_hz = kI2cHz;
        if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_gauge) != ESP_OK) {
            s_gauge = nullptr;
        }
    }

    ReadStatus();
    ESP_LOGI(kTag, "battery %s %d%%, usb power %s",
             s_status.battery_known ? "at" : "unknown,", s_status.percent,
             s_status.usb_power ? "yes" : "no");
    if (xTaskCreate(PowerTask, "power", kTaskStackBytes, nullptr, 2, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool WokeFromSleep() { return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1; }

Status GetStatus() {
    if (s_lock == nullptr) {
        return {};
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const Status status = s_status;
    xSemaphoreGive(s_lock);
    return status;
}

void NoteActivity() { s_last_activity_us = esp_timer_get_time(); }

int64_t IdleMs() { return (esp_timer_get_time() - s_last_activity_us) / 1000; }

void EnterDeepSleep() {
    // A button still down would wake the board straight away.
    bool any_down = true;
    while (any_down) {
        any_down = false;
        for (gpio_num_t pin : kButtons) {
            any_down = any_down || gpio_get_level(pin) == 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(60));

    HoldAt(BOARD_POWER_HOLD_PIN, 1);
    HoldAt(BOARD_POWER_LOCK_PIN, 1);
    HoldAt(BOARD_CHARGER_EN_PIN, 0);  // keep charging while asleep
    for (gpio_num_t pin : {BOARD_EPD_POWER_EN_PIN, BOARD_TP_PWR_EN_PIN, BOARD_TP_RST_PIN,
                           BOARD_PDM_POWER_EN_PIN}) {
        HoldAt(pin, 0);
    }
    uint64_t wake_mask = 0;
    for (gpio_num_t pin : kButtons) {
        gpio_hold_dis(pin);
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_pullup_en(pin);
        gpio_hold_en(pin);
        wake_mask |= 1ULL << pin;
    }
    esp_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
    gpio_deep_sleep_hold_en();
    ESP_LOGI(kTag, "deep sleep: any button wakes");
    vTaskDelay(pdMS_TO_TICKS(20));  // let the log line out
    esp_deep_sleep_start();
}

}  // namespace power_service
