"""Voice assistant proxy for the reTerminal Sticky, plus the API the device's
settings page uses.

Receives a WAV recording from the device, transcribes it locally with
faster-whisper, sends the transcript to the configured AI model (see
ai_providers.py; Claude by default), with whatever tools the
configured plugins contribute -- see plugins/ -- so it can actually act,
not just talk about acting), and returns the transcript + reply as JSON
for the device to render. The settings page (served by the device; see
firmware/components/settings_web) reads
conversation history, settings, device status, and each plugin's own
view over a small JSON API.
"""
import io
import logging
import os
import threading
import time
from datetime import datetime, timezone
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException, Request
from fastapi.concurrency import run_in_threadpool
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, RedirectResponse
from faster_whisper import WhisperModel

import ai_providers
import device_views
import plugin_hub
import plugins
import settings
import storage

load_dotenv()

logging.basicConfig(level=logging.INFO)
log = logging.getLogger("sticky-assistant")

# Whisper is loaded once at startup; changing these needs a restart.
WHISPER_MODEL_SIZE = os.getenv("WHISPER_MODEL_SIZE", "small")
WHISPER_DEVICE = os.getenv("WHISPER_DEVICE", "cpu")
WHISPER_COMPUTE_TYPE = os.getenv("WHISPER_COMPUTE_TYPE", "int8")
MAX_AUDIO_BYTES = 10 * 1024 * 1024  # generous ceiling; the device caps clips at ~10s

SYSTEM_PROMPT_TEMPLATE = (
    "You are a general-purpose voice assistant. The user can ask about anything, so don't "
    "assume a question is about you or the device you run on. Their words were transcribed "
    "from speech and may contain misheard words. Use web search for current events and for "
    "anything you aren't sure of, and give the answer itself rather than saying what you "
    "searched for. Your reply is shown as plain text on a small e-paper screen with no "
    "speaker, so keep it to 2-4 short sentences, with no markdown, lists or citations. "
    "Today is {today}. When a tool is available for what the user is asking, use "
    "it rather than just describing what you would do. Earlier messages in the conversation "
    "are the user's recent requests, so a short reply like \"yes\" answers your last question."
)

# Recent exchanges, so a follow-up ("yes, the 13th") makes sense. Only the text
# of each question and answer is kept, in memory, for a few minutes.
MEMORY_EXCHANGES = 3
MEMORY_SECONDS = 10 * 60
_memory: list[tuple[float, str, str]] = []
_memory_lock = threading.Lock()


def _recent_exchanges() -> list[dict]:
    cutoff = time.time() - MEMORY_SECONDS
    with _memory_lock:
        _memory[:] = [entry for entry in _memory if entry[0] >= cutoff][-MEMORY_EXCHANGES:]
        recent = list(_memory)
    messages: list[dict] = []
    for _, question, answer in recent:
        messages.append({"role": "user", "content": question})
        messages.append({"role": "assistant", "content": answer})
    return messages


def _remember(question: str, answer: str) -> None:
    with _memory_lock:
        _memory.append((time.time(), question, answer))

app = FastAPI(title="sticky-assistant backend")
whisper_model: WhisperModel | None = None


# The settings page is served by the device itself and calls this API from the
# browser, so cross-origin requests are allowed -- from home-network addresses
# only, the same trust model the backend has always had (no login, LAN only).
app.add_middleware(
    CORSMiddleware,
    allow_origin_regex=(r"^https?://(localhost|127\.0\.0\.1|10(\.\d{1,3}){3}|192\.168(\.\d{1,3}){2}"
                        r"|172\.(1[6-9]|2\d|3[01])(\.\d{1,3}){2})(:\d+)?$"),
    allow_methods=["*"],
    allow_headers=["*"],
)

for _plugin in plugins.all_plugins():
    _plugin.register_routes(app)
plugin_hub.route_registrar = lambda plugin: plugin.register_routes(app)


@app.on_event("startup")
def load_models() -> None:
    global whisper_model
    storage.init_db()
    device_views.start()
    log.info("Loading faster-whisper model=%s device=%s compute_type=%s",
              WHISPER_MODEL_SIZE, WHISPER_DEVICE, WHISPER_COMPUTE_TYPE)
    whisper_model = WhisperModel(WHISPER_MODEL_SIZE, device=WHISPER_DEVICE,
                                  compute_type=WHISPER_COMPUTE_TYPE)
    log.info("Startup complete")


