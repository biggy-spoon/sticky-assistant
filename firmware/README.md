# sticky-assistant firmware

ESP-IDF (C++) firmware for the Seeed Studio reTerminal Sticky (ESP32-S3R8).
Hold the AI/record button, speak, release — the clip gets POSTed to the
`backend` service, which transcribes it and asks Claude (with calendar
tool-use if you've connected one), and the reply renders on the ePaper
screen.

The interface follows [Sticky OS](https://github.com/Sticky-OS/sticky-os): a
status bar on every screen, and a home screen with a clock, your next event and
a tile for Claude and each configured plugin (see `device_nav`, below). Tap a
tile, swipe left or right, or press Up/Down to move between pages, and tap the
status bar to go home. Swipe up or down to scroll. Tap a checkable row to toggle
it (e.g. a todo), or a row with details to open them (e.g. a calendar event). A
short press of the AI button goes back: from details to the list, and from any
other page to the home page. The home page is the widgets unless Settings >
Home page picks another, such as the calendar; the widgets then stay one swipe
away, and tapping the status bar on the home page goes to them. On home, a short press puts the board to sleep. A double
press redraws the screen with a full refresh, which clears any faint traces the
quick partial updates leave behind. A single press waits about a third of a
second before acting, to tell it from a double press.

After 5 minutes with no touch or button press, the current screen is drawn once
more with a moon in the status bar and an "Asleep" banner along the bottom, and
the board goes into deep sleep. Any
button wakes it, back on the same page.

## Build

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html)
v5.3.x. Built, flashed, and run on a real device — the full
record → transcribe → Claude → display loop is confirmed working on
hardware, not just compiled.

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

No WiFi/backend config step needed before flashing — the device handles
that itself on first boot (see "First-run setup" below).

### First-run setup

On first boot (or after a factory reset / NVS erase), the device opens an
open WiFi access point named `StickyAssistant-Setup`. Connect to it from a
phone or laptop; a setup page should open automatically (the captive
portal trick), or browse to `http://192.168.4.1/` manually. Enter your
real WiFi credentials and your backend's `/converse` URL
(e.g. `http://192.168.1.50:8000/converse`); the device saves this to NVS
and reboots into normal operation.

## Architecture

| Component | Role |
| --- | --- |
| `board` | GPIO/SPI pin map for the buttons, PDM mic, ePaper panel, touch, battery gauge and charger |
| `power_service` | Power latch, charging, battery level (BQ27220), idle time and deep sleep |
| `button_service` | Polls the AI/Up/Down buttons, debounced; press-start/press-end callbacks |
| `pdm_mic` | I2S PDM RX capture, 16kHz mono 16-bit |
| `recording_service` | PSRAM clip buffer + WAV encoding |
| `settings_service` | NVS-backed WiFi/backend settings, written by the portal |
| `captive_portal` | First-run AP + DNS hijack + HTTP setup form |
| `device_nav` cache | The last views response in the `storage` partition (`/cache/views.json`), drawn at boot before WiFi; rewritten only when the pages change |
| `settings_web` | The settings page on port 80 (embedded `settings.html`), plus `/device/config` for the device's own settings and `/device/ping` to stay awake while the page is open |
| `wifi_service` | STA connect (using `settings_service` values) + SoftAP mode |
| `assistant_service` | POSTs the WAV to the backend, parses `{transcript, reply}` |
| `device_nav` | Page changes, scrolling, taps and "back" across plugin device-views. Draws from a cached `GET /api/device/views` and refreshes in the background; sends toggles with `POST /api/plugins/{id}/device-action` |
| `touch_service` | GT911 bring-up, polling, tap and swipe detection, and mapping to screen coordinates |
| `epaper_display` | SPI transport + SSD1677 command set + 1bpp framebuffer, full and partial refresh, panel sleep |
| `ui` | LVGL screens in Sticky OS's style, rendered via `epaper_display`. Its own task owns LVGL and the panel, so `Show*` calls never block; if several arrive during a refresh, only the newest is drawn. LVGL's heap is 128KB in PSRAM |
| `main` | Wires button hold → record → upload → display, buttons and gestures → `device_nav`, and first-run routing |

## Using your own font

The device uses Liberation Sans by default. To use another typeface, give
`tools/make_fonts.sh` a regular and a bold TrueType file:

```sh
tools/make_fonts.sh ~/Fonts/MyFont-Regular.ttf ~/Fonts/MyFont-Bold.ttf
idf.py build flash
```

It writes the UI's seven sizes to `components/ui/fonts/custom/`, with the font's
name (from the file name) in `name.txt` for the Settings page's font picker, and
the build uses them whenever that folder exists. The folder is ignored by git, so a font
licensed only for your own devices never ends up in the repository. Delete the
folder to go back to Liberation Sans. Icons and drawings are shapes, not
characters, so they don't change. It needs Node.js for `npx lv_font_conv`, and
one `idf.py build` beforehand so LVGL's sources are downloaded.

Only printable ASCII, the degree sign and the bullet are taken from the font.
A serif or wider font takes more room, so check long titles after switching.

## Known gaps

1. **No pre-roll buffering.** Recording starts exactly when the capture
   task notices the button is down, so the first ~20-50ms of speech right
   at press can get clipped. Followup's `recording_service` solves this
   with a continuously-filled ring buffer armed ahead of the long-press
   threshold; worth porting that pattern if clipped first words become
   annoying in practice.
2. **Button timing not finely tuned.** `kMinHoldMs` (400ms) separates a tap
   from a recording; adjust against how the button feels in hand. Also: the
   AI/record button is physically the same pin as the device's power button,
   which has its own hardware-level long-hold behavior independent of firmware
   — holding it for several seconds may trigger a hardware power cycle
   unrelated to recording.
3. **No SD card or sensor support wired up** — the board has them, this
   project doesn't use them (out of scope for the voice-note flow).
4. **Touch bring-up isn't fully explained.** The GT911 only starts properly here
   with a real RST pulse followed by INT held low for 50ms. Other firmware for
   this board works without that INT step, and we don't yet know why.
5. **A tap during a refresh is ignored.** Partial refreshes are quick, but a
   tap that lands while one is running might hit a row that's about to move.
6. **Only buttons wake the board.** The touch controller is powered off in
   deep sleep, so a tap on a sleeping screen does nothing. The clock and battery
   on a sleeping screen are as they were when it fell asleep.
7. **Waking is a full refresh.** The panel's controller loses its image memory
   in sleep, so the first screen after waking flashes.

## Licensing note

The partial refresh, panel sleep and power-off sequences in `epaper_display`,
the deep sleep sequence in `power_service` and the battery pins in `board` are
adapted from [Sticky OS](https://github.com/Sticky-OS/sticky-os) and the
SSD1677 driver it vendors, both MIT. `components/ui/fonts` is Liberation Sans
under the SIL Open Font License (`OFL-LiberationSans.txt` there).

`components/epaper_display` is **GPLv3**, not MIT like the rest of this
repository — its SSD1677 command sequence is ported from the GPLv3
[`folloup-sticky`](https://github.com/alxv2016/folloup-sticky) project.
See that directory's own `LICENSE` file, and `docs/ARCHITECTURE.md` for
why a from-scratch version didn't work.
