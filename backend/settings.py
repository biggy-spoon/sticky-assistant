"""Mutable runtime settings, backed by storage.py's settings table with
.env as the first-run fallback. Anthropic API key, Claude model, and
calendar config are hot-editable from the dashboard without a restart.
Whisper settings are not here -- the model is loaded once at startup, so
changing it requires a restart; those stay env-only (see app.py).
"""
import logging
import os
from datetime import datetime, tzinfo
from typing import Optional
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

import storage

_ENV_DEFAULTS = {
    "anthropic_api_key": "ANTHROPIC_API_KEY",
    "claude_model": "CLAUDE_MODEL",
    "gemini_api_key": "GEMINI_API_KEY",
    "openai_api_key": "OPENAI_API_KEY",
    "ai_provider": "AI_PROVIDER",
}

# Each provider keeps its own key and model, so switching back and forth keeps
# both set up. Defaults are a reasonable starting model for each.
_AI_KEYS = {
    "anthropic": ("anthropic_api_key", "claude_model", "claude-haiku-5-5"),
    "gemini": ("gemini_api_key", "gemini_model", "gemini-2.5-flash"),
    "openai": ("openai_api_key", "openai_model", "gpt-4.1-mini"),
}

_CALENDAR_KEYS = [
    "calendar_provider",  # "none" | "caldav" | "google"
    "caldav_url",
    "caldav_username",
    "caldav_password",
    "caldav_calendar_name",
    "google_client_id",
    "google_client_secret",
    "google_refresh_token",
    "google_calendar_id",
]


def get(key: str, default: Optional[str] = None) -> Optional[str]:
    stored = storage.get_setting(key)
    if stored is not None:
        return stored
    env_key = _ENV_DEFAULTS.get(key)
    if env_key is not None:
        return os.getenv(env_key, default)
    return default


def set(key: str, value: str) -> None:
    storage.set_setting(key, value)


def get_anthropic_api_key() -> str:
    return get("anthropic_api_key", "") or ""


def get_claude_model() -> str:
    return get("claude_model", "claude-haiku-5-5") or "claude-haiku-5-5"


def get_ai_provider() -> str:
    """"anthropic" (Claude), "gemini" or "openai" (OpenAI or any compatible API)."""
    value = get("ai_provider", "") or "anthropic"
    return value if value in _AI_KEYS else "anthropic"


def get_ai_api_key(provider: str) -> str:
    return get(_AI_KEYS[provider][0], "") or ""


def get_ai_model(provider: str) -> str:
    _, model_key, default = _AI_KEYS[provider]
    return get(model_key, "") or default


def set_ai_model(provider: str, model: str) -> None:
    set(_AI_KEYS[provider][1], model)


def set_ai_api_key(provider: str, key: str) -> None:
    set(_AI_KEYS[provider][0], key)


def get_openai_base_url() -> str:
    """For the "openai" provider: OpenAI itself, or e.g. http://localhost:11434/v1
    for Ollama or https://openrouter.ai/api/v1 for OpenRouter."""
    return get("openai_base_url", "") or "https://api.openai.com/v1"


def get_timezone_name() -> str:
    # "todo_timezone" is where this lived before it applied to the whole device.
    return get("timezone", "") or get("todo_timezone", "") or ""


def local_timezone() -> tzinfo:
    """The user's timezone, for the device's clock, its agenda and the todo
    list's daily reset. The server's own timezone if none is set or the name is
    unknown."""
    name = get_timezone_name()
    if name:
        try:
            return ZoneInfo(name)
        except (ZoneInfoNotFoundError, ValueError):
            logging.getLogger("sticky-claude").warning("Unknown timezone %r, using the server's", name)
    return datetime.now().astimezone().tzinfo


def get_units() -> str:
    """"imperial" (°F, mph) or "metric" (°C, km/h), for everything that shows a
    measurement. Falls back to the weather plugin's older setting."""
    value = get("units", "") or get("weather_units", "") or "imperial"
    return value if value in ("imperial", "metric") else "imperial"


def get_device_font() -> str:
    """"custom" uses a font built into the firmware with make_fonts.sh, when it
    has one; "default" uses the bundled Liberation Sans."""
    value = get("device_font", "") or "custom"
    return value if value in ("custom", "default") else "custom"


# Timezones the device's Settings page steps through, in order. Any other
# IANA name can be typed on the dashboard.
TIMEZONE_CHOICES = [
    "America/New_York", "America/Chicago", "America/Denver", "America/Phoenix",
    "America/Los_Angeles", "America/Anchorage", "Pacific/Honolulu", "UTC",
    "Europe/London", "Europe/Paris", "Europe/Berlin", "Asia/Kolkata", "Asia/Tokyo",
    "Australia/Sydney",
]


def get_calendar_config() -> dict:
    return {key: get(key, "") for key in _CALENDAR_KEYS}


def set_calendar_config(config: dict) -> None:
    for key in _CALENDAR_KEYS:
        if key in config:
            storage.set_setting(key, config[key])


def redact(config: dict) -> dict:
    """Mask secret-shaped values for display in the dashboard."""
    secret_keys = {"anthropic_api_key", "caldav_password", "google_client_secret",
                   "google_refresh_token"}
    redacted = dict(config)
    for key in secret_keys:
        value = redacted.get(key)
        if value:
            redacted[key] = f"...{value[-4:]}" if len(value) > 4 else "****"
    return redacted
