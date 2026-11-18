"""Each plugin's device screen, kept ready so the device gets an answer at once.

Building a screen can be slow -- the calendar asks a CalDAV server every time
(seconds), the weather looks up a forecast -- and the device waits for all of
them, including straight after waking. So screens are built in the background
and served from here: a copy younger than FRESH_SECONDS as is, an older one as
is too while a new one is built, and only a plugin seen for the first time
makes a request wait.

Something that changes a plugin's data (a tap on the device, its settings, a
conversation) calls invalidate() so the next request rebuilds it.
"""
import logging
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from typing import Optional

import plugins
from plugins.base import Plugin

log = logging.getLogger("sticky-assistant.device_views")

FRESH_SECONDS = 120
REFRESH_EVERY_SECONDS = 90
# Built per request: it depends on what the asking device can do (its fonts).
_LIVE = {"settings"}

_lock = threading.Lock()
_cache: dict[str, tuple[float, Optional[dict]]] = {}
_building: set[str] = set()
_pool = ThreadPoolExecutor(max_workers=4, thread_name_prefix="device-views")


def serialize(view) -> Optional[dict]:
    if view is None:
        return None
    return {
        "view_type": view.view_type,
        "data": view.data,
        "title": view.title,
        "body": view.body,
        "items": [{"id": item.id, "text": item.text, "secondary": item.secondary,
                   "detail": item.detail, "checked": item.checked,
                   "start_ts": item.start_ts, "end_ts": item.end_ts, "all_day": item.all_day,
                   "action": item.action, "options": item.options or [], "value": item.value}
                  for item in (view.items or [])],
    }


def _build(plugin: Plugin) -> Optional[dict]:
    try:
        built = serialize(plugin.device_view())
    except Exception:  # noqa: BLE001 -- one broken plugin mustn't blank the others
        log.exception("Building %s's device screen failed", plugin.id)
        built = None
    with _lock:
        _cache[plugin.id] = (time.time(), built)
        _building.discard(plugin.id)
    return built


def _rebuild_later(plugin: Plugin) -> None:
    with _lock:
        if plugin.id in _building:
            return
        _building.add(plugin.id)
    _pool.submit(_build, plugin)


def views(ordered: list[Plugin]) -> list[tuple[Plugin, Optional[dict]]]:
    """The device screens of these plugins, in order. Plugins never built
    before are built now, at the same time as each other."""
    now = time.time()
    missing = []
    with _lock:
        cached = {plugin.id: _cache.get(plugin.id) for plugin in ordered}
    for plugin in ordered:
        entry = cached[plugin.id]
        if plugin.id in _LIVE or entry is None:
            missing.append(plugin)
        elif now - entry[0] > FRESH_SECONDS:
            _rebuild_later(plugin)  # serve this copy; the next request gets a newer one
    built = dict(zip([p.id for p in missing], _pool.map(_build, missing))) if missing else {}
    with _lock:
        return [(plugin, built[plugin.id] if plugin.id in built else _cache[plugin.id][1])
                for plugin in ordered]


def invalidate(plugin_id: Optional[str] = None) -> None:
    """Rebuild one plugin's screen (or all of them) before it's next served."""
    with _lock:
        if plugin_id is None:
            _cache.clear()
        else:
            _cache.pop(plugin_id, None)


def _keep_warm() -> None:
    while True:
        for plugin in plugins.all_plugins():
            if plugin.id not in _LIVE and plugin.is_configured():
                _rebuild_later(plugin)
        time.sleep(REFRESH_EVERY_SECONDS)


def start() -> None:
    threading.Thread(target=_keep_warm, name="device-views-warm", daemon=True).start()