@app.get("/health")
def health() -> dict:
    return {
        "status": "ok",
        "whisper_model": WHISPER_MODEL_SIZE,
        "ai_provider": settings.get_ai_provider(),
        "ai_model": settings.get_ai_model(settings.get_ai_provider()),
        "ai_configured": ai_providers.is_configured(),
    }


@app.post("/converse")
async def converse(request: Request) -> JSONResponse:
    if whisper_model is None:
        raise HTTPException(status_code=503, detail="Whisper model not loaded yet")
    if not ai_providers.is_configured():
        provider = ai_providers.PROVIDERS[settings.get_ai_provider()]
        raise HTTPException(status_code=500, detail=f"No API key set for {provider}")

    audio_bytes = await request.body()
    if not audio_bytes:
        raise HTTPException(status_code=400, detail="Empty request body")
    if len(audio_bytes) > MAX_AUDIO_BYTES:
        raise HTTPException(status_code=413, detail="Audio clip too large")

    if request.client is not None:
        storage.record_device_seen(request.client.host)

    # Transcribing and asking the model each take seconds, so they run on a
    # worker thread: the device and dashboard can still reach the backend.
    try:
        transcript = await run_in_threadpool(_transcribe, audio_bytes)
    except Exception as exc:
        log.exception("Transcription failed")
        raise HTTPException(status_code=400, detail=f"Could not process audio: {exc}") from exc
    if not transcript.strip():
        raise HTTPException(status_code=422, detail="Could not transcribe any speech")

    try:
        reply = await run_in_threadpool(_reply_to, transcript)
    except ai_providers.AIError as exc:
        log.exception("Model request failed")
        storage.log_conversation(transcript, None, success=False, error_message=str(exc))
        raise HTTPException(status_code=502, detail=str(exc)) from exc

    log.info("transcript=%r reply=%r", transcript, reply)
    storage.log_conversation(transcript, reply, success=True)
    _remember(transcript, reply)
    device_views.invalidate()  # the model may have changed a calendar, todos or settings
    return JSONResponse({"transcript": transcript, "reply": reply})


def _transcribe(wav_bytes: bytes) -> str:
    segments, _info = whisper_model.transcribe(io.BytesIO(wav_bytes), language="en")
    return " ".join(segment.text.strip() for segment in segments).strip()


def _collect_tools() -> tuple[list[dict], dict[str, str]]:
    """Tool definitions from every configured plugin, plus a name -> plugin
    id map so a tool_use block can be routed back to whichever plugin
    owns it."""
    tool_defs: list[dict] = []
    tool_owner: dict[str, str] = {}
    for plugin in plugins.all_plugins():
        if not plugin.is_configured():
            continue
        for tool in plugin.get_tools():
            tool_defs.append(tool)
            tool_owner[tool["name"]] = plugin.id
    return tool_defs, tool_owner


def _execute_tool(name: str, tool_input: dict, tool_owner: dict[str, str]) -> str:
    plugin = plugins.get(tool_owner.get(name, ""))
    if plugin is None:
        return f"Unknown tool: {name}"
    return plugin.execute_tool(name, tool_input)


def _system_prompt() -> str:
    now = datetime.now(settings.local_timezone())
    return SYSTEM_PROMPT_TEMPLATE.format(
        today=f"{now.strftime('%A, %B %d, %Y')}, and the time is {now.strftime('%I:%M %p').lstrip('0')}"
              f" ({settings.get_timezone_name() or 'server time'})")


def _reply_to(transcript: str) -> str:
    tool_defs, tool_owner = _collect_tools()
    return ai_providers.converse(
        _system_prompt(), _recent_exchanges(), transcript, tool_defs,
        lambda name, tool_input: _execute_tool(name, tool_input, tool_owner))


# --- Dashboard ---------------------------------------------------------

@app.get("/dashboard")
def dashboard():
    """The dashboard moved onto the device: send the browser there."""
    last_ip = storage.get_device_status().get("last_ip")
    if last_ip:
        return RedirectResponse(f"http://{last_ip}/")
    return JSONResponse({"detail": "The settings page is on the device: open its address "
                                   "(shown on its Settings page) in a browser."}, status_code=404)


