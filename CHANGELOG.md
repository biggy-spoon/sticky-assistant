# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
This project doesn't have tagged releases yet -- everything below is
tracked under Unreleased until there's a first stable cut.

## [Unreleased]

### Added
- Project scaffold: ESP-IDF firmware for the reTerminal Sticky (button,
  PDM mic, WiFi, ePaper SPI driver, HTTP client) and a FastAPI backend
  (faster-whisper transcription + Claude API call).
- Browser-based first-run setup: the device opens an AP + captive portal
  for entering WiFi credentials and the backend URL, replacing
  Kconfig/recompile-based configuration. Settings persist in NVS.
- LVGL-based on-device UI: idle, recording, thinking, and reply screens
  with real typography, replacing a placeholder status bar.
- Backend dashboard (`/dashboard`): conversation history, device status,
  upcoming calendar events, and editable settings (API key, Claude model,
  calendar connection) with no restart required for those.
- Calendar integration: Claude gets `create_calendar_event` and
  `list_calendar_events` tools once a calendar is connected, via either
  CalDAV (iCloud, Fastmail, Nextcloud, etc.) or Google Calendar (OAuth).
  Backed by a new SQLite store for conversation history, device status,
  and mutable settings.
- Plugin system (`backend/plugins/`): generalized the calendar
  integration into the first implementation of a `Plugin` contract
  (tool definitions, a declarative settings schema the dashboard renders
  generically, optional custom routes). `app.py`'s `/converse` handler
  and the dashboard's settings UI are now plugin-generic -- neither has
  calendar-specific logic left. A new plugin needs no backend or
  dashboard changes beyond implementing the contract and registering it.
- Dashboard visual redesign: a proper design-token system (color,
  spacing, radius, shadow, motion), icon-box card headers, status/
  connection pills, a light/dark toggle, and date-chip calendar rows,
  replacing the original flat dark-only cards.
- Todo-list plugin (`add_todo`/`list_todos`/`complete_todo`), the second
  default plugin alongside calendar -- no settings, backed directly by a
  new `todos` table.
- Plugins are now auto-discovered: every `.py` file in `backend/plugins/`
  is scanned at import time for a `Plugin` subclass, instead of being
  hardcoded into the registry. Installing a community plugin is now
  "copy its file into `backend/plugins/`, restart" -- no code changes.
