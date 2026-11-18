"""The calendar plugin: create_calendar_event / list_calendar_events tools
backed by calendar_service.py's CalDAV/Google providers. This is the
reference implementation of plugins.base.Plugin -- a new plugin can
follow this file's shape.
"""
import logging
from datetime import datetime, timedelta, timezone
from typing import Optional

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import RedirectResponse

import calendar_service
import settings
from .base import DeviceView, DeviceViewItem, Plugin, SettingField

log = logging.getLogger("sticky-assistant.plugins.calendar")

# Device screens are small and glanceable -- cap how many upcoming events
# get sent down for the on-device list rather than however many the
# calendar has in the next 30 days.
_DEVICE_VIEW_MAX_EVENTS = 15
_DEVICE_DETAIL_MAX_CHARS = 600


def _clock(value: datetime) -> str:
    return value.strftime("%I:%M %p").lstrip("0")


def _row_when(event: calendar_service.CalendarEvent) -> str:
    # CalDAV all-day events carry plain dates, which have no time of day.
    if not isinstance(event.start, datetime):
        return event.start.strftime("%a %b %d") + ", all day"
    return f"{event.start.strftime('%a %b %d')}, {_clock(event.start)}"


def _timestamp(value, tz) -> int:
    """Unix time of an event boundary. A date (all day) means local midnight,
    and a time with no zone is taken as local too."""
    if not isinstance(value, datetime):
        value = datetime(value.year, value.month, value.day)
    if value.tzinfo is None:
        value = value.replace(tzinfo=tz)
    return int(value.timestamp())


def _detail(event: calendar_service.CalendarEvent) -> str:
    if isinstance(event.start, datetime):
        lines = [event.start.strftime("%A, %B %d"), f"{_clock(event.start)} - {_clock(event.end)}"]
    else:
        lines = [event.start.strftime("%A, %B %d"), "All day"]
    description = event.description.strip()
    if description:
        lines += ["", description[:_DEVICE_DETAIL_MAX_CHARS]]
    return "\n".join(lines)

_TOOLS = [
    {
        "name": "create_calendar_event",
        "description": "Add an event to the user's calendar.",
        "input_schema": {
            "type": "object",
            "properties": {
                "summary": {"type": "string", "description": "Short event title"},
                "start_time": {"type": "string",
                               "description": "ISO 8601 datetime, e.g. 2026-11-24T15:00:00"},
                "end_time": {"type": "string",
                             "description": "ISO 8601 datetime; defaults to 1 hour after "
                                            "start_time if omitted"},
                "description": {"type": "string", "description": "Optional extra detail"},
            },
            "required": ["summary", "start_time"],
        },
    },
    {
        "name": "list_calendar_events",
        "description": ("List the user's calendar events within a date range. Each line starts "
                        "with the event's id in brackets, which update_calendar_event and "
                        "delete_calendar_event need."),
        "input_schema": {
            "type": "object",
            "properties": {
                "start_date": {"type": "string", "description": "ISO 8601 date, e.g. 2026-11-24"},
                "end_date": {"type": "string", "description": "ISO 8601 date"},
            },
            "required": ["start_date", "end_date"],
        },
    },
    {
        "name": "update_calendar_event",
        "description": ("Move, retime or rename an existing calendar event. Find its id with "
                        "list_calendar_events first. Given only a new start, the event keeps "
                        "its length."),
        "input_schema": {
            "type": "object",
            "properties": {
                "event_id": {"type": "string", "description": "The id from list_calendar_events"},
                "start_time": {"type": "string",
                               "description": "New start, ISO 8601 datetime, e.g. 2027-01-13T09:20:00"},
                "end_time": {"type": "string", "description": "New end, ISO 8601 datetime"},
                "summary": {"type": "string", "description": "New title"},
            },
            "required": ["event_id"],
        },
    },
    {
        "name": "delete_calendar_event",
        "description": ("Delete an event from the user's calendar. Find its id with "
                        "list_calendar_events first, and only delete what the user asked to."),
        "input_schema": {
            "type": "object",
            "properties": {
                "event_id": {"type": "string", "description": "The id from list_calendar_events"},
            },
            "required": ["event_id"],
        },
    },
]


