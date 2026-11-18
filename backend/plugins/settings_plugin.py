"""The Settings page: the AI provider and model, units, timezone, weather
location and the device's font, on the device itself, and a tool so they can be
changed by voice. A default plugin, like the calendar and the todo list.

Each row opens its own screen on the device: a list to pick from, or a
keyboard for the weather location.
"""
import logging
from typing import Optional
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

import ai_providers
import settings
from .base import DeviceView, DeviceViewItem, Plugin

log = logging.getLogger("sticky-claude.plugins.settings")

# What the device that's asking can do, set by app.py per request: whether its
# firmware was built with a custom font, and that font's name.
device_has_custom_font = False
device_custom_font_name = ""

_MAX_MODELS = 20  # a long list is slow to page through on e-paper

_UNITS = {"imperial": "Imperial: °F, mph", "metric": "Metric: °C, km/h"}

_TOOLS = [
    {
        "name": "update_settings",
        "description": ("Change the device's settings: units (imperial or metric), timezone "
                        "(an IANA name such as America/Chicago), the weather location (a city, "
                        "e.g. 'Seattle, WA'), or font ('custom' for the user's own font, "
                        "'default' for Liberation Sans). Give only what should change."),
        "input_schema": {
            "type": "object",
            "properties": {
                "units": {"type": "string", "enum": ["imperial", "metric"]},
                "timezone": {"type": "string"},
                "weather_location": {"type": "string"},
                "font": {"type": "string", "enum": ["custom", "default"]},
            },
        },
    },
]


def _fonts() -> dict:
    return {"custom": device_custom_font_name or "Your own font", "default": "Liberation Sans"}


def _timezone_label(name: str) -> str:
    if not name:
        return "This server's timezone"
    return name.split("/")[-1].replace("_", " ") + f" ({name})"


def _options(pairs) -> list[dict]:
    return [{"value": value, "label": label} for value, label in pairs]


def _set_weather_location(location: str) -> str:
    """Checks the place exists first. Returns what to tell the user."""
    from . import weather_plugin
    try:
        place = weather_plugin.locate(location)
    except Exception as exc:  # noqa: BLE001 -- surfaced to the user
        return f"Couldn't look up {location!r}: {exc}"
    if place is None:
        return f"Couldn't find a place called {location!r}. Nothing was changed."
    settings.set("weather_location", location)
    return f"weather location to {place['name']}"


class SettingsPlugin(Plugin):
    @property
    def id(self) -> str:
        return "settings"

    @property
    def name(self) -> str:
        return "Settings"

    @property
    def description(self) -> str:
        return "The AI, units, timezone, weather location and font, on the device and by voice."

    def is_configured(self) -> bool:
        return True

    def get_tools(self) -> list[dict]:
        return _TOOLS

    def execute_tool(self, name: str, tool_input: dict) -> str:
        if name != "update_settings":
            raise NotImplementedError(f"settings plugin has no tool {name!r}")
        changed = []
        units = tool_input.get("units")
        if units in _UNITS:
            settings.set("units", units)
            changed.append(f"units to {units}")
        zone = (tool_input.get("timezone") or "").strip()
        if zone:
            try:
                ZoneInfo(zone)
            except (ZoneInfoNotFoundError, ValueError):
                return f"{zone!r} isn't a timezone name. Nothing was changed."
            settings.set("timezone", zone)
            changed.append(f"timezone to {zone}")
        location = (tool_input.get("weather_location") or "").strip()
        if location:
            result = _set_weather_location(location)
            if not result.startswith("weather location"):
                return result
            changed.append(result)
        font = tool_input.get("font")
        if font in ("custom", "default"):
            settings.set("device_font", font)
            changed.append(f"font to {_fonts()[font]}")
        if not changed:
            return "Nothing to change."
        return "Changed " + ", ".join(changed) + ". The device shows it within a few seconds."

    def device_view(self) -> Optional[DeviceView]:
        provider = settings.get_ai_provider()
        model = settings.get_ai_model(provider)
        try:
            models = ai_providers.list_models(provider)[:_MAX_MODELS]
        except Exception as exc:  # noqa: BLE001 -- the current model is still shown
            log.warning("Couldn't list models for the settings page: %s", exc)
            models = [model]
        if model not in models:
            models.insert(0, model)

        zones = list(settings.TIMEZONE_CHOICES)
        zone = settings.get_timezone_name()
        if zone and zone not in zones:
            zones.insert(0, zone)
        location = settings.get("weather_location", "") or ""

        items = [
            DeviceViewItem(id="ai_provider", text="AI", secondary=ai_providers.PROVIDERS[provider],
                           action="choose", value=provider,
                           options=_options(ai_providers.PROVIDERS.items())),
            DeviceViewItem(id="ai_model", text="Model", secondary=model, action="choose",
                           value=model, options=_options((m, m) for m in models)),
            DeviceViewItem(id="units", text="Units", secondary=_UNITS[settings.get_units()],
                           action="choose", value=settings.get_units(),
                           options=_options(_UNITS.items())),
            DeviceViewItem(id="timezone", text="Timezone", secondary=_timezone_label(zone),
                           action="choose", value=zone,
                           options=_options((z, _timezone_label(z)) for z in zones)),
            DeviceViewItem(id="weather_location", text="Weather location",
                           secondary=location or "Not set", action="text", value=location),
        ]
        if device_has_custom_font:
            items.append(DeviceViewItem(id="font", text="Font",
                                        secondary=_fonts()[settings.get_device_font()],
                                        action="choose", value=settings.get_device_font(),
                                        options=_options(_fonts().items())))
        return DeviceView(view_type="list", title="Settings", items=items)

    def device_input(self, item_id: str, value: str) -> None:
        value = value.strip()
        if item_id == "ai_provider" and value in ai_providers.PROVIDERS:
            settings.set("ai_provider", value)
        elif item_id == "ai_model" and value:
            settings.set_ai_model(settings.get_ai_provider(), value)
        elif item_id == "units" and value in _UNITS:
            settings.set("units", value)
        elif item_id == "timezone" and value:
            try:
                ZoneInfo(value)
            except (ZoneInfoNotFoundError, ValueError):
                log.warning("device picked an unknown timezone %r", value)
                return
            settings.set("timezone", value)
        elif item_id == "weather_location" and value:
            log.info("weather location from the device: %s", _set_weather_location(value))
        elif item_id == "font" and value in ("custom", "default"):
            settings.set("device_font", value)
        else:
            log.warning("device_input for %r with %r ignored", item_id, value)
