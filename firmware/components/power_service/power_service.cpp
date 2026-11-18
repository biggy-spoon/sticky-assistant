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
constexpr int kI2cTimeoutMs = 100;
constexpr uint8_t kGaugeStateOfCharge = 0x2C;  // BQ27220 standard commands: percent
constexpr uint8_t kGaugeVoltage = 0x08;        // ... millivolts
constexpr uint8_t kGaugeCurrent = 0x0C;        // ... milliamps, positive while charging
constexpr uint8_t kGaugeBatteryStatus = 0x0A;  // ... and flags; bit 0 (DSG) set while discharging
// The gauge misses the odd request. The last good reading stands until this
// many in a row fail, so one hiccup doesn't blank the status bar.
constexpr int kFailuresBeforeUnknown = 4;

constexpr gpio_num_t kButtons[] = {BOARD_BUTTON_AI_PIN, BOARD_BUTTON_UP_PIN,
                                   BOARD_BUTTON_DOWN_PIN};

SemaphoreHandle_t s_lock = nullptr;
Status s_status;
int s_failures = 0;
std::atomic<int64_t> s_last_activity_us{0};

void HoldAt(gpio_num_t pin, int level) {
    gpio_hold_dis(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, level);
    gpio_hold_en(pin);
}

// I2C controller 1 stops working after its first transaction on this board
// (every device on the bus then times out, with both lines idle high), so each
// reading sets the bus up fresh and takes it down again.
bool ReadGauge(int* percent, int* millivolts, int* milliamps, int* flags) {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_1;  // the touch panel has I2C_NUM_0
    bus_cfg.sda_io_num = BOARD_SENSOR_I2C_SDA_PIN;
    bus_cfg.scl_io_num = BOARD_SENSOR_I2C_SCL_PIN;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = 1;
    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "sensor bus setup failed: %s", esp_err_to_name(err));
        return false;
    }
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = BOARD_FUEL_GAUGE_ADDR;
    dev_cfg.scl_speed_hz = kI2cHz;
    i2c_master_dev_handle_t gauge = nullptr;
    err = i2c_master_bus_add_device(bus, &dev_cfg, &gauge);

    uint8_t soc[2] = {};
    uint8_t volts[2] = {};
    // The first answer after the gauge wakes can be stale (3% where the second
    // read, 30ms later, says 81%), so the percent is read twice.
    for (int i = 0; i < 2 && err == ESP_OK; ++i) {
        err = i2c_master_transmit_receive(gauge, &kGaugeStateOfCharge, 1, soc, sizeof(soc),
                                          kI2cTimeoutMs);
    }
    if (err == ESP_OK) {
        err = i2c_master_transmit_receive(gauge, &kGaugeVoltage, 1, volts, sizeof(volts),
                                          kI2cTimeoutMs);
    }
    uint8_t amps[2] = {};
    if (err == ESP_OK) {
        err = i2c_master_transmit_receive(gauge, &kGaugeCurrent, 1, amps, sizeof(amps),
                                          kI2cTimeoutMs);
    }
    uint8_t status[2] = {};
    if (err == ESP_OK) {
        err = i2c_master_transmit_receive(gauge, &kGaugeBatteryStatus, 1, status, sizeof(status),
                                          kI2cTimeoutMs);
    }
    if (gauge != nullptr) {
        i2c_master_bus_rm_device(gauge);
    }
    i2c_del_master_bus(bus);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "battery read error: %s", esp_err_to_name(err));
        return false;
    }
    const int value = soc[0] | (soc[1] << 8);
    if (value > 100) {
        ESP_LOGW(kTag, "gauge returned %d%%", value);
        return false;
    }
    *percent = value;
    *millivolts = volts[0] | (volts[1] << 8);
    *milliamps = static_cast<int16_t>(amps[0] | (amps[1] << 8));
    *flags = status[0] | (status[1] << 8);
    return true;
}

void ReadStatus() {
    int percent = 0;
    int millivolts = 0;
    int milliamps = 0;
    int flags = 0;
    const bool read = ReadGauge(&percent, &millivolts, &milliamps, &flags);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.usb_power = gpio_get_level(BOARD_USB_POWER_PIN) == 1;
    if (read) {
        const bool charging = milliamps > 0;
        if (percent != s_status.percent || !s_status.battery_known || charging != s_status.charging) {
            // The charger pin level and the gauge's flags, to see why it isn't
            // charging when USB power is there.
            ESP_LOGI(kTag, "battery %d%% at %d mV, %d mA (%s), gauge flags 0x%04x, charger pin %d",
                     percent, millivolts, milliamps, charging ? "charging" : "not charging", flags,
                     gpio_get_level(BOARD_CHARGER_EN_PIN));
        }
        s_status.charging = charging;
        s_failures = 0;
        s_status.battery_known = true;
        s_status.percent = percent;
    } else if (++s_failures >= kFailuresBeforeUnknown) {
        s_status.battery_known = false;
    }
    const int failures = s_failures;
    xSemaphoreGive(s_lock);
    if (!read) {
        ESP_LOGW(kTag, "battery read failed (%d in a row)", failures);
    }
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