@app.get("/api/history")
def api_history(limit: int = 50) -> list[dict]:
    return storage.get_history(limit=limit)


@app.get("/api/status")
def api_status() -> dict:
    status = storage.get_device_status()
    status["whisper_model"] = WHISPER_MODEL_SIZE
    provider = settings.get_ai_provider()
    status["ai_provider"] = ai_providers.PROVIDERS[provider]
    status["ai_model"] = settings.get_ai_model(provider)
    status["ai_configured"] = ai_providers.is_configured()
    return status


@app.get("/api/settings")
def api_get_settings() -> dict:
    return {
        "ai_provider": settings.get_ai_provider(),
        "ai_providers": ai_providers.PROVIDERS,
        # Per provider, so the form can show each one's key (masked) and model.
        "ai": {provider: {"api_key": settings.redact({"k": settings.get_ai_api_key(provider)})["k"],
                          "model": settings.get_ai_model(provider)}
               for provider in ai_providers.PROVIDERS},
        "openai_base_url": settings.get_openai_base_url(),
        "timezone": settings.get_timezone_name(),
        "units": settings.get_units(),
        "device_font": settings.get_device_font(),
    }


@app.get("/api/ai/models")
def api_ai_models(provider: str = "") -> dict:
    provider = provider or settings.get_ai_provider()
    if provider not in ai_providers.PROVIDERS:
        raise HTTPException(status_code=400, detail=f"Unknown AI provider: {provider}")
    try:
        return {"models": ai_providers.list_models(provider)}
    except Exception as exc:  # noqa: BLE001 -- shown in the dashboard
        return {"models": [settings.get_ai_model(provider)], "error": str(exc)}


@app.post("/api/settings")
async def api_set_settings(request: Request) -> dict:
    body = await request.json()
    provider = body.get("ai_provider")
    if provider is not None:
        if provider not in ai_providers.PROVIDERS:
            raise HTTPException(status_code=400, detail=f"Unknown AI provider: {provider}")
        settings.set("ai_provider", provider)
    provider = settings.get_ai_provider()
    if body.get("ai_api_key"):
        settings.set_ai_api_key(provider, body["ai_api_key"])
    if body.get("ai_model"):
        settings.set_ai_model(provider, str(body["ai_model"]).strip())
    if body.get("openai_base_url"):
        settings.set("openai_base_url", str(body["openai_base_url"]).strip())
    # The older Claude-only fields, still accepted.
    if body.get("anthropic_api_key"):
        settings.set("anthropic_api_key", body["anthropic_api_key"])
    if body.get("claude_model"):
        settings.set("claude_model", body["claude_model"])
    if "timezone" in body:
        name = str(body["timezone"]).strip()
        if name:
            try:
                ZoneInfo(name)
            except (ZoneInfoNotFoundError, ValueError) as exc:
                raise HTTPException(status_code=400, detail=f"Unknown timezone: {name}") from exc
        settings.set("timezone", name)
    if body.get("units") in ("imperial", "metric"):
        settings.set("units", body["units"])
    if body.get("device_font") in ("custom", "default"):
        settings.set("device_font", body["device_font"])
    return {"success": True}


@app.get("/api/plugins")
def api_plugins() -> list[dict]:
    result = []
    for plugin in plugins.all_plugins():
        fields = []
        for field in plugin.settings_schema():
            value = None
            if field.type != "button":
                value = settings.get(field.key, "")
                if field.type == "password" and value:
                    value = f"...{value[-4:]}" if len(value) > 4 else "****"
            fields.append({
                "key": field.key, "label": field.label, "type": field.type,
                "placeholder": field.placeholder, "options": field.options, "value": value,
                "button_label": field.button_label, "button_url": field.button_url,
                "show_if": list(field.show_if) if field.show_if else None,
            })
        result.append({
            "id": plugin.id, "name": plugin.name, "description": plugin.description,
            "configured": plugin.is_configured(), "fields": fields,
        })
    return result


