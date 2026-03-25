#!/usr/bin/env python3
"""
Claude Code Stop hook: reads transcript, totals token usage, and updates ESP32 display.
Receives JSON on stdin with session_id and transcript_path.
"""

import json
import os
import sys
import urllib.request

ESP32_MDNS = "esp32-display.local"
ESP32_IP   = "192.168.68.75"   # fallback if mDNS fails
PREV_STATS_PATH  = "/tmp/claude_display_prev.json"
PAUSE_FLAG_PATH  = "/tmp/esp32_display_paused"

# Pricing per million tokens by model prefix
MODEL_PRICING = {
    "claude-opus-4":   {"input": 5.00,  "output": 25.00, "cache_write": 6.25,  "cache_read": 0.50},
    "claude-sonnet-4": {"input": 3.00,  "output": 15.00, "cache_write": 3.75,  "cache_read": 0.30},
    "claude-haiku-4":  {"input": 1.00,  "output": 5.00,  "cache_write": 1.25,  "cache_read": 0.10},
}
DEFAULT_PRICING = MODEL_PRICING["claude-opus-4"]

CONTEXT_MAX = 1_000_000  # 1M context window


def get_pricing(model: str) -> dict:
    for prefix, prices in MODEL_PRICING.items():
        if model and model.startswith(prefix):
            return prices
    return DEFAULT_PRICING


def get_stats(transcript_path: str):
    input_tokens = 0
    output_tokens = 0
    cache_write = 0
    cache_read = 0
    cost = 0.0
    turns = 0
    last_context = 0
    last_model = "claude-opus-4"

    with open(transcript_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                entry = json.loads(line)
            except json.JSONDecodeError:
                continue

            msg = entry.get("message", {})
            if not isinstance(msg, dict):
                continue
            usage = msg.get("usage")
            if not usage:
                continue

            model = msg.get("model", "")
            pricing = get_pricing(model)
            if model:
                last_model = model

            i = usage.get("input_tokens", 0)
            o = usage.get("output_tokens", 0)
            cw = usage.get("cache_creation_input_tokens", 0)
            cr = usage.get("cache_read_input_tokens", 0)

            input_tokens += i
            output_tokens += o
            cache_write += cw
            cache_read += cr

            cost += (
                (i  / 1_000_000) * pricing["input"]
                + (o  / 1_000_000) * pricing["output"]
                + (cw / 1_000_000) * pricing["cache_write"]
                + (cr / 1_000_000) * pricing["cache_read"]
            )

            if msg.get("role") == "assistant" and msg.get("stop_reason") == "end_turn":
                turns += 1

            ctx = i + cw + cr
            if ctx > 0:
                last_context = ctx

    # Billed-equivalent IN tokens normalized to last model's input rate
    last_pricing = get_pricing(last_model)
    billed_in = int(
        input_tokens
        + cache_write * (last_pricing["cache_write"] / last_pricing["input"])
        + cache_read  * (last_pricing["cache_read"]  / last_pricing["input"])
    )

    return {
        "input_tokens": input_tokens,
        "output_tokens": output_tokens,
        "cache_write": cache_write,
        "cache_read": cache_read,
        "cost": cost,
        "turns": turns,
        "context_used": last_context,
        "billed_in": billed_in,
        "model": last_model,
    }


def _model_short(model: str) -> str:
    """Convert model ID to short display name, e.g. 'claude-sonnet-4-6' -> 'sonnet 4.6'"""
    for prefix, name in [("claude-opus-4", "opus 4"), ("claude-sonnet-4", "sonnet 4"), ("claude-haiku-4", "haiku 4")]:
        if model.startswith(prefix):
            # extract version suffix like -5, -6, -5-20251001
            suffix = model[len(prefix):]
            parts = [p for p in suffix.lstrip("-").split("-") if p.isdigit() and len(p) <= 2]
            version = ".".join(parts[:2]) if len(parts) >= 2 else (parts[0] if parts else "")
            return f"{name.split()[0]} {name.split()[1]}.{version}" if version else name
    return model or "claude"


def load_prev() -> dict:
    try:
        with open(PREV_STATS_PATH) as f:
            return json.load(f)
    except Exception:
        return {}


def save_prev(stats: dict):
    try:
        with open(PREV_STATS_PATH, "w") as f:
            json.dump({"cost": stats["cost"], "total_in": stats["billed_in"], "total_out": stats["output_tokens"]}, f)
    except Exception:
        pass


def send_to_display(stats: dict, project: str = ""):
    prev = load_prev()
    payload = {
        "project": project,
        "model": _model_short(stats.get("model", "")),
        "cost": stats["cost"],
        "cost_prev": prev.get("cost", 0.0),
        "in_tokens": stats["billed_in"],
        "in_prev": prev.get("total_in", 0),
        "out_tokens": stats["output_tokens"],
        "out_prev": prev.get("total_out", 0),
        "turns": stats["turns"],
        "context_used": stats["context_used"],
        "context_max": CONTEXT_MAX,
    }
    data = json.dumps(payload).encode()
    for host in [ESP32_MDNS, ESP32_IP]:
        try:
            req = urllib.request.Request(
                f"http://{host}/dashboard",
                data=data,
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            urllib.request.urlopen(req, timeout=3)
            break
        except Exception:
            continue
    save_prev(stats)


def main():
    try:
        hook_input = json.loads(sys.stdin.read())
    except (json.JSONDecodeError, Exception):
        return

    transcript_path = hook_input.get("transcript_path")
    if not transcript_path:
        return

    cwd = hook_input.get("cwd", "")
    project = os.path.basename(cwd) if cwd else ""

    if os.path.exists(PAUSE_FLAG_PATH):
        return  # dashboard updates paused

    stats = get_stats(transcript_path)
    send_to_display(stats, project)


if __name__ == "__main__":
    main()
