# sticky-assistant backend

Small FastAPI service meant to run on an always-on home server (Pi or ZimaOS).
It is the proxy the reTerminal Sticky firmware talks to: the device uploads a
WAV recording, this service transcribes it locally with
[faster-whisper](https://github.com/SYSTRAN/faster-whisper), sends the
transcript to the AI model set on the dashboard (Claude by default, or Gemini,
OpenAI or any OpenAI-compatible API), and returns `{ "transcript": ..., "reply": ... }`
for the device to display.

## Run locally

```bash
cd backend
cp .env.example .env   # fill in ANTHROPIC_API_KEY
pip install -r requirements.txt
uvicorn app:app --host 0.0.0.0 --port 8000
```

First request will download the `small` Whisper model (~500MB); switch
`WHISPER_MODEL_SIZE` to `tiny` or `base` in `.env` if the Pi is slow or short
on RAM.

## Run with Docker

From the repository root:

```bash
cp backend/.env.example backend/.env   # optional: API keys can also be set on the device
docker compose up -d
```

Then set the device's backend to `http://<this computer's IP>:8000/converse`
(the device's setup network asks for it on first start). Everything worth
keeping lives in the `sticky-assistant-data` volume, mounted at `/data`: the
database, plugins installed from the hub (`PLUGINS_DIR`) and the speech model
(`HF_HOME`), so `docker compose up -d --build` after a `git pull` keeps them.

## Settings page

The settings page is served by the device, not the backend: open the
Sticky's address in a browser (its Settings page shows it, and
`http://<backend-host>:8000/dashboard` redirects there). The page calls this
backend's `/api/*` routes directly from the browser, which is why the backend
allows cross-origin requests, but only from home-network addresses (10.x,
172.16-31.x, 192.168.x, localhost). It shows recent conversations, status,
the AI provider and keys, each plugin's settings and data view, and the plugin
hub. None of this needs touching `.env` or restarting (the Whisper settings
are the exception: they load once at startup).

**There is no authentication on the API.** It's meant for your home LAN,
same trust model as the device's own setup portal. Don't port-forward it to
the public internet without putting something in front of it.

### Installing plugins from the hub

A plugin is code that runs on this backend, so the settings page and the
device only install hub entries marked `"reviewed": true` with a
`download_url` to a single `.py` file on raw.githubusercontent.com, pinned to
a commit so what was reviewed is what gets installed. The file
is checked (size, name), saved to `plugins/`, and loaded without a restart.
Anything else is still installed by hand.

## Plugins

Claude's capabilities beyond plain conversation — calendar access, a
todo list — come from plugins (`plugins/`). A plugin declares the Claude
tools it contributes, the settings fields it needs (rendered generically
by the dashboard — a plugin with only text/password fields needs zero
frontend changes), and optionally extra HTTP routes for its own setup
flow (calendar's Google OAuth callback, for example).

Every `.py` file in `plugins/` is scanned at import time for a
`plugins.base.Plugin` subclass and registered automatically — that's the
whole install story, for a built-in or a third-party plugin alike. **To
install a community plugin**, copy its file(s) into `plugins/` and
restart the backend; nothing else to edit. The dashboard's "Plugin hub"
card lists what's available from
[sticky-assistant-plugin-hub](https://github.com/biggy-spoon/sticky-assistant-plugin-hub)
(a metadata index only — the backend never fetches or runs a plugin's
code on its own, so review a plugin before dropping it in).

See `plugins/base.py` for the contract, and `plugins/calendar_plugin.py`
/ `plugins/todo_plugin.py` for the two reference implementations (one
with external-service settings, one with none). Want to publish your
own? See that repo's `CONTRIBUTING.md`.

### Calendar plugin setup

Pick one in the dashboard's Calendar card:

**CalDAV** (iCloud, Fastmail, Nextcloud, or any other CalDAV server):
1. For iCloud specifically: go to [appleid.apple.com](https://appleid.apple.com) →
   Sign-In and Security → App-Specific Passwords, and generate one. Don't
   use your real Apple ID password.
2. In the dashboard, set CalDAV URL to `https://caldav.icloud.com` (or your
   provider's CalDAV endpoint), Username to your account email, and
   Password to the app-specific password.
3. Leave "Calendar name" blank to use the account's first calendar, or
   enter one to target a specific calendar.

**Google Calendar**:
1. In the [Google Cloud Console](https://console.cloud.google.com/), create
   a project (or use an existing one), enable the Calendar API, and create
   an OAuth 2.0 Client ID (type: Web application). Add
   `http://<backend-host>:8000/api/plugins/calendar/google/callback` as
   an authorized redirect URI.
2. In the dashboard, enter the client ID and secret, save, then click
   "Connect with Google" and sign in. That exchanges a one-time code for a
   refresh token, which gets stored so the backend can call the Calendar
   API without you signing in again.

Once either is configured, Claude gets `create_calendar_event` and
`list_calendar_events` tools automatically — ask the device to add or
check something on your calendar and it'll actually do it, not just talk
about it.

## API

- `GET /health` — model/config status.
- `POST /converse` — body is raw WAV bytes (16-bit PCM mono, see firmware's
  `recording_service`). Returns JSON `{"transcript": "...", "reply": "..."}`.
- `GET /dashboard` — redirects to the settings page on the device.
- `POST /api/plugin-hub/{id}/install` — installs a reviewed hub plugin.
- `GET /api/history`, `GET /api/status`, `GET`/`POST /api/settings` —
  core data, also usable directly.
- `GET /api/plugins` — every plugin's id/name/description/configured
  state and settings schema (what the dashboard renders its forms from).
- `POST /api/plugins/{id}/settings` — save a plugin's settings.
- `GET /api/plugins/{id}/view` — a plugin's dashboard data, if it has one
  (calendar returns `{"events": [...]}`, todo returns
  `{"open": [...], "done": [...]}`).
- `GET /api/plugin-hub` — the community plugin index (name, description,
  repo URL, install state), proxied and cached from
  `sticky-assistant-plugin-hub`.
- Plugins may add their own routes too — calendar adds
  `GET /api/plugins/calendar/google/login` and `/callback` for its OAuth
  flow.

## Sizing note

`faster-whisper` on CPU is the main latency cost. On a Raspberry Pi 4/5,
`small` + `int8` is usually a few seconds for a 5-10s clip; ZimaOS hardware
with more cores will be faster. Tune `WHISPER_MODEL_SIZE` for your box.
