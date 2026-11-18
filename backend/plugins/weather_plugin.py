"""The weather plugin: a 7-day forecast on the device, with each day's hours
one tap away, and a get_weather_forecast tool so Claude can answer weather
questions by voice.

Forecasts come from Open-Meteo (https://open-meteo.com), which is free for
non-commercial use and needs no API key. The location is a place name, found
with Open-Meteo's geocoding, or "latitude,longitude".
"""
import logging
import threading
import time
from datetime import date, datetime
from typing import Optional

import requests

import settings
from .base import DeviceView, DeviceViewItem, Plugin, SettingField

log = logging.getLogger("sticky-assistant.plugins.weather")

_GEOCODE_URL = "https://geocoding-api.open-meteo.com/v1/search"
_FORECAST_URL = "https://api.open-meteo.com/v1/forecast"
_CACHE_SECONDS = 15 * 60  # the device asks every few minutes; forecasts change hourly
_TIMEOUT_SECONDS = 10

# WMO weather codes, as Open-Meteo reports them.
_CONDITIONS = {
    0: "Clear", 1: "Mostly clear", 2: "Partly cloudy", 3: "Cloudy",
    45: "Fog", 48: "Freezing fog",
    51: "Light drizzle", 53: "Drizzle", 55: "Heavy drizzle",
    56: "Freezing drizzle", 57: "Freezing drizzle",
    61: "Light rain", 63: "Rain", 65: "Heavy rain",
    66: "Freezing rain", 67: "Freezing rain",
    71: "Light snow", 73: "Snow", 75: "Heavy snow", 77: "Snow grains",
    80: "Showers", 81: "Showers", 82: "Heavy showers",
    85: "Snow showers", 86: "Heavy snow showers",
    95: "Thunderstorms", 96: "Thunderstorms, hail", 99: "Thunderstorms, hail",
}

_TOOLS = [
    {
        "name": "get_weather_forecast",
        "description": ("The weather forecast for the user's configured location: the next 7 "
                        "days, or one day's hours. Prefer this over web search for the weather "
                        "where the user is."),
        "input_schema": {
            "type": "object",
            "properties": {
                "date": {"type": "string",
                         "description": "Optional ISO date (e.g. 2026-11-19) for that day's hours"},
            },
        },
    },
]

_cache_lock = threading.Lock()
_cache: dict = {"key": None, "at": 0.0, "data": None}
_place_cache: dict = {"query": None, "place": None}


def _condition(code) -> str:
    return _CONDITIONS.get(int(code), "Unknown") if code is not None else "Unknown"


def _imperial() -> bool:
    return settings.get_units() == "imperial"


def locate(query: str) -> Optional[dict]:
    """Public for the settings plugin, which checks a location before saving it."""
    return _locate(query)


def _locate(query: str) -> Optional[dict]:
    """{"name", "latitude", "longitude"} for a place name or "lat,lon"."""
    query = query.strip()
    if _place_cache["query"] == query:
        return _place_cache["place"]
    place = None
    parts = [p.strip() for p in query.split(",")]
    try:
        if len(parts) == 2:
            place = {"name": query, "latitude": float(parts[0]), "longitude": float(parts[1])}
    except ValueError:
        place = None
    if place is None:
        # Open-Meteo matches the place name alone, so "Seattle, WA" searches "Seattle".
        response = requests.get(_GEOCODE_URL, params={"name": parts[0], "count": 5},
                                timeout=_TIMEOUT_SECONDS)
        response.raise_for_status()
        results = response.json().get("results") or []
        if len(parts) > 1:
            hint = parts[1].lower()
            preferred = [r for r in results
                         if hint in (r.get("admin1", "") + " " + r.get("country", "") + " "
                                     + r.get("country_code", "")).lower()
                         or hint == _US_STATES.get(r.get("admin1", ""), "").lower()]
            results = preferred or results
        if not results:
            return None
        found = results[0]
        place = {"name": found.get("name", query), "latitude": found["latitude"],
                 "longitude": found["longitude"]}
    _place_cache.update(query=query, place=place)
    return place


