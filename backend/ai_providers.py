"""The AI model behind the assistant, which the user picks in settings.

Two ways of talking to a model:

- Anthropic's Messages API, for Claude. This is the only one with web search,
  which Anthropic runs on its side.
- The chat completions API that OpenAI defined and most other providers also
  offer: Google's Gemini (through its OpenAI-compatible endpoint), OpenAI, and
  anything else that speaks it, such as OpenRouter, Ollama or LM Studio, given
  a base URL.

Plugins describe their tools in Anthropic's format (name, description,
input_schema); the chat completions client converts them.
"""
import json
import logging
import threading
import time
from typing import Callable

import anthropic
import requests

import settings

log = logging.getLogger("sticky-assistant.ai")

PROVIDERS = {
    "anthropic": "Claude (Anthropic)",
    "gemini": "Gemini (Google)",
    "openai": "OpenAI or compatible",
}

GEMINI_BASE_URL = "https://generativelanguage.googleapis.com/v1beta/openai"
_REQUEST_TIMEOUT_SECONDS = 90
_MODEL_LIST_SECONDS = 10 * 60
_MAX_TOOL_ROUNDS = 5

ToolRunner = Callable[[str, dict], str]


class AIError(Exception):
    """A request to the model failed. The message is fit to show the user."""


# --- Anthropic ------------------------------------------------------------------

# Models that accept the newer web search tool. The model setting is free text,
# and an older model rejects the whole request if sent this version, so anything
# not listed here gets the basic one.
_WEB_SEARCH_20260209_MODELS = {
    "claude-fable-5-1", "claude-fable-5", "claude-opus-5-5", "claude-opus-5", "claude-opus-4-8",
    "claude-opus-4-7", "claude-opus-4-6", "claude-sonnet-5-5", "claude-sonnet-5",
    "claude-sonnet-4-6", "claude-haiku-5-5",
}


def _web_search_tool(model: str) -> dict:
    """Anthropic runs this tool itself, so there's no plugin behind it. max_uses
    limits how long a reply can take and what it can cost, since each search is
    billed."""
    version = "20260209" if model in _WEB_SEARCH_20260209_MODELS else "20250305"
    return {"type": f"web_search_{version}", "name": "web_search", "max_uses": 3}


def _final_text(content: list) -> str:
    """The text after the last tool activity. Claude can write a line before a
    search, like "Let me look that up", which isn't part of the answer."""
    parts: list[str] = []
    for block in content:
        if block.type == "text":
            parts.append(block.text)
        elif block.type != "thinking":
            parts = []
    return "".join(parts).strip()


def _converse_anthropic(system: str, messages: list[dict], tools: list[dict],
                        run_tool: ToolRunner) -> str:
    client = anthropic.Anthropic(api_key=settings.get_ai_api_key("anthropic"))
    model = settings.get_ai_model("anthropic")
    tools = tools + [_web_search_tool(model)]
    try:
        for _ in range(_MAX_TOOL_ROUNDS):
            # Thinking counts towards max_tokens, so this is well above the reply length.
            response = client.messages.create(model=model, max_tokens=4096, system=system,
                                              messages=messages, tools=tools)
            messages.append({"role": "assistant", "content": response.content})
            if response.stop_reason == "pause_turn":
                continue  # a long search paused; sending it back as is resumes it
            if response.stop_reason != "tool_use":
                return _final_text(response.content) or "I don't have an answer for that."
            results = [{"type": "tool_result", "tool_use_id": block.id,
                        "content": run_tool(block.name, block.input)}
                       for block in response.content if block.type == "tool_use"]
            messages.append({"role": "user", "content": results})
    except anthropic.APIError as exc:
        raise AIError(f"Claude request failed: {exc}") from exc
    return "I'm having trouble finishing that request -- can you try again?"


def _anthropic_models() -> list[str]:
    client = anthropic.Anthropic(api_key=settings.get_ai_api_key("anthropic"))
    try:
        return [model.id for model in client.models.list(limit=50)]
    except anthropic.APIError as exc:
        raise AIError(f"Couldn't list Claude models: {exc}") from exc


# --- chat completions (Gemini, OpenAI and compatible) ----------------------------------

def _chat_base_url(provider: str) -> str:
    if provider == "gemini":
        return GEMINI_BASE_URL
    return settings.get_openai_base_url().rstrip("/")


def _chat_headers(provider: str) -> dict:
    key = settings.get_ai_api_key(provider)
    # A local server such as Ollama needs no key.
    return {"Authorization": f"Bearer {key}"} if key else {}


def _chat_tools(tools: list[dict]) -> list[dict]:
    return [{"type": "function",
             "function": {"name": tool["name"], "description": tool.get("description", ""),
                          "parameters": tool.get("input_schema", {"type": "object"})}}
            for tool in tools]


