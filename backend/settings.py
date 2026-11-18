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