def _forecast() -> Optional[dict]:
    """The forecast for the configured location, cached for a few minutes."""
    location = settings.get("weather_location", "") or ""
    if not location:
        return None
    key = (location, _imperial())
    with _cache_lock:
        if _cache["key"] == key and time.time() - _cache["at"] < _CACHE_SECONDS:
            return _cache["data"]
    place = _locate(location)
    if place is None:
        raise ValueError(f"Couldn't find a place called {location!r}")
    params = {
        "latitude": place["latitude"], "longitude": place["longitude"],
        "timezone": "auto", "forecast_days": 7,
        "daily": "weather_code,temperature_2m_max,temperature_2m_min,"
                 "precipitation_probability_max,sunrise,sunset",
        "hourly": "temperature_2m,precipitation_probability,weather_code,wind_speed_10m",
        "current": "temperature_2m,weather_code,is_day",
    }
    if _imperial():
        params.update(temperature_unit="fahrenheit", wind_speed_unit="mph",
                      precipitation_unit="inch")
    response = requests.get(_FORECAST_URL, params=params, timeout=_TIMEOUT_SECONDS)
    response.raise_for_status()
    data = response.json()
    data["place"] = place["name"]
    with _cache_lock:
        _cache.update(key=key, at=time.time(), data=data)
    return data


def _days(data: dict) -> list[dict]:
    daily = data["daily"]
    days = []
    for i, day in enumerate(daily["time"]):
        days.append({
            "date": date.fromisoformat(day),
            "high": round(daily["temperature_2m_max"][i]),
            "low": round(daily["temperature_2m_min"][i]),
            "rain": daily["precipitation_probability_max"][i],
            "condition": _condition(daily["weather_code"][i]),
            "sunrise": datetime.fromisoformat(daily["sunrise"][i]),
            "sunset": datetime.fromisoformat(daily["sunset"][i]),
        })
    return days


def _hours(data: dict, day: date) -> list[dict]:
    hourly = data["hourly"]
    hours = []
    for i, stamp in enumerate(hourly["time"]):
        moment = datetime.fromisoformat(stamp)
        if moment.date() != day:
            continue
        hours.append({
            "time": moment,
            "temp": round(hourly["temperature_2m"][i]),
            "rain": hourly["precipitation_probability"][i],
            "condition": _condition(hourly["weather_code"][i]),
            "wind": round(hourly["wind_speed_10m"][i]),
        })
    return hours


def _clock(moment: datetime) -> str:
    return moment.strftime("%I %p").lstrip("0")


def _day_name(day: date, today: date) -> str:
    if day == today:
        return "Today"
    if (day - today).days == 1:
        return "Tomorrow"
    return day.strftime("%A")


def _hourly_text(data: dict, day: dict, now: datetime) -> str:
    wind_unit = "mph" if _imperial() else "km/h"
    lines = [
        f"{day['condition']}, high {day['high']}°, low {day['low']}°"
        + (f", {day['rain']}% chance of rain" if day["rain"] else ""),
        f"Sunrise {day['sunrise'].strftime('%I:%M %p').lstrip('0')}, "
        f"sunset {day['sunset'].strftime('%I:%M %p').lstrip('0')}",
        "",
    ]
    for hour in _hours(data, day["date"]):
        if hour["time"].date() == now.date() and hour["time"].hour < now.hour:
            continue  # today: from this hour on
        rain = f"  rain {hour['rain']}%" if hour["rain"] else ""
        lines.append(f"{_clock(hour['time']):>5}  {hour['temp']}°  {hour['condition']}"
                     f"{rain}  wind {hour['wind']} {wind_unit}")
    return "\n".join(lines)


def _epoch(local: datetime, data: dict) -> int:
    """Unix time of one of Open-Meteo's local wall-clock times."""
    return int((local - datetime(1970, 1, 1)).total_seconds()) - int(data.get("utc_offset_seconds", 0))


def _is_day(moment: datetime, day: Optional[dict]) -> bool:
    return day is None or day["sunrise"] <= moment < day["sunset"]


