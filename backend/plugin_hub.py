"""Fetches the community plugin-hub index (biggy-spoon/sticky-assistant-plugin-hub)
so the device's settings page can show what plugins exist beyond the ones
already installed.

A plugin is code that runs on this server, so installing one from the device
is limited to entries the hub's maintainers have marked "reviewed": true, with
a "download_url" to a single Python file on raw.githubusercontent.com at a
pinned commit. Anything
else is still installed by hand (copy it into backend/plugins/ and restart):
see the hub's README.
"""
import logging
import os
import re
import time
from typing import Optional

import requests

import plugins

log = logging.getLogger("sticky-assistant.plugin_hub")

HUB_INDEX_URL = "https://raw.githubusercontent.com/biggy-spoon/sticky-assistant-plugin-hub/main/index.json"
_CACHE_TTL_SECONDS = 15 * 60
_REQUEST_TIMEOUT_SECONDS = 5

_cache: Optional[list[dict]] = None
_cache_at: float = 0.0


def get_index(force: bool = False) -> tuple[list[dict], Optional[str]]:
    """Returns (entries, error). A fetch failure with a previously-good
    cache available returns the stale cache and no error, so a flaky
    connection doesn't make the dashboard's hub card flicker empty; only a
    failure with nothing cached yet surfaces an error."""
    global _cache, _cache_at
    if not force and _cache is not None and time.monotonic() - _cache_at < _CACHE_TTL_SECONDS:
        return _cache, None
    try:
        response = requests.get(HUB_INDEX_URL, timeout=_REQUEST_TIMEOUT_SECONDS)
        response.raise_for_status()
        entries = response.json()
        if not isinstance(entries, list):
            raise ValueError("hub index.json did not contain a JSON array")
        _cache, _cache_at = entries, time.monotonic()
        return entries, None
    except Exception as exc:
        log.warning("Could not fetch plugin hub index: %s", exc)
        if _cache is not None:
            return _cache, None
        return [], str(exc)


_PLUGINS_DIR = plugins.INSTALL_DIR
_MAX_PLUGIN_BYTES = 200 * 1024
# A single .py file at a full commit SHA: what the hub reviewed is exactly what
# gets installed, even if the plugin's branch changes later.
_PINNED_DOWNLOAD = re.compile(
    r"^https://raw\.githubusercontent\.com/[\w.-]+/[\w.-]+/[0-9a-f]{40}/[\w./-]+\.py$")
_MODULE_NAME = re.compile(r"^[a-z][a-z0-9_]{1,40}$")


# Set by app.py: mounts a newly installed plugin's HTTP routes.
route_registrar = None


class InstallError(Exception):
    """Why a plugin can't be installed, fit to show the user."""


def installable(entry: dict) -> bool:
    url = str(entry.get("download_url") or "")
    return entry.get("reviewed") is True and bool(_PINNED_DOWNLOAD.match(url))


def install(plugin_id: str, register_routes=None) -> str:
    register_routes = register_routes or route_registrar
    """Downloads a reviewed hub plugin into backend/plugins/ and loads it.
    `register_routes`, if given, is called with each plugin added so its HTTP
    routes are mounted. Returns the plugin's name."""
    entries, error = get_index()
    entry = next((e for e in entries if e.get("id") == plugin_id), None)
    if entry is None:
        raise InstallError(error or f"The hub has no plugin called {plugin_id!r}")
    if plugins.get(plugin_id) is not None:
        raise InstallError(f"{entry.get('name', plugin_id)} is already installed")
    if not installable(entry):
        raise InstallError(f"{entry.get('name', plugin_id)} hasn't been reviewed for installing "
                           "from the device. Install it by hand, from its repository.")
    module_name = f"{plugin_id.replace('-', '_')}_plugin"
    if not _MODULE_NAME.match(module_name):
        raise InstallError(f"{plugin_id!r} isn't a usable plugin id")
    path = os.path.join(_PLUGINS_DIR, f"{module_name}.py")
    if os.path.exists(path):
        raise InstallError(f"A file called {module_name}.py is already in {_PLUGINS_DIR}")
    try:
        response = requests.get(entry["download_url"], timeout=15)
        response.raise_for_status()
    except requests.RequestException as exc:
        raise InstallError(f"Couldn't download {entry.get('name', plugin_id)}: {exc}") from exc
    if len(response.content) > _MAX_PLUGIN_BYTES:
        raise InstallError("That plugin is larger than a plugin file should be")
    with open(path, "wb") as out:
        out.write(response.content)
    added = plugins.load_module(module_name)
    if not added:
        os.remove(path)
        raise InstallError(f"{entry.get('name', plugin_id)} didn't load, so it was removed again")
    for plugin in added:
        if register_routes is not None:
            register_routes(plugin)
    log.info("Installed plugin %s from the hub", plugin_id)
    return entry.get("name", plugin_id)
