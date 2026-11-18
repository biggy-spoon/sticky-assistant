"""Provider-agnostic calendar access. Two providers so this works for
either major account type without picking a side:

- CalDAV: covers iCloud, Fastmail, Nextcloud, and any other CalDAV server.
  iCloud needs an app-specific password (appleid.apple.com -> Sign-In and
  Security -> App-Specific Passwords), not your normal Apple ID password.
- Google Calendar: via OAuth (see the /api/calendar/google/* routes in
  app.py for the one-time authorization flow).

Which one is active is just a setting (settings.get_calendar_config()),
so the dashboard can switch providers without a code change.
"""
import logging
from abc import ABC, abstractmethod
from dataclasses import dataclass
from datetime import date, datetime
from typing import Optional

import settings

log = logging.getLogger("sticky-assistant.calendar")


@dataclass
class CalendarEvent:
    summary: str
    start: datetime
    end: datetime
    description: str = ""
    uid: str = ""  # the provider's id for it, for changing or deleting it later
    recurring: bool = False


def _google_time(value: dict):
    """An all-day event has only a "date", which must stay a date: parsed as a
    datetime it would look like an event at midnight."""
    if "dateTime" in value:
        return datetime.fromisoformat(value["dateTime"])
    return date.fromisoformat(value["date"])


class CalendarProvider(ABC):
    @abstractmethod
    def add_event(self, event: CalendarEvent) -> None:
        ...

    @abstractmethod
    def list_events(self, start: datetime, end: datetime) -> list[CalendarEvent]:
        ...

    @abstractmethod
    def update_event(self, uid: str, start: Optional[datetime] = None,
                     end: Optional[datetime] = None, summary: Optional[str] = None) -> CalendarEvent:
        """Changes an event by its uid. A new start with no new end keeps the
        event's length. Returns the event as it now is."""

    @abstractmethod
    def delete_event(self, uid: str) -> None:
        ...


def _moved_end(old_start, old_end, new_start, new_end):
    if new_end is not None:
        return new_end
    if new_start is not None:
        return new_start + (old_end - old_start)
    return old_end


class CalDavProvider(CalendarProvider):
    def __init__(self, url: str, username: str, password: str, calendar_name: str = ""):
        self._url = url
        self._username = username
        self._password = password
        self._calendar_name = calendar_name

    def _get_calendar(self):
        import caldav

        client = caldav.DAVClient(url=self._url, username=self._username,
                                  password=self._password)
        principal = client.principal()
        calendars = principal.calendars()
        if not calendars:
            raise RuntimeError("CalDAV account has no calendars")
        if self._calendar_name:
            for calendar in calendars:
                if calendar.name == self._calendar_name:
                    return calendar
            raise RuntimeError(f"No CalDAV calendar named {self._calendar_name!r}")
        return calendars[0]

    def add_event(self, event: CalendarEvent) -> None:
        from icalendar import Calendar as ICalendar
        from icalendar import Event as ICalEvent

        ical = ICalendar()
        ical.add("prodid", "-//sticky-assistant//EN")
        ical.add("version", "2.0")
        vevent = ICalEvent()
        vevent.add("summary", event.summary)
        vevent.add("dtstart", event.start)
        vevent.add("dtend", event.end)
        if event.description:
            vevent.add("description", event.description)
        ical.add_component(vevent)

        calendar = self._get_calendar()
        calendar.save_event(ical.to_ical().decode("utf-8"))

    def list_events(self, start: datetime, end: datetime) -> list[CalendarEvent]:
        calendar = self._get_calendar()
        results = calendar.date_search(start=start, end=end)
        events: list[CalendarEvent] = []
        for result in results:
            for component in result.icalendar_instance.walk("VEVENT"):
                events.append(CalendarEvent(
                    summary=str(component.get("summary", "")),
                    start=component.get("dtstart").dt,
                    end=component.get("dtend").dt,
                    description=str(component.get("description", "")),
                    uid=str(component.get("uid", "")),
                    recurring="rrule" in component or "recurrence-id" in component,
                ))
        return events

    def update_event(self, uid, start=None, end=None, summary=None):
        resource = self._get_calendar().get_event_by_uid(uid)
        component = resource.icalendar_component
        if "rrule" in component:
            raise ValueError("That's a repeating event, which can't be changed from here yet")
        old_start = component.get("dtstart").dt
        old_end = component.get("dtend").dt
        new_end = _moved_end(old_start, old_end, start, end)
        if start is not None:
            del component["dtstart"]
            component.add("dtstart", start)
        if new_end is not old_end:
            del component["dtend"]
            component.add("dtend", new_end)
        if summary:
            del component["summary"]
            component.add("summary", summary)
        resource.save()
        return CalendarEvent(summary=str(component.get("summary", "")),
                             start=component.get("dtstart").dt, end=component.get("dtend").dt,
                             uid=uid)

    def delete_event(self, uid):
        resource = self._get_calendar().get_event_by_uid(uid)
        if "rrule" in resource.icalendar_component:
            raise ValueError("That's a repeating event, which can't be deleted from here yet")
        resource.delete()