def _device_data(data: dict) -> dict:
    """What the device's weather screen draws: now, the next hours, and each
    day with its hours every three, all with WMO codes for the icons."""
    days = _days(data)
    by_date = {day["date"]: day for day in days}
    current = data.get("current") or {}
    now_local = datetime.fromisoformat(current["time"]) if "time" in current else datetime.now()
    hourly = data["hourly"]
    upcoming = []
    for i, stamp in enumerate(hourly["time"]):
        moment = datetime.fromisoformat(stamp)
        if moment < now_local.replace(minute=0, second=0, microsecond=0):
            continue
        upcoming.append({"ts": _epoch(moment, data), "temp": round(hourly["temperature_2m"][i]),
                         "code": int(hourly["weather_code"][i]),
                         "day": _is_day(moment, by_date.get(moment.date())),
                         "rain": hourly["precipitation_probability"][i] or 0})
        if len(upcoming) == 12:
            break
    out_days = []
    for day in days:
        hours = [h for h in _hours(data, day["date"]) if h["time"].hour % 3 == 0]
        out_days.append({
            "ts": _epoch(datetime(day["date"].year, day["date"].month, day["date"].day), data),
            "code": _code_for_day(data, day["date"]),
            "low": day["low"], "high": day["high"], "rain": day["rain"] or 0,
            "condition": day["condition"],
            "sunrise": _epoch(day["sunrise"], data), "sunset": _epoch(day["sunset"], data),
            "hours": [{"ts": _epoch(h["time"], data), "temp": h["temp"],
                       "code": _code_at(data, h["time"]), "day": _is_day(h["time"], day),
                       "rain": h["rain"] or 0, "wind": h["wind"]} for h in hours],
        })
    today = days[0] if days else None
    return {
        "place": data["place"],
        "unit": "F" if _imperial() else "C",
        "wind_unit": "mph" if _imperial() else "km/h",
        "current": {
            "temp": round(current.get("temperature_2m", 0)),
            "code": int(current.get("weather_code", 0)),
            "day": bool(current.get("is_day", 1)),
            "condition": _condition(current.get("weather_code")),
            "high": today["high"] if today else 0, "low": today["low"] if today else 0,
        },
        "hourly": upcoming,
        "days": out_days,
    }


def _code_for_day(data: dict, day: date) -> int:
    daily = data["daily"]
    return int(daily["weather_code"][daily["time"].index(day.isoformat())])


def _code_at(data: dict, moment: datetime) -> int:
    hourly = data["hourly"]
    return int(hourly["weather_code"][hourly["time"].index(moment.strftime("%Y-%m-%dT%H:%M"))])


def current_conditions() -> Optional[dict]:
    """Now, for the device's home screen: {"temp", "unit", "condition", "code",
    "is_day", "as_of"} with as_of the Unix time of the reading, or None."""
    try:
        data = _forecast()
    except Exception:
        log.exception("Failed to load current weather")
        return None
    if data is None or "current" not in data:
        return None
    current = data["current"]
    # Open-Meteo gives local wall-clock times plus the location's UTC offset.
    observed = datetime.fromisoformat(current["time"])
    as_of = int((observed - datetime(1970, 1, 1)).total_seconds()) - int(data.get("utc_offset_seconds", 0))
    return {
        "temp": round(current["temperature_2m"]),
        "unit": "F" if _imperial() else "C",
        "condition": _condition(current["weather_code"]),
        "code": int(current["weather_code"]),
        "is_day": bool(current.get("is_day", 1)),
        "as_of": as_of,
        "place": data["place"],
    }


