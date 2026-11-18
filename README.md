# sticky-assistant

[![CI](https://github.com/biggy-spoon/sticky-assistant/actions/workflows/ci.yml/badge.svg)](https://github.com/biggy-spoon/sticky-assistant/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A push-to-talk voice assistant on a [Seeed Studio reTerminal Sticky](https://www.seeedstudio.com/sticky/),
proxied through your own always-on server to the AI of your choice: Claude by
default, or Gemini, OpenAI, or any OpenAI-compatible API, including models you
run yourself. No cloud middleman you don't control, no subscription beyond what
you already pay for API usage (which, for occasional voice notes, runs to cents
a month, not dollars).

Hold the button, speak, release. The reply shows up on the device's
e-paper screen a few seconds later. Ask it to put something on your
calendar and it actually will.

## Features

- **Push-to-talk voice notes**, answered by Claude, shown on the
  device's e-paper display with real typography (not a status bar).
- **Answers from the web**: Claude can search the internet, so you can ask
  about anything, not just your calendar and todos. Each search costs about
  1 cent on your Anthropic API key.
- **A calm e-paper interface** modelled on
  [Sticky OS](https://github.com/Sticky-OS/sticky-os): a status bar with the
  time, WiFi and battery, and a home screen with a big clock, your next event and
  a tile for Claude and each plugin. Tap a tile, swipe left or right, or use the
  Up/Down buttons to move between pages. Swipe up or down to scroll, tap a
  calendar event for its details, and tap a todo to check it off. Finished todos
  drop off the list at midnight, and unfinished ones carry over.
- **Sleeps when idle**: after 5 minutes without a touch or button press, or a
  single press of the top button on home, it freezes the current screen with an
  "Asleep" banner (e-paper keeps the picture with no power) and sleeps. Any
  button wakes it on the same page.
- **Weather**: the current conditions on the home screen and a 7-day forecast
  page where tapping a day shows its hours, from
  [Open-Meteo](https://open-meteo.com) (free, no API key).
- **Your choice of AI**: Claude by default, or Gemini, OpenAI, or anything that
  offers an OpenAI-compatible API (OpenRouter, Ollama, LM Studio and others).
  Web search works with Claude only.
- **Settings on the device**: AI provider and model, units, timezone, weather
  location and font, each on its own screen: a list to pick from, or an
  on-screen keyboard for the location. Also on the settings page, and changeable by
  voice ("switch to metric").
- **A calendar Claude can edit**: "move my dentist appointment to Friday at 3"
  moves it; events can also be renamed or deleted.
- **Follow-up questions**: the last few exchanges are remembered for ten
  minutes, so "yes, that one" makes sense.
- **No-recompile setup**: first boot opens a WiFi access point with a
  browser-based setup page for WiFi credentials and your backend's
  address. Settings are saved to the device's flash.
- **A settings page on the device itself**: open the Sticky's address in a
  browser on the same WiFi (its Settings page shows it) for conversation
  history, status, the AI provider and keys, every plugin's settings, the
  plugin hub, and the device's own WiFi, backend address and sleep timeout. The
  device stays awake while the page is open. No `.env` editing or restart
  needed.
- **A plugin system** for giving Claude new capabilities, shipping with
  two: calendar access (CalDAV or Google Calendar — "add a dentist
  appointment at 3pm on the 24th" actually lands on your calendar) and a
  todo list ("add buy milk to my todo list"). Every plugin is just a
  `.py` file in `backend/plugins/` discovered automatically at startup —
  writing a new one doesn't require touching the core backend, and
  installing someone else's is a copy-and-restart. See
  `backend/plugins/` and the companion
  [`sticky-assistant-plugin-hub`](https://github.com/biggy-spoon/sticky-assistant-plugin-hub)
  repo, a community index of plugins beyond these two defaults.
- **Runs on hardware you already have**: the backend is a small FastAPI
  service meant for a Raspberry Pi or a ZimaOS box you already leave on.

## How it works

```
reTerminal Sticky  <--WAV / text-->  backend (Pi or ZimaOS)  <--transcript / reply-->  Claude API
  (ESP32-S3 + ePaper)                (whisper STT + tool-use)                                │
                                             │                                                │
                                   create / list event  <─────────────────────────────────────┘
                                             │
                                             v
                                  your calendar (CalDAV or Google)
```

1. Hold the AI button on the device, speak, release.
2. The device records the clip and uploads it over WiFi to your backend.
3. The backend transcribes it locally (faster-whisper, no cloud STT),
   then sends the transcript to Claude — with calendar tools available
   if you've connected one.
4. If Claude needs to create or check a calendar event, the backend
   executes that for real and loops the result back to Claude for a
   natural-language reply.
5. The reply comes back to the device and renders on the e-paper screen.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the hardware
details and design notes behind this.

## Quickstart

**1. Backend** (on your Pi, ZimaOS box, or any always-on machine on your
home network). With Docker:

```bash
cp backend/.env.example backend/.env   # optional: API keys can be set on the device
docker compose up -d
```

Or with Python:

```bash
cd backend
cp .env.example .env   # fill in an AI provider's API key
pip install -r requirements.txt
uvicorn app:app --host 0.0.0.0 --port 8000
```

Once the device is set up (below), open its address in a browser (shown on its
Settings page) to connect a calendar and check status.

**2. Firmware.** The easiest way is the browser: each
[release](https://github.com/biggy-spoon/sticky-assistant/releases) has the
files for the [Seeed Sticky Playground](https://www.seeedstudio.com/sticky/playground/firmware/)
and any ESP web flasher (`manifest.json`, or `sticky-assistant-full.bin` at
`0x0`). Erase first if another firmware is on the device. To build it yourself
(requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html) v5.3.x):

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash
```

**3. First boot**: the screen shows the steps. The device opens a WiFi
network named `StickyAssistant-Setup`. Connect to it from your phone, a setup
page should open automatically (or browse to `http://192.168.4.1/`); enter
your real WiFi credentials and the backend's address
(`http://<that-machine>:8000/converse`). The device reboots and connects. If
the WiFi or the backend can't be reached, home says so and what to check. To
change the network later, hold Up and Down together for two seconds as the
device starts.

See `firmware/README.md` and `backend/README.md` for the full detail,
including known gaps and calendar provider setup.

## Project structure

- `firmware/` — ESP-IDF (C++) firmware: button, mic, WiFi, captive
  portal, LVGL UI, ePaper driver.
- `backend/` — FastAPI service: transcription, Claude calls, a plugin
  system (`backend/plugins/`; calendar and the todo list ship by
  default), and a SQLite-backed dashboard/history/settings.
- `docs/ARCHITECTURE.md` — hardware details and design notes.
- `playground/` — the entry for the Seeed Sticky Playground, and how to
  submit a release to it. `scripts/package_release.py` and
  `.github/workflows/release.yml` build the release files from a `v*` tag.

## Status

Built and verified end-to-end against real hardware: the full
record → transcribe → Claude (with calendar tool-use) → display loop
works on an actual device. See [`CHANGELOG.md`](CHANGELOG.md) for what's
landed and what's been fixed along the way (several real bugs only
surfaced by actually running this on hardware, not just compiling it).

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md). This project follows the
[Contributor Covenant](CODE_OF_CONDUCT.md).

## License

MIT (see [`LICENSE`](LICENSE)), firmware and backend alike. The e-paper
driver follows the MIT-licensed SSD1677 driver from Sticky OS, whose notice is
in `firmware/components/epaper_display/LICENSE-sticky-os`. The fonts are
under the SIL Open Font License.

## Acknowledgments

- [Sticky OS](https://github.com/Sticky-OS/sticky-os) (MIT) — the on-device
  interface follows its design, and the partial refresh, sleep sequence and
  battery pins are adapted from it, and the e-paper driver follows the SSD1677
  driver it vendors by limengdu (MIT, Copyright (c) 2026 The Sticky OS
  contributors).
- [Liberation Sans](https://github.com/liberationfonts/liberation-fonts) — the
  device's typeface, under the SIL Open Font License
  (`firmware/components/ui/fonts/OFL-LiberationSans.txt`).
- [`folloup-sticky`](https://github.com/alxv2016/folloup-sticky) by
  alxv2016 — the reference the board pin map was checked against.
- [Seeed Studio](https://www.seeedstudio.com/) for the reTerminal Sticky
  hardware.
