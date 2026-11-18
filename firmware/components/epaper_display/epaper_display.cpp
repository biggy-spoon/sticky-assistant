// SSD1677 driver for the reTerminal Sticky's 800x480 panel.
//
// The controller sequences (init, RAM window, full and partial refresh,
// power-off and deep sleep) follow the MIT-licensed SSD1677 driver by limengdu
// that Sticky OS vendors (https://github.com/Sticky-OS/sticky-os,
// firmware/sticky-os/src/sticky_drivers/ssd1677.c). Its copyright and license
// notice is in LICENSE-sticky-os in this directory. The SPI transport, the
// framebuffers and the darkening pass are this project's own.
#include "epaper_display.h"

#include <algorithm>
#include <cstring>

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace epaper_display {
namespace {

constexpr const char* kTag = "epaper_display";
constexpr uint32_t kSpiClockHz = 4 * 1000 * 1000;
constexpr uint32_t kBusyPollMs = 10;
constexpr uint32_t kBusyTimeoutMs = 20000;
// One SPI DMA transaction can't exceed 32KB on the ESP32-S3, and a frame is
// 48000 bytes, so data goes out in pieces.
constexpr size_t kSpiChunkBytes = 4096;

enum class Mode { kFull, kPartial };

// Controller commands (SSD1677 datasheet).
enum Command : uint8_t {
    kDriverOutput = 0x01,
    kBoosterSoftStart = 0x0C,
    kDeepSleep = 0x10,
    kDataEntryMode = 0x11,
    kSoftwareReset = 0x12,
    kTemperatureSensor = 0x18,
    kMasterActivate = 0x20,
    kUpdateSequence = 0x22,
    kWriteCurrent = 0x24,   // the image to show
    kWritePrevious = 0x26,  // what's showing now; the controller drives the difference
    kBorderWaveform = 0x3C,
    kRamXRange = 0x44,
    kRamYRange = 0x45,
    kRamXCounter = 0x4E,
    kRamYCounter = 0x4F,
};

// Update sequences for kUpdateSequence.
constexpr uint8_t kSequenceFull = 0xF7;
// Like a full update's fast mode, but leaves the analog power and oscillator
// on, so back-to-back partial refreshes skip the charge-pump ramp.
constexpr uint8_t kSequencePartial = 0xFC;
constexpr uint8_t kSequencePowerOff = 0x83;

// The panel's RAM row 0 is the bottom of the glass as the Sticky is held, so
// rows go into the framebuffer bottom-up and SetPixel flips y.
constexpr bool kFlipRows = true;

spi_device_handle_t s_spi = nullptr;
uint8_t* s_framebuffer = nullptr;  // 1bpp, row-major, MSB leftmost, 1 = white
// What the glass shows, the "previous" plane for a partial refresh. Valid
// only after a full refresh since the controller last reset or slept.
uint8_t* s_previous = nullptr;
bool s_baseline_valid = false;
bool s_analog_on = false;  // left on by a partial refresh

esp_err_t Transmit(const uint8_t* data, size_t len, bool is_data) {
    gpio_set_level(BOARD_EPD_DC_PIN, is_data ? 1 : 0);
    for (size_t offset = 0; offset < len; offset += kSpiChunkBytes) {
        spi_transaction_t transaction = {};
        transaction.length = std::min(kSpiChunkBytes, len - offset) * 8;
        transaction.tx_buffer = data + offset;
        const esp_err_t err = spi_device_transmit(s_spi, &transaction);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t Write(uint8_t command, const uint8_t* data = nullptr, size_t len = 0) {
    esp_err_t err = Transmit(&command, 1, false);
    if (err == ESP_OK && len > 0) {
        err = Transmit(data, len, true);
    }
    return err;
}

esp_err_t Write8(uint8_t command, uint8_t value) { return Write(command, &value, 1); }

esp_err_t WaitReady() {
    for (uint32_t waited = 0; gpio_get_level(BOARD_EPD_BUSY_PIN) == 1; waited += kBusyPollMs) {
        if (waited >= kBusyTimeoutMs) {
            ESP_LOGW(kTag, "panel still busy after %lums", static_cast<unsigned long>(waited));
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(kBusyPollMs));
    }
    return ESP_OK;
}

void Reset() {
    gpio_set_level(BOARD_EPD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

#define RETURN_IF_ERROR(expr)              \
    do {                                   \
        const esp_err_t err_ = (expr);     \
        if (err_ != ESP_OK) return err_;   \
    } while (0)

// Runs an update sequence and waits for the panel to finish it.
esp_err_t Activate(uint8_t sequence) {
    RETURN_IF_ERROR(Write8(kUpdateSequence, sequence));
    RETURN_IF_ERROR(Write(kMasterActivate));
    return WaitReady();
}

// Turns off the analog power a partial refresh leaves on, before a reset or
// sleep that would otherwise cut it uncleanly.
esp_err_t PowerOff() {
    if (!s_analog_on) {
        return ESP_OK;
    }
    RETURN_IF_ERROR(Activate(kSequencePowerOff));
    s_analog_on = false;
    return ESP_OK;
}

// Points the RAM address counters back at the start of the frame.
esp_err_t RewindRam() {
    const uint8_t zero[] = {0x00, 0x00};
    RETURN_IF_ERROR(Write(kRamXCounter, zero, sizeof(zero)));
    return Write(kRamYCounter, zero, sizeof(zero));
}

// Reset and configure the controller for a refresh: power, gate scan, RAM
// addressing (x then y, both increasing, over the whole panel) and the
// border. Every refresh starts from a reset, which the panel needs to give a
// clean waveform; only a full one adds the software reset.
esp_err_t Configure(Mode mode) {
    RETURN_IF_ERROR(PowerOff());
    Reset();
    RETURN_IF_ERROR(WaitReady());
    if (mode == Mode::kFull) {
        RETURN_IF_ERROR(Write(kSoftwareReset));
        RETURN_IF_ERROR(WaitReady());
    }
    const uint16_t last_x = BOARD_EPD_WIDTH - 1;
    const uint16_t last_y = BOARD_EPD_HEIGHT - 1;
    const uint8_t soft_start[] = {0xAE, 0xC7, 0xC3, 0xC0, 0x80};
    const uint8_t driver_output[] = {static_cast<uint8_t>(last_y & 0xFF),
                                     static_cast<uint8_t>(last_y >> 8), 0x02};
    const uint8_t x_range[] = {0x00, 0x00, static_cast<uint8_t>(last_x & 0xFF),
                               static_cast<uint8_t>(last_x >> 8)};
    const uint8_t y_range[] = {0x00, 0x00, static_cast<uint8_t>(last_y & 0xFF),
                               static_cast<uint8_t>(last_y >> 8)};
    RETURN_IF_ERROR(Write8(kTemperatureSensor, 0x80));  // the built-in sensor
    // A full refresh cycles the border with the image; a partial one leaves it.
    RETURN_IF_ERROR(Write8(kBorderWaveform, mode == Mode::kFull ? 0x01 : 0x80));
    RETURN_IF_ERROR(Write(kBoosterSoftStart, soft_start, sizeof(soft_start)));
    RETURN_IF_ERROR(Write(kDriverOutput, driver_output, sizeof(driver_output)));
    RETURN_IF_ERROR(Write8(kDataEntryMode, 0x03));
    RETURN_IF_ERROR(Write(kRamXRange, x_range, sizeof(x_range)));
    RETURN_IF_ERROR(Write(kRamYRange, y_range, sizeof(y_range)));
    RETURN_IF_ERROR(RewindRam());
    return WaitReady();
}

esp_err_t WritePlane(uint8_t command, const uint8_t* frame) {
    RETURN_IF_ERROR(RewindRam());
    return Write(command, frame, BOARD_EPD_BUFFER_LEN);
}

esp_err_t FullRefresh() {
    RETURN_IF_ERROR(Configure(Mode::kFull));
    // Nothing to compare against: both planes get the new image.
    RETURN_IF_ERROR(WritePlane(kWritePrevious, s_framebuffer));
    RETURN_IF_ERROR(WritePlane(kWriteCurrent, s_framebuffer));
    return Activate(kSequenceFull);
}

// The fast waveform leaves black pixels grey. A second pass with an all-white
// "previous" plane drives every black pixel again, as if it had just turned
// black, while white pixels see white-to-white and are left alone.
esp_err_t DarkenPass() {
    static uint8_t* white = nullptr;
    if (white == nullptr) {
        white = static_cast<uint8_t*>(heap_caps_malloc(BOARD_EPD_BUFFER_LEN, MALLOC_CAP_SPIRAM));
        if (white == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        memset(white, 0xFF, BOARD_EPD_BUFFER_LEN);
    }
    RETURN_IF_ERROR(WritePlane(kWritePrevious, white));
    RETURN_IF_ERROR(WritePlane(kWriteCurrent, s_framebuffer));
    return Activate(kSequencePartial);
}

esp_err_t PartialRefresh() {
    RETURN_IF_ERROR(Configure(Mode::kPartial));
    RETURN_IF_ERROR(WritePlane(kWritePrevious, s_previous));
    RETURN_IF_ERROR(WritePlane(kWriteCurrent, s_framebuffer));
    RETURN_IF_ERROR(Activate(kSequencePartial));
    s_analog_on = true;
    return DarkenPass();
}

esp_err_t ConfigureGpio() {
    gpio_config_t outputs = {};
    outputs.pin_bit_mask = (1ULL << BOARD_EPD_DC_PIN) | (1ULL << BOARD_EPD_RST_PIN) |
                           (1ULL << BOARD_EPD_POWER_EN_PIN);
    outputs.mode = GPIO_MODE_OUTPUT;
    RETURN_IF_ERROR(gpio_config(&outputs));
    gpio_config_t busy = {};
    busy.pin_bit_mask = 1ULL << BOARD_EPD_BUSY_PIN;
    busy.mode = GPIO_MODE_INPUT;
    return gpio_config(&busy);
}

esp_err_t ConfigureSpi() {
    spi_bus_config_t bus = {};
    bus.mosi_io_num = BOARD_EPD_MOSI_PIN;
    bus.miso_io_num = -1;  // the panel is write-only
    bus.sclk_io_num = BOARD_EPD_SCK_PIN;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = kSpiChunkBytes;
    RETURN_IF_ERROR(spi_bus_initialize(BOARD_EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t device = {};
    device.clock_speed_hz = kSpiClockHz;
    device.mode = 0;
    device.spics_io_num = BOARD_EPD_CS_PIN;
    device.queue_size = 1;
    return spi_bus_add_device(BOARD_EPD_SPI_HOST, &device, &s_spi);
}

}  // namespace

esp_err_t Init() {
    esp_err_t err = ConfigureGpio();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "gpio config failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level(BOARD_EPD_POWER_EN_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    err = ConfigureSpi();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "spi config failed: %s", esp_err_to_name(err));
        return err;
    }
    if (s_framebuffer == nullptr) {
        s_framebuffer =
            static_cast<uint8_t*>(heap_caps_malloc(BOARD_EPD_BUFFER_LEN, MALLOC_CAP_SPIRAM));
        s_previous =
            static_cast<uint8_t*>(heap_caps_malloc(BOARD_EPD_BUFFER_LEN, MALLOC_CAP_SPIRAM));
        if (s_framebuffer == nullptr || s_previous == nullptr) {
            ESP_LOGE(kTag, "couldn't allocate the %d byte frames", BOARD_EPD_BUFFER_LEN);
            return ESP_ERR_NO_MEM;
        }
    }
    Clear(false);
    ESP_LOGI(kTag, "initialized: %dx%d", BOARD_EPD_WIDTH, BOARD_EPD_HEIGHT);
    return ESP_OK;
}

void Clear(bool black) {
    if (s_framebuffer != nullptr) {
        memset(s_framebuffer, black ? 0x00 : 0xFF, BOARD_EPD_BUFFER_LEN);
    }
}

void SetPixel(int x, int y, bool black) {
    if (s_framebuffer == nullptr || x < 0 || y < 0 || x >= BOARD_EPD_WIDTH ||
        y >= BOARD_EPD_HEIGHT) {
        return;
    }
    const int row = kFlipRows ? BOARD_EPD_HEIGHT - 1 - y : y;
    const size_t index = (static_cast<size_t>(row) * BOARD_EPD_WIDTH + x) / 8;
    const uint8_t mask = 0x80 >> (x % 8);
    if (black) {
        s_framebuffer[index] &= ~mask;
    } else {
        s_framebuffer[index] |= mask;
    }
}

esp_err_t Refresh(bool full, bool* did_full) {
    if (s_framebuffer == nullptr || s_spi == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool run_full = full || !s_baseline_valid;
    if (did_full != nullptr) {
        *did_full = run_full;
    }
    const esp_err_t err = run_full ? FullRefresh() : PartialRefresh();
    if (err == ESP_OK) {
        memcpy(s_previous, s_framebuffer, BOARD_EPD_BUFFER_LEN);
        s_baseline_valid = true;
    } else {
        ESP_LOGE(kTag, "%s refresh failed: %s", run_full ? "full" : "partial",
                 esp_err_to_name(err));
        s_baseline_valid = false;  // unknown what's on the glass now
    }
    return err;
}

esp_err_t Sleep() {
    if (s_spi == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = PowerOff();
    const esp_err_t sleep_err = Write8(kDeepSleep, 0x03);
    vTaskDelay(pdMS_TO_TICKS(100));
    s_baseline_valid = false;  // waking needs a reset, then a full refresh
    return err != ESP_OK ? err : sleep_err;
}

}  // namespace epaper_display