class GoogleCalendarProvider(CalendarProvider):
    def __init__(self, client_id: str, client_secret: str, refresh_token: str,
                 calendar_id: str = "primary"):
        self._client_id = client_id
        self._client_secret = client_secret
        self._refresh_token = refresh_token
        self._calendar_id = calendar_id or "primary"

    def _get_service(self):
        from google.oauth2.credentials import Credentials
        from googleapiclient.discovery import build

        creds = Credentials(
            token=None,
            refresh_token=self._refresh_token,
            client_id=self._client_id,
            client_secret=self._client_secret,
            token_uri="https://oauth2.googleapis.com/token",
        )
        return build("calendar", "v3", credentials=creds)

    def add_event(self, event: CalendarEvent) -> None:
        service = self._get_service()
        body = {
            "summary": event.summary,
            "description": event.description,
            "start": {"dateTime": event.start.isoformat()},
            "end": {"dateTime": event.end.isoformat()},
        }  # times reach here with a timezone (see calendar_plugin._parse_time)
        service.events().insert(calendarId=self._calendar_id, body=body).execute()

    def list_events(self, start: datetime, end: datetime) -> list[CalendarEvent]:
        service = self._get_service()
        response = service.events().list(
            calendarId=self._calendar_id,
            timeMin=start.isoformat(),
            timeMax=end.isoformat(),
            singleEvents=True,
            orderBy="startTime",
        ).execute()
        events: list[CalendarEvent] = []
        for item in response.get("items", []):
            events.append(CalendarEvent(
                summary=item.get("summary", ""),
                start=_google_time(item["start"]),
                end=_google_time(item["end"]),
                description=item.get("description", ""),
                uid=item.get("id", ""),
                recurring=bool(item.get("recurringEventId")),
            ))
        return events

    def update_event(self, uid, start=None, end=None, summary=None):
        service = self._get_service()
        item = service.events().get(calendarId=self._calendar_id, eventId=uid).execute()
        old_start = _google_time(item["start"])
        old_end = _google_time(item["end"])
        body = {}
        if start is not None:
            body["start"] = {"dateTime": start.isoformat()}
            body["end"] = {"dateTime": _moved_end(old_start, old_end, start, end).isoformat()}
        elif end is not None:
            body["end"] = {"dateTime": end.isoformat()}
        if summary:
            body["summary"] = summary
        item = service.events().patch(calendarId=self._calendar_id, eventId=uid, body=body).execute()
        return CalendarEvent(summary=item.get("summary", ""), start=_google_time(item["start"]),
                             end=_google_time(item["end"]), uid=uid)

    def delete_event(self, uid):
        self._get_service().events().delete(calendarId=self._calendar_id, eventId=uid).execute()


def get_provider() -> Optional[CalendarProvider]:
    config = settings.get_calendar_config()
    provider = config.get("calendar_provider", "none")

    if provider == "caldav":
        if not (config.get("caldav_url") and config.get("caldav_username")
                and config.get("caldav_password")):
            log.warning("CalDAV selected but not fully configured")
            return None
        return CalDavProvider(config["caldav_url"], config["caldav_username"],
                              config["caldav_password"], config.get("caldav_calendar_name", ""))

    if provider == "google":
        if not (config.get("google_client_id") and config.get("google_client_secret")
                and config.get("google_refresh_token")):
            log.warning("Google Calendar selected but not fully configured")
            return None
        return GoogleCalendarProvider(config["google_client_id"], config["google_client_secret"],
                                      config["google_refresh_token"],
                                      config.get("google_calendar_id", "primary"))

    return None