class WeatherPlugin(Plugin):
    @property
    def id(self) -> str:
        return "weather"

    @property
    def name(self) -> str:
        return "Weather"

    @property
    def description(self) -> str:
        return "A 7-day forecast with hourly detail, from Open-Meteo (free, no API key)."

    def settings_schema(self) -> list[SettingField]:
        # Units are a core setting (Overview tab, or the device's Settings page).
        return [
            SettingField(key="weather_location", label="Location",
                         placeholder='A city, e.g. "Seattle, WA", or "47.61,-122.33"'),
        ]

    def is_configured(self) -> bool:
        return bool(settings.get("weather_location", ""))

    def get_tools(self) -> list[dict]:
        return _TOOLS

    def execute_tool(self, name: str, tool_input: dict) -> str:
        if name != "get_weather_forecast":
            raise NotImplementedError(f"weather plugin has no tool {name!r}")
        try:
            data = _forecast()
            if data is None:
                return "No weather location is set -- the user can add one in the dashboard."
            now = datetime.now(settings.local_timezone()).replace(tzinfo=None)
            days = _days(data)
            wanted = tool_input.get("date")
            if wanted:
                for day in days:
                    if day["date"].isoformat() == wanted:
                        return f"{data['place']}, {wanted}:\n" + _hourly_text(data, day, now)
                return f"No forecast for {wanted}: it covers {days[0]['date']} to {days[-1]['date']}."
            unit = "F" if _imperial() else "C"
            return f"{data['place']}, 7 days (°{unit}):\n" + "\n".join(
                f"{d['date'].isoformat()} ({_day_name(d['date'], now.date())}): {d['condition']}, "
                f"high {d['high']}, low {d['low']}, rain {d['rain']}%" for d in days)
        except Exception as exc:  # noqa: BLE001 -- surfaced to Claude as a tool result
            log.exception("Weather lookup failed")
            return f"Weather lookup failed: {exc}"

    def dashboard_view(self) -> dict:
        try:
            data = _forecast()
        except Exception as exc:  # noqa: BLE001 -- shown in the dashboard, not raised
            log.exception("Failed to load weather for dashboard view")
            return {"days": [], "error": str(exc)}
        if data is None:
            return {"days": []}
        return {"place": data["place"],
                "days": [{"date": d["date"].isoformat(), "condition": d["condition"],
                          "high": d["high"], "low": d["low"], "rain": d["rain"]}
                         for d in _days(data)]}

    def device_view(self) -> Optional[DeviceView]:
        try:
            data = _forecast()
        except Exception:
            log.exception("Failed to load weather for device view")
            return None
        if data is None:
            return None
        now = datetime.now(settings.local_timezone()).replace(tzinfo=None)
        items = []
        for i, day in enumerate(_days(data)):
            rain = f", rain {day['rain']}%" if day["rain"] else ""
            items.append(DeviceViewItem(
                id=str(i),
                text=f"{_day_name(day['date'], now.date())}: {day['high']}° / {day['low']}°",
                secondary=f"{day['date'].strftime('%a %b')} {day['date'].day}, "
                          f"{day['condition']}{rain}",
                detail=_hourly_text(data, day, now),
            ))
        return DeviceView(view_type="weather", title=f"Weather, {data['place']}", items=items,
                          data=_device_data(data))


# Open-Meteo reports US states by full name; this lets "Portland, OR" pick Oregon.
_US_STATES = {
    "Alabama": "AL", "Alaska": "AK", "Arizona": "AZ", "Arkansas": "AR", "California": "CA",
    "Colorado": "CO", "Connecticut": "CT", "Delaware": "DE", "Florida": "FL", "Georgia": "GA",
    "Hawaii": "HI", "Idaho": "ID", "Illinois": "IL", "Indiana": "IN", "Iowa": "IA",
    "Kansas": "KS", "Kentucky": "KY", "Louisiana": "LA", "Maine": "ME", "Maryland": "MD",
    "Massachusetts": "MA", "Michigan": "MI", "Minnesota": "MN", "Mississippi": "MS",
    "Missouri": "MO", "Montana": "MT", "Nebraska": "NE", "Nevada": "NV", "New Hampshire": "NH",
    "New Jersey": "NJ", "New Mexico": "NM", "New York": "NY", "North Carolina": "NC",
    "North Dakota": "ND", "Ohio": "OH", "Oklahoma": "OK", "Oregon": "OR", "Pennsylvania": "PA",
    "Rhode Island": "RI", "South Carolina": "SC", "South Dakota": "SD", "Tennessee": "TN",
    "Texas": "TX", "Utah": "UT", "Vermont": "VT", "Virginia": "VA", "Washington": "WA",
    "West Virginia": "WV", "Wisconsin": "WI", "Wyoming": "WY", "District of Columbia": "DC",
}