- Plugin hub: a new `GET /api/plugin-hub` endpoint and dashboard card
  surface a community-maintained index of third-party plugins from the
  companion [`sticky-assistant-plugin-hub`](https://github.com/biggy-spoon/sticky-assistant-plugin-hub)
  repo. The hub is metadata only -- it never fetches or runs a plugin's
  code automatically.
- Dashboard split into tabs (Overview, Plugins, Plugin hub, History)
  instead of one long scrolling page.
- On-device plugin views: a new `Plugin.device_view()`/`device_action()`
  contract lets a plugin describe its own ePaper screen (calendar's
  upcoming events, todo's open items); the device's previously-unused
  Up/Down buttons (GPIO5/GPIO6, confirmed from Seeed's hardware-overview
  docs and an independent review) page through them, and a short tap of
  the AI button confirms/toggles the highlighted row. `button_service`
  now polls all three buttons; a new firmware `device_nav` component
  owns the paging state machine and the backend round-trips.
- Home screen page-position indicator: up to 3 small dots in the
  bottom-right corner (home shown filled/current, plus the next couple
  of plugin views), with a ">" arrow if there are more views beyond
  what the 3 dots show. Only appears once the view list has actually
  been fetched at least once this boot -- deliberately not fetched
  proactively just to populate the indicator, to avoid a network round
  trip every time the idle screen shows on a battery-powered device.
- Touchscreen support: new `touch_service` drives the GT911 and reports
  taps in screen coordinates. Tapping a device-view row selects it, and
  tapping a checkable row completes it.
- Redesigned device-view screens: larger rows, drawn checkboxes, a position
  counter, and an optional second line on each row, which calendar events use
  for their date.
- Web search: Claude can answer questions from the internet using Anthropic's
  server-side search tool, up to 3 searches per question. The system prompt no
  longer steers answers towards the device itself.
- Swipe gestures: left and right change page, up and down scroll a page at a
  time. The Up/Down buttons now change page directly instead of moving a row
  highlight, which is gone.
- A page indicator on the home, reply, plugin and detail screens: a window of 3
  dots with arrows when more pages lie beyond it.
- Calendar details: tapping an event opens its date, time and description. A
  short press of the AI button now means "back".
- Todos stay on the device list when completed, with a ticked box and
  strikethrough, and move to the bottom. Tapping again reopens them. Finished
  items drop off at midnight (new `todo_timezone` setting) and unfinished ones
  carry over.
- Plugin pages draw immediately from a cached copy and refresh in the
  background, redrawing only if the current page changed.
- A new on-device interface modelled on Sticky OS: a status bar with the
  time, page dots, WiFi and battery on every screen; a home screen with a large
  clock, the next event and a tile for Claude and each plugin; Calendar as an
  agenda with a "now" card and a week strip; the todo list as a checklist; and
  the last conversation on its own Claude page. Liberation Sans replaces the
  Montserrat fonts.
- Partial e-paper refresh: screens change in well under a second without a
  flash, with a full refresh mixed in to clear ghosting, so the clock can update
  every minute.
- Sleep: after 5 minutes without input the current screen is frozen with a
  moon and the board deep-sleeps. Any button wakes it on the same page, and the
  last conversation survives.
- Battery level and charging indicator, and charging enabled: the charger's
  enable pin was never driven, which can stop the battery charging.
- A `timezone` setting on the dashboard for the device's clock, the agenda and
  the todo reset (the todo plugin's own setting is still read if it's set).
- Plugins can return `view_type="agenda"` with `start_ts`, `end_ts` and
  `all_day` on each item, and `view_type="text"` views are now drawn.
- Weather plugin: current conditions on the home screen, a 7-day forecast with
  hourly detail on the device, and a `get_weather_forecast` tool, from
  Open-Meteo.
- A default Settings plugin: units, timezone, weather location and font on the
  device (tap a row to step it on), on the dashboard, and by voice through an
  `update_settings` tool. Plugin items can carry `action: "tap"`.
- Calendar: `update_calendar_event` and `delete_calendar_event` tools (CalDAV
  and Google). Times without a timezone are read in the user's.
- Conversation memory: the last three exchanges, for ten minutes.
- AI button: a double press is a full refresh, and a single press on home
  sleeps. A sleeping screen shows an "Asleep" banner.
- Custom fonts: `firmware/tools/make_fonts.sh` builds your own TrueType font
  into a git-ignored folder, and the device's Settings page switches to it.
- AI providers: Claude, Gemini, or any OpenAI-compatible API, chosen on the
  dashboard or the device, each with its own key and model. The model list
  comes from the provider. New `ai_providers.py`; `GET /api/ai/models`.
- Picker screens on the device for the AI provider, model, units, timezone and
  font, and an on-screen keyboard for the weather location. Plugin items can
  carry `action: "choose"` with `options`, or `action: "text"`, answered
  through the new `Plugin.device_input(item_id, value)`.
- The device calls the assistant after the active model ("Claude", "Gemini",
  "GPT", ...) on its home tile and conversation screens, and the AI picker marks
  providers that have no API key, which can't be chosen from the device.
- The weather page in the style of Apple's Weather app, with a day view.
- The battery log shows the current, the gauge's flags and the charger pin, to
  confirm charging.
- The settings page moved onto the device: open the Sticky's address in a
  browser. It adds the device's own settings (backend address, WiFi, sleep
  timeout, including never), and keeps the device awake while it's open. The
  backend no longer serves a dashboard; `/dashboard` redirects to the device.
- Plugins can be installed from the settings page or the device's Settings
  page, limited to hub entries marked reviewed, and load without a restart.
  The device's Settings page lists plugins and shows the settings page's
  address.

### Changed
- `/converse` transcribes and asks the model on a worker thread, so the device
  and dashboard can still reach the backend while a reply is being worked out.
- Page changes and scrolling use a full e-paper refresh, since partial refreshes
  of a whole new page ghosted badly. Partial refreshes, with a second pass that
  darkens text, are kept for updates within a screen.
- Flash configured as 16MB with a custom partition table (6MB app). NVS stays
  at the same offset, so saved settings survive the update.
- **Plugin API:** `Plugin.device_action(item_id)` is now
  `device_action(item_id, checked)`, and `POST
  /api/plugins/{id}/device-action` requires `checked`. `DeviceViewItem` gains an
  optional `detail`. `storage.complete_todo_by_id` is replaced by
  `set_todo_done`.

### Fixed
- The sleep screen looked grey and smudged: its last frame was a partial
  refresh, which leaves the panel greyer, and it stays up for hours. It's now a
  full refresh, as in Sticky OS.
- The battery never charged on this firmware: the charger's enable pin, GPIO39,
  starts out as a JTAG pin, and setting only its direction left it there, so
  the charger stayed off. It's now configured as a GPIO first.
- The battery level: I2C controller 1 stops after its first transaction on this
  board, so the gauge is read on a bus set up fresh each time, and its first
  answer, which can be stale, is read twice.
- Google Calendar all-day events came through as events at midnight.
- Rendering ran on the button polling task, which overflowed its stack and
  hid button releases for the length of a screen refresh, so every AI press
  measured about 2.3 seconds. A dedicated UI task now owns the display.
- Characters missing from the display font, such as `➤`, drew as empty boxes.
  They're now mapped to what the font can draw.
- The board's power latch (GPIO45/46) is now set at boot.
- `epaper_display`'s SSD1677 command sequence, written from generic
  SSD16xx recollection at scaffold time, never produced a visible update
  once real (non-solid-block) content needed rendering. Ported the
  proven sequence from the `folloup-sticky` reference project (same
  board/panel) -- it was missing the "old" RAM plane write, the booster
  soft-start sequence, and Display Update Control 1 entirely, and used
  byte-address RAM-X-window granularity where this controller expects
  pixel addressing. Confirmed visually on hardware afterward.
  `firmware/components/epaper_display` is GPLv3 as a result (ported from
  a GPLv3 source); the rest of the repository stays MIT.
- SPI DMA transfer-size overflow: a single `spi_device_transmit` call
  covering the full ~48KB framebuffer exceeded the ESP32-S3's
  `SPI_LL_DMA_MAX_BIT_LEN` (32768 bytes); chunked into 4096-byte writes.
- Two real ESP-IDF compile errors caught by actually building against the
  toolchain for the first time: a nonexistent I2S PDM slot-config macro
  name, and a component missing `esp_timer` in its `CMakeLists.txt`
  `REQUIRES` (only fails once something actually includes the header).
- Backend: `faster-whisper==1.0.3` with an unconstrained `av` dependency
  resolved to a combination that crashed every real transcription
  (`av.open()` no longer accepts the kwarg `faster-whisper`'s
  `decode_audio()` passes). Bumped `faster-whisper` and pinned `av` to a
  combination confirmed to work.
- Backend: Claude API exceptions in the `/converse` handler weren't
  caught, so a real API failure (bad key, rate limit, network error)
  surfaced to the device as an opaque 500 instead of a useful error.
- Backend: `anthropic==0.40.0` throws `AttributeError` parsing any
  response containing `tool_use` blocks on this stack (its
  discriminated-union handling). Upgraded to `2.5.0`.
- The calendar plugin's dashboard view (`GET /api/plugins/calendar/view`)
  didn't catch provider exceptions, so a misconfigured or temporarily
  unreachable calendar crashed the whole endpoint with a 500 instead of
  showing a clean error in the dashboard.

## Earlier

Everything before the dashboard/calendar work was built and verified in
one continuous session: scaffold, hardware bring-up (ESP-IDF toolchain
install, first real build, first flash), the captive portal, the LVGL
UI, and the display driver fix, each confirmed against real hardware
(a Seeed Studio reTerminal Sticky) before moving on. See the git history
for the detailed story -- several of these commits document a real bug
found by actually running the thing, not just compiling it.
