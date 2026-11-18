"""The Settings page: units, timezone, weather location and the device's font,
on the device itself, and a tool so they can be changed by voice. A default
plugin, like the calendar and the todo list.

Tapping a row steps the setting on (units and font toggle, the timezone moves
to the next of settings.TIMEZONE_CHOICES). The weather location needs typing,
which the device can't do, so its row explains how to change it instead.
"""
import logging
from typing import Optional
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

import settings
from .base import DeviceView, DeviceViewItem, Plugin

log = logging.getLogger("sticky-claude.plugins.settings")

# What the device that's asking can do, set by app.py per request: whether its
# firmware was built with a custom font.
device_has_custom_font = False

_UNITS = {"imperial": "Imperial: °F, mph", "metric": "Metric: °C, km/h"}
_FONTS = {"custom": "Your own font", "default": "Liberation Sans"}

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


def _timezone_label(name: str) -> str:
    if not name:
        return "This server's timezone"
    return name.split("/")[-1].replace("_", " ") + f" ({name})"


class SettingsPlugin(Plugin):
    @property
    def id(self) -> str:
        return "settings"

    @property
    def name(self) -> str:
        return "Settings"

    @property
    def description(self) -> str:
        return "Units, timezone, weather location and font, on the device and by voice."

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
            from . import weather_plugin
            try:
                place = weather_plugin.locate(location)
            except Exception as exc:  # noqa: BLE001 -- surfaced to Claude
                return f"Couldn't look up {location!r}: {exc}"
            if place is None:
                return f"Couldn't find a place called {location!r}. Nothing was changed."
            settings.set("weather_location", location)
            changed.append(f"weather location to {place['name']}")
        font = tool_input.get("font")
        if font in _FONTS:
            settings.set("device_font", font)
            changed.append(f"font to {_FONTS[font]}")
        if not changed:
            return "Nothing to change."
        return "Changed " + ", ".join(changed) + ". The device shows it within a few seconds."

    def device_view(self) -> Optional[DeviceView]:
        location = settings.get("weather_location", "") or ""
        items = [
            DeviceViewItem(id="units", text="Units", secondary=_UNITS[settings.get_units()],
                           action="tap"),
            DeviceViewItem(id="timezone", text="Timezone",
                           secondary=_timezone_label(settings.get_timezone_name()), action="tap"),
            DeviceViewItem(
                id="location", text="Weather location", secondary=location or "Not set",
                detail=("To change it, hold the top button and say, for example, \"Set my "
                        "weather location to Seattle, Washington\". Or type it on the "
                        "dashboard, under Plugins, Weather."),
            ),
        ]
        if device_has_custom_font:
            items.append(DeviceViewItem(id="font", text="Font",
                                        secondary=_FONTS[settings.get_device_font()],
                                        action="tap"))
        return DeviceView(view_type="list", title="Settings", items=items)

    def device_action(self, item_id: str, checked: Optional[bool]) -> None:
        if item_id == "units":
            settings.set("units", "metric" if settings.get_units() == "imperial" else "imperial")
        elif item_id == "font":
            settings.set("device_font",
                         "default" if settings.get_device_font() == "custom" else "custom")
        elif item_id == "timezone":
            choices = settings.TIMEZONE_CHOICES
            current = settings.get_timezone_name()
            index = choices.index(current) + 1 if current in choices else 0
            settings.set("timezone", choices[index % len(choices)])
        else:
            log.warning("device_action for unknown setting %r", item_id)
