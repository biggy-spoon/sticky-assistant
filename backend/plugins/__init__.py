"""Plugin registry. Every .py file in this package (other than base.py) is
scanned at import time for a plugins.base.Plugin subclass defined in it;
any found are instantiated and registered automatically.

This auto-discovery *is* the "install" story for a community plugin: copy
its file(s) into this directory and restart the backend -- no registry
edits needed. It's also exactly how the two built-in plugins
(calendar_plugin.py, todo_plugin.py) get registered; they aren't special-
cased, which is deliberate proof the discovery mechanism is what actually
runs in production, not just what community plugins get.

See base.py for the plugin contract and calendar_plugin.py / todo_plugin.py
for reference implementations.
"""
import importlib
import inspect
import logging
import os
import pkgutil
from typing import Optional

from .base import Plugin

log = logging.getLogger("sticky-assistant.plugins")

# Where plugins installed from the hub go: this directory, or PLUGINS_DIR if
# set, which keeps them out of the code (in Docker, on the data volume, so
# they survive the container being rebuilt). Plugins there load like the
# built-in ones, as part of this package.
INSTALL_DIR = os.path.abspath(os.getenv("PLUGINS_DIR") or os.path.dirname(__file__))
if INSTALL_DIR != os.path.abspath(os.path.dirname(__file__)):
    os.makedirs(INSTALL_DIR, exist_ok=True)
    __path__.append(INSTALL_DIR)

_plugins: dict[str, Plugin] = {}


def register(plugin: Plugin) -> None:
    _plugins[plugin.id] = plugin


def all_plugins() -> list[Plugin]:
    return list(_plugins.values())


def get(plugin_id: str) -> Optional[Plugin]:
    return _plugins.get(plugin_id)


def load_module(name: str) -> list[Plugin]:
    """Imports plugins/<name>.py and registers each Plugin class it defines.
    Returns the plugins it added. Used at startup and to load a plugin just
    installed from the hub, without a restart."""
    try:
        module = importlib.import_module(f".{name}", package=__name__)
    except Exception:
        log.exception("Failed to import plugin module %r -- skipping it", name)
        return []
    added = []
    for _, obj in inspect.getmembers(module, inspect.isclass):
        # obj.__module__ check excludes names merely imported into this
        # module (e.g. `from .base import Plugin`) -- only classes
        # actually defined here should be registered.
        if obj is Plugin or not issubclass(obj, Plugin) or obj.__module__ != module.__name__:
            continue
        try:
            plugin = obj()
            register(plugin)
            added.append(plugin)
        except Exception:
            log.exception("Failed to load plugin %s from %r -- skipping it", obj.__name__, name)
    return added


def _discover() -> None:
    for module_info in pkgutil.iter_modules(__path__):
        if module_info.name != "base":
            load_module(module_info.name)


_discover()