@app.post("/api/plugins/{plugin_id}/settings")
async def api_set_plugin_settings(plugin_id: str, request: Request) -> dict:
    plugin = plugins.get(plugin_id)
    if plugin is None:
        raise HTTPException(status_code=404, detail=f"Unknown plugin: {plugin_id}")
    body = await request.json()
    field_keys = {field.key for field in plugin.settings_schema() if field.type != "button"}
    for key, value in body.items():
        # Skip blanks so an untouched password field doesn't overwrite a
        # previously-saved secret with an empty string.
        if key in field_keys and value:
            settings.set(key, value)
    device_views.invalidate()  # a setting can change any screen (e.g. the weather location)
    return {"success": True}


@app.get("/api/plugins/{plugin_id}/view")
def api_plugin_view(plugin_id: str) -> dict:
    plugin = plugins.get(plugin_id)
    if plugin is None:
        raise HTTPException(status_code=404, detail=f"Unknown plugin: {plugin_id}")
    return plugin.dashboard_view() or {}


@app.get("/api/plugin-hub")
def api_plugin_hub() -> dict:
    entries, error = plugin_hub.get_index()
    installed_ids = {plugin.id for plugin in plugins.all_plugins()}
    entries = [dict(entry) for entry in entries]  # the cache stays as fetched
    for entry in entries:
        entry["installed"] = entry.get("id") in installed_ids
        entry["installable"] = plugin_hub.installable(entry)
    return {"plugins": entries, "error": error}


@app.post("/api/plugin-hub/{plugin_id}/install")
def api_plugin_hub_install(plugin_id: str) -> dict:
    try:
        name = plugin_hub.install(plugin_id, lambda plugin: plugin.register_routes(app))
    except plugin_hub.InstallError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    return {"success": True, "name": name}


# --- Device screens (Up/Down button navigation on the ePaper panel) ----

@app.get("/api/device/views")
def api_device_views(fonts: str = "", font_name: str = "") -> dict:
    """Every configured plugin's own on-device screen, in a fixed order
    (plugin registration order), for the firmware to page through. The device's own home/conversation screen isn't
    included -- it's rendered locally by the firmware without a backend
    round-trip."""
    from plugins import settings_plugin
    settings_plugin.device_has_custom_font = "custom" in fonts.split(",")
    settings_plugin.device_custom_font_name = font_name.strip()[:40]
    # Settings goes last, after the plugins it configures. Screens come ready
    # made from device_views, so this answers at once.
    ordered = sorted((p for p in plugins.all_plugins() if p.is_configured()),
                     key=lambda plugin: plugin.id == "settings")
    views = [{"plugin_id": plugin.id, **view}
             for plugin, view in device_views.views(ordered) if view is not None]
    # The device has no clock of its own worth trusting, so it sets its time
    # from here, and shows it in the user's timezone.
    now = time.time()
    offset = datetime.fromtimestamp(now, settings.local_timezone()).utcoffset()
    result = {"views": views,
              "clock": {"now": int(now), "utc_offset": int(offset.total_seconds()) if offset else 0},
              "device": {"font": settings.get_device_font()},
              "assistant": {"name": ai_providers.assistant_name(),
                            "model": settings.get_ai_model(settings.get_ai_provider())}}
    weather = plugins.get("weather")
    if weather is not None and weather.is_configured():
        from plugins import weather_plugin
        current = weather_plugin.current_conditions()
        if current is not None:
            result["weather"] = current  # the home screen's "now" corner
    return result


@app.post("/api/plugins/{plugin_id}/device-action")
async def api_plugin_device_action(plugin_id: str, request: Request) -> dict:
    plugin = plugins.get(plugin_id)
    if plugin is None:
        raise HTTPException(status_code=404, detail=f"Unknown plugin: {plugin_id}")
    body = await request.json()
    item_id = body.get("item_id")
    if not item_id:
        raise HTTPException(status_code=400, detail="item_id is required")
    if "value" in body:  # a pick from a list, or typed text
        plugin.device_input(str(item_id), str(body["value"]))
        device_views.invalidate(plugin.id)
        return {"success": True}
    # Checkable rows send the state they should now have; tap rows send none.
    checked = body.get("checked")
    if checked is not None and not isinstance(checked, bool):
        raise HTTPException(status_code=400, detail="checked must be true, false or absent")
    plugin.device_action(str(item_id), checked)
    device_views.invalidate(plugin.id)
    return {"success": True}
