#!/usr/bin/env python3
"""
Claude Code Stop hook: reads transcript, totals token usage, and updates ESP32 display.
Receives JSON on stdin with session_id and transcript_path.
"""

import json
import os
import sys
import time
import urllib.request

ESP32_IP = "192.168.68.75"
PREV_STATS_PATH = "/tmp/claude_display_prev.json"

# Pricing per million tokens (Opus 4)
PRICE_INPUT = 15.00
PRICE_OUTPUT = 75.00
PRICE_CACHE_WRITE = 18.75
PRICE_CACHE_READ = 1.50


CONTEXT_MAX = 1_000_000  # Opus 4.6 context window


def get_stats(transcript_path: str):
    input_tokens = 0
    output_tokens = 0
    cache_write = 0
    cache_read = 0
    turns = 0
    last_context = 0  # context size from most recent usage

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

            input_tokens += usage.get("input_tokens", 0)
            output_tokens += usage.get("output_tokens", 0)
            cache_write += usage.get("cache_creation_input_tokens", 0)
            cache_read += usage.get("cache_read_input_tokens", 0)
            if msg.get("role") == "assistant" and msg.get("stop_reason") == "end_turn":
                turns += 1

            # Track latest context size (input + cache = total context sent)
            ctx = (usage.get("input_tokens", 0)
                   + usage.get("cache_creation_input_tokens", 0)
                   + usage.get("cache_read_input_tokens", 0))
            if ctx > 0:
                last_context = ctx

    cost = (
        (input_tokens / 1_000_000) * PRICE_INPUT
        + (output_tokens / 1_000_000) * PRICE_OUTPUT
        + (cache_write / 1_000_000) * PRICE_CACHE_WRITE
        + (cache_read / 1_000_000) * PRICE_CACHE_READ
    )

    return {
        "input_tokens": input_tokens,
        "output_tokens": output_tokens,
        "cache_write": cache_write,
        "cache_read": cache_read,
        "cost": cost,
        "turns": turns,
        "context_used": last_context,
    }


def fmt_k(n: int) -> str:
    """Format token count: 1234 -> '1.2k', 12345 -> '12k', 123456 -> '123k'"""
    if n < 1000:
        return str(n)
    elif n < 10_000:
        return f"{n/1000:.1f}k"
    elif n < 1_000_000:
        return f"{n//1000}k"
    else:
        return f"{n/1_000_000:.1f}M"


def fmt_full(n: int) -> str:
    """Format token count with commas: 123456 -> '123,456'"""
    return f"{n:,}"