def _chat_post(provider: str, body: dict) -> dict:
    url = _chat_base_url(provider) + "/chat/completions"
    try:
        response = requests.post(url, json=body, headers=_chat_headers(provider),
                                 timeout=_REQUEST_TIMEOUT_SECONDS)
    except requests.RequestException as exc:
        raise AIError(f"{PROVIDERS[provider]} isn't reachable: {exc}") from exc
    if response.status_code >= 400:
        detail = response.text[:300]
        try:
            detail = response.json()["error"]["message"]
        except (ValueError, KeyError, TypeError):
            pass
        raise AIError(f"{PROVIDERS[provider]} request failed ({response.status_code}): {detail}")
    return response.json()


def _converse_chat(provider: str, system: str, messages: list[dict], tools: list[dict],
                   run_tool: ToolRunner) -> str:
    body_messages = [{"role": "system", "content": system}] + messages
    body = {"model": settings.get_ai_model(provider), "messages": body_messages}
    if tools:
        body["tools"] = _chat_tools(tools)
    for _ in range(_MAX_TOOL_ROUNDS):
        reply = _chat_post(provider, body)
        try:
            message = reply["choices"][0]["message"]
        except (KeyError, IndexError, TypeError) as exc:
            raise AIError(f"{PROVIDERS[provider]} sent an answer this couldn't read") from exc
        calls = message.get("tool_calls") or []
        if not calls:
            return (message.get("content") or "").strip() or "I don't have an answer for that."
        body_messages.append({"role": "assistant", "content": message.get("content"),
                              "tool_calls": calls})
        for call in calls:
            function = call.get("function", {})
            try:
                arguments = json.loads(function.get("arguments") or "{}")
            except json.JSONDecodeError:
                arguments = {}
            body_messages.append({"role": "tool", "tool_call_id": call.get("id", ""),
                                  "content": run_tool(function.get("name", ""), arguments)})
    return "I'm having trouble finishing that request -- can you try again?"


def _chat_models(provider: str) -> list[str]:
    try:
        response = requests.get(_chat_base_url(provider) + "/models",
                                headers=_chat_headers(provider), timeout=15)
        response.raise_for_status()
    except requests.RequestException as exc:
        raise AIError(f"Couldn't list {PROVIDERS[provider]} models: {exc}") from exc
    ids = [item.get("id", "") for item in response.json().get("data", [])]
    ids = [model_id.removeprefix("models/") for model_id in ids if model_id]
    if provider == "gemini":
        ids = [model_id for model_id in ids if model_id.startswith("gemini")]
    return sorted(ids)


# --- what the rest of the backend uses ---------------------------------------------

def provider_configured(provider: str) -> bool:
    if provider == "openai":
        return bool(settings.get_openai_base_url())  # a local server may need no key
    return bool(settings.get_ai_api_key(provider))


def is_configured() -> bool:
    return provider_configured(settings.get_ai_provider())


def assistant_name() -> str:
    """A short name for the device to call the assistant: "Claude", "Gemini",
    or for any other model the start of its id ("gpt-4.1-mini" -> "GPT",
    "llama3:8b" -> "Llama3")."""
    provider = settings.get_ai_provider()
    if provider == "anthropic":
        return "Claude"
    if provider == "gemini":
        return "Gemini"
    model = settings.get_ai_model(provider).split("/")[-1]
    stem = model.replace(":", "-").split("-")[0]
    if not stem:
        return "Assistant"
    return stem.upper() if len(stem) <= 3 else stem[:1].upper() + stem[1:12]


def converse(system: str, history: list[dict], transcript: str, tools: list[dict],
             run_tool: ToolRunner) -> str:
    """One turn: the reply to `transcript`, after any tool calls. `history` is
    earlier plain-text user and assistant messages."""
    provider = settings.get_ai_provider()
    messages = history + [{"role": "user", "content": transcript}]
    if provider == "anthropic":
        return _converse_anthropic(system, messages, tools, run_tool)
    return _converse_chat(provider, system, messages, tools, run_tool)


_model_cache: dict[tuple, tuple[float, list[str]]] = {}
_model_cache_lock = threading.Lock()


def list_models(provider: str) -> list[str]:
    """The models this provider offers to this account, cached for a while.
    Includes the one in use even if the list leaves it out."""
    key = (provider, settings.get_ai_api_key(provider), _chat_base_url(provider))
    with _model_cache_lock:
        cached = _model_cache.get(key)
        if cached and time.time() - cached[0] < _MODEL_LIST_SECONDS:
            models = list(cached[1])
        else:
            models = None
    if models is None:
        if provider != "openai" and not settings.get_ai_api_key(provider):
            models = []  # no key yet: nothing to ask
        else:
            models = _anthropic_models() if provider == "anthropic" else _chat_models(provider)
        with _model_cache_lock:
            _model_cache[key] = (time.time(), list(models))
    current = settings.get_ai_model(provider)
    if current and current not in models:
        models.insert(0, current)
    return models