def _parse_time(value: str) -> datetime:
    """A time from Claude, which usually has no timezone: read it as the user's."""
    parsed = datetime.fromisoformat(value)
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=settings.local_timezone())
    return parsed


def _describe(event: calendar_service.CalendarEvent) -> str:
    start = event.start
    if isinstance(start, datetime) and start.tzinfo is not None:
        start = start.astimezone(settings.local_timezone())
    when = start.isoformat() if isinstance(start, datetime) else f"{start.isoformat()} (all day)"
    repeats = " (repeating)" if event.recurring else ""
    return f"[{event.uid}] {when}: {event.summary}{repeats}"


class CalendarPlugin(Plugin):
    @property
    def id(self) -> str:
        return "calendar"

    @property
    def name(self) -> str:
        return "Calendar"

    @property
    def description(self) -> str:
        return ("Create and check events by voice, via CalDAV (iCloud, Fastmail, Nextcloud, "
                "etc.) or Google Calendar.")

    def settings_schema(self) -> list[SettingField]:
        return [
            SettingField(
                key="calendar_provider", label="Calendar", type="select",
                options=[
                    {"value": "none", "label": "None"},
                    {"value": "caldav", "label": "CalDAV (iCloud, Fastmail, Nextcloud...)"},
                    {"value": "google", "label": "Google Calendar"},
                ],
            ),
            SettingField(key="caldav_url", label="CalDAV URL",
                        placeholder="https://caldav.icloud.com",
                        show_if=("calendar_provider", "caldav")),
            SettingField(key="caldav_username", label="Username",
                        placeholder="you@icloud.com", show_if=("calendar_provider", "caldav")),
            SettingField(key="caldav_password", label="App-specific password", type="password",
                        show_if=("calendar_provider", "caldav")),
            SettingField(key="caldav_calendar_name", label="Calendar name (optional)",
                        placeholder="Leave blank to use the first calendar",
                        show_if=("calendar_provider", "caldav")),
            SettingField(key="google_client_id", label="Google OAuth client ID",
                        show_if=("calendar_provider", "google")),
            SettingField(key="google_client_secret", label="Google OAuth client secret",
                        type="password", show_if=("calendar_provider", "google")),
            SettingField(key="google_calendar_id", label="Calendar ID (optional)",
                        placeholder="primary", show_if=("calendar_provider", "google")),
            SettingField(key="google_connect", label="", type="button",
                        button_label="Connect with Google",
                        button_url="/api/plugins/calendar/google/login",
                        show_if=("calendar_provider", "google")),
        ]

    def is_configured(self) -> bool:
        return calendar_service.get_provider() is not None

    def get_tools(self) -> list[dict]:
        return _TOOLS

    def execute_tool(self, name: str, tool_input: dict) -> str:
        provider = calendar_service.get_provider()
        if provider is None:
            return "Calendar is not configured yet -- tell the user to set it up in the dashboard."

        try:
            if name == "create_calendar_event":
                start = _parse_time(tool_input["start_time"])
                end_raw = tool_input.get("end_time")
                end = _parse_time(end_raw) if end_raw else start + timedelta(hours=1)
                provider.add_event(calendar_service.CalendarEvent(
                    summary=tool_input["summary"], start=start, end=end,
                    description=tool_input.get("description", ""),
                ))
                return f"Created '{tool_input['summary']}' at {start.isoformat()}."

            if name == "list_calendar_events":
                start = _parse_time(tool_input["start_date"])
                end = _parse_time(tool_input["end_date"])
                if end <= start:
                    end = start + timedelta(days=1)
                events = provider.list_events(start, end)
                if not events:
                    return "No events found in that range."
                return "\n".join(_describe(e) for e in events)

            if name == "update_calendar_event":
                start_raw = tool_input.get("start_time")
                end_raw = tool_input.get("end_time")
                event = provider.update_event(
                    tool_input["event_id"],
                    start=_parse_time(start_raw) if start_raw else None,
                    end=_parse_time(end_raw) if end_raw else None,
                    summary=tool_input.get("summary") or None,
                )
                return f"Updated: {_describe(event)}"

            if name == "delete_calendar_event":
                provider.delete_event(tool_input["event_id"])
                return f"Deleted event {tool_input['event_id']}."

            raise NotImplementedError(f"calendar plugin has no tool {name!r}")
        except Exception as exc:  # noqa: BLE001 -- surfaced to Claude as a tool result
            log.exception("Calendar tool %s failed", name)
            return f"Calendar action failed: {exc}"

    def dashboard_view(self) -> dict:
        provider = calendar_service.get_provider()
        if provider is None:
            return {"events": []}
        try:
            now = datetime.now(timezone.utc)
            events = provider.list_events(now, now + timedelta(days=30))
        except Exception as exc:  # noqa: BLE001 -- shown in the dashboard, not raised
            log.exception("Failed to load calendar events for dashboard view")
            return {"events": [], "error": str(exc)}
        return {"events": [{"summary": e.summary, "start": e.start.isoformat(),
                            "end": e.end.isoformat(), "description": e.description}
                           for e in events]}

    def device_view(self) -> Optional[DeviceView]:
        provider = calendar_service.get_provider()
        if provider is None:
            return None
        try:
            now = datetime.now(timezone.utc)
            events = provider.list_events(now, now + timedelta(days=30))
        except Exception:
            log.exception("Failed to load calendar events for device view")
            return None
        tz = settings.local_timezone()
        items = [
            DeviceViewItem(id=str(i), text=e.summary, secondary=_row_when(e), detail=_detail(e),
                           start_ts=_timestamp(e.start, tz), end_ts=_timestamp(e.end, tz),
                           all_day=not isinstance(e.start, datetime))
            for i, e in enumerate(events[:_DEVICE_VIEW_MAX_EVENTS])
        ]
        return DeviceView(view_type="agenda", title="Calendar", items=items)

    def register_routes(self, app: FastAPI) -> None:
        @app.get("/api/plugins/calendar/google/login")
        def google_login(request: Request) -> RedirectResponse:
            config = settings.get_calendar_config()
            client_id = config.get("google_client_id")
            if not client_id:
                raise HTTPException(status_code=400, detail="Set Google client ID/secret first")
            redirect_uri = str(request.base_url) + "api/plugins/calendar/google/callback"
            params = {
                "client_id": client_id,
                "redirect_uri": redirect_uri,
                "response_type": "code",
                "scope": "https://www.googleapis.com/auth/calendar",
                "access_type": "offline",
                "prompt": "consent",
            }
            query = "&".join(f"{key}={value}" for key, value in params.items())
            return RedirectResponse(f"https://accounts.google.com/o/oauth2/v2/auth?{query}")

        @app.get("/api/plugins/calendar/google/callback")
        def google_callback(request: Request, code: str) -> dict:
            import requests

            config = settings.get_calendar_config()
            redirect_uri = str(request.base_url) + "api/plugins/calendar/google/callback"
            response = requests.post("https://oauth2.googleapis.com/token", data={
                "client_id": config.get("google_client_id"),
                "client_secret": config.get("google_client_secret"),
                "code": code,
                "grant_type": "authorization_code",
                "redirect_uri": redirect_uri,
            }, timeout=15)
            response.raise_for_status()
            token_data = response.json()
            refresh_token = token_data.get("refresh_token")
            if not refresh_token:
                raise HTTPException(status_code=400,
                                    detail="Google did not return a refresh token -- revoke "
                                           "prior access in your Google Account and try again")
            settings.set("google_refresh_token", refresh_token)
            settings.set("calendar_provider", "google")
            return {"success": True, "message": "Google Calendar connected. You can close this tab."}