def send_batch(commands: list):
    """Send a batch of draw commands to the ESP32."""
    data = json.dumps({"commands": commands}).encode()
    req = urllib.request.Request(
        f"http://{ESP32_IP}/batch",
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        urllib.request.urlopen(req, timeout=3)
    except Exception:
        pass


def context_color(used: int) -> str:
    """Green -> Yellow -> Red as context fills up."""
    pct = used / CONTEXT_MAX
    if pct < 0.5:
        return "#00FF88"
    elif pct < 0.75:
        return "#F39C12"
    else:
        return "#E74C3C"


def static_commands(stats: dict, project: str = "") -> list:
    """Build the non-animated parts of the display."""
    header = project or "Claude Session"
    # Size 2 font is ~14px per char, 240px wide with 10px margin = ~16 chars max
    if len(header) > 16:
        header = header[:16]
    return [
        {"type": "clear", "color": "#000000"},
        # Header
        {"type": "text", "text": header, "x": 10, "y": 8, "size": 2, "color": "#7B68EE", "bg": "#000000"},
        # Labels only (values animated separately)
        {"type": "text", "text": "IN", "x": 10, "y": 88, "size": 2, "color": "#666666", "bg": "#000000"},
        {"type": "text", "text": "OUT", "x": 10, "y": 122, "size": 2, "color": "#666666", "bg": "#000000"},
        # Turns
        {"type": "text", "text": f"Turns: {stats['turns']}", "x": 10, "y": 152, "size": 2, "color": "#7B68EE", "bg": "#000000"},
        # Context window usage
        {"type": "text", "text": f"Context {fmt_k(stats['context_used'])}/{fmt_k(CONTEXT_MAX)} ({int(stats['context_used']/CONTEXT_MAX*100)}%)", "x": 10, "y": 182, "size": 1, "color": "#FFFFFF", "bg": "#000000"},
        {"type": "bar", "x": 10, "y": 196, "w": 220, "h": 18, "value": min(stats["context_used"] / CONTEXT_MAX, 1.0), "color": context_color(stats["context_used"]), "track": "#1a1a2e", "radius": 6},
        # Footer
        {"type": "rect", "x": 0, "y": 224, "w": 240, "h": 16, "color": "#0a0a15", "filled": True},
        {"type": "text", "text": "claude code | opus 4.6", "x": 30, "y": 226, "size": 1, "color": "#FFFFFF", "bg": "#0a0a15"},
    ]


def load_prev() -> dict:
    try:
        with open(PREV_STATS_PATH) as f:
            return json.load(f)
    except Exception:
        return {}


def save_prev(stats: dict):
    try:
        with open(PREV_STATS_PATH, "w") as f:
            total_in = stats["input_tokens"] + stats["cache_write"] + stats["cache_read"]
            json.dump({"cost": stats["cost"], "total_in": total_in, "total_out": stats["output_tokens"]}, f)
    except Exception:
        pass


def send_to_display(stats: dict, project: str = ""):
    total_in = stats["input_tokens"] + stats["cache_write"] + stats["cache_read"]
    total_out = stats["output_tokens"]
    cost = stats["cost"]

    prev = load_prev()
    prev_cost = prev.get("cost", 0.0)
    prev_in = prev.get("total_in", 0)
    prev_out = prev.get("total_out", 0)

    def claude_mascot(squish=0):
        """Draw Claude as block rects. squish: 0=normal, 1=slight, 2=flat."""
        ox, oy = 170, 48
        sw = 4   # block width
        sh = 7   # block height (taller rectangles)
        color = "#D97757"
        blocks = []
        eyes = []

        if squish == 0:
            #   OOOOOOOOOO       row 0: cols 2-11 (10 wide)
            #   OO.OOOO.OO      row 1: cols 2-11, eyes at 4,9
            # OOOOOOOOOOOOOO    row 2: cols 0-13 (14 wide, arms out)
            #   OOOOOOOOOO      row 3: cols 2-11
            #    O O  O O       row 4: feet at 3,5,8,10
            for c in range(2, 12): blocks.append((c, 0))
            for c in range(2, 12): blocks.append((c, 1))
            eyes += [(4, 1), (9, 1)]
            for c in range(0, 14): blocks.append((c, 2))
            for c in range(2, 12): blocks.append((c, 3))
            for c in [3, 5, 8, 10]: blocks.append((c, 4))
        elif squish == 1:
            # Squished: lose top row, arms stay
            for c in range(2, 12): blocks.append((c, 1))
            for c in range(0, 14): blocks.append((c, 2))
            eyes += [(4, 2), (9, 2)]
            for c in range(2, 12): blocks.append((c, 3))
            for c in range(2, 12): blocks.append((c, 4))
        else:
            # Full squish: flat blob
            for c in range(0, 14): blocks.append((c, 2))
            eyes += [(4, 2), (9, 2)]
            for c in range(0, 14): blocks.append((c, 3))
            for c in range(0, 14): blocks.append((c, 4))

        cmds = []
        for c, r in blocks:
            cmds.append({"type": "rect", "x": ox + c * sw, "y": oy + r * sh, "w": sw, "h": sh, "color": color, "filled": True})
        for c, r in eyes:
            cmds.append({"type": "rect", "x": ox + c * sw + 1, "y": oy + r * sh + 1, "w": sw - 2, "h": sh - 2, "color": "#000000", "filled": True})
        return cmds

    def value_commands(c, i, o):
        return [
            {"type": "rect", "x": 5, "y": 40, "w": 170, "h": 46, "color": "#000000", "filled": True},
            {"type": "text", "text": f"${c:.2f}", "x": 10, "y": 45, "size": 3, "color": "#00FF88", "bg": "#000000"},
            {"type": "rect", "x": 43, "y": 85, "w": 195, "h": 34, "color": "#000000", "filled": True},
            {"type": "text", "text": fmt_full(i), "x": 45, "y": 88, "size": 2, "color": "#5DADE2", "bg": "#000000"},
            {"type": "rect", "x": 58, "y": 119, "w": 180, "h": 30, "color": "#000000", "filled": True},
            {"type": "text", "text": fmt_full(o), "x": 60, "y": 122, "size": 2, "color": "#F39C12", "bg": "#000000"},
        ]

    # Draw everything in one batch (static + initial values + mascot) — no flash
    send_batch(
        static_commands(stats, project)
        + value_commands(prev_cost, prev_in, prev_out)
        + claude_mascot(0)
    )

    # Animate count-up + mascot squish: normal → squish1 → squish2 → squish1 → normal
    squish_seq = [0, 1, 2, 1, 0, 1, 2, 1, 0, 0]
    frames = 10
    for i in range(1, frames + 1):
        frac = i / frames
        cur_cost = prev_cost + (cost - prev_cost) * frac
        cur_in = int(prev_in + (total_in - prev_in) * frac)
        cur_out = int(prev_out + (total_out - prev_out) * frac)
        sq = squish_seq[i - 1]
        send_batch(
            value_commands(cur_cost, cur_in, cur_out)
            + [{"type": "rect", "x": 168, "y": 46, "w": 62, "h": 40, "color": "#000000", "filled": True}]
            + claude_mascot(sq)
        )
        if i < frames:
            time.sleep(0.08)

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

    stats = get_stats(transcript_path)
    send_to_display(stats, project)


if __name__ == "__main__":
    main()
