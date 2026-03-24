#!/usr/bin/env python3
"""MCP server that exposes the ESP32 display as tools Claude can use."""

import sys
import logging
import requests
from mcp.server.fastmcp import FastMCP

logging.basicConfig(level=logging.INFO, stream=sys.stderr)

ESP32_IP = "192.168.68.75"
BASE = f"http://{ESP32_IP}"

mcp = FastMCP("esp32-display")


def post(endpoint: str, data: dict) -> str:
    try:
        r = requests.post(f"{BASE}{endpoint}", json=data, timeout=5)
        return r.text
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
def display_clear(color: str = "#000000") -> str:
    """Clear the entire 240x240 display with a solid color.

    Args:
        color: Hex color like #000000 (black), #FF0000 (red), etc.
    """
    return post("/clear", {"color": color})


@mcp.tool()
def display_text(text: str, x: int, y: int, size: int = 2, color: str = "#FFFFFF", bg: str = "#000000") -> str:
    """Draw text on the display. Font is monospace, size 1=tiny, 2=normal, 3=large, 4=huge.
    Display is 240x240 pixels. Each character at size 2 is roughly 12x16px.

    Args:
        text: The text to display
        x: X position in pixels (0-239, left edge)
        y: Y position in pixels (0-239, top edge)
        size: Text size multiplier (1-4)
        color: Text color in hex
        bg: Background color in hex (prevents ghosting)
    """
    return post("/text", {"text": text, "x": x, "y": y, "size": size, "font": 1, "color": color, "bg": bg})


@mcp.tool()
def display_rect(x: int, y: int, w: int, h: int, color: str = "#FFFFFF", filled: bool = True, radius: int = 0) -> str:
    """Draw a rectangle on the display.

    Args:
        x: X position
        y: Y position
        w: Width in pixels
        h: Height in pixels
        color: Hex color
        filled: Fill the rectangle or just outline
        radius: Corner radius for rounded rectangles
    """
    return post("/rect", {"x": x, "y": y, "w": w, "h": h, "color": color, "filled": filled, "radius": radius})


@mcp.tool()
def display_circle(x: int, y: int, r: int, color: str = "#FFFFFF", filled: bool = True) -> str:
    """Draw a circle on the display.

    Args:
        x: Center X
        y: Center Y
        r: Radius in pixels
        color: Hex color
        filled: Fill or just outline
    """
    return post("/circle", {"x": x, "y": y, "r": r, "color": color, "filled": filled})


@mcp.tool()
def display_line(x0: int, y0: int, x1: int, y1: int, color: str = "#FFFFFF") -> str:
    """Draw a line on the display.

    Args:
        x0: Start X
        y0: Start Y
        x1: End X
        y1: End Y
        color: Hex color
    """
    return post("/line", {"x0": x0, "y0": y0, "x1": x1, "y1": y1, "color": color})


@mcp.tool()
def display_gauge(x: int, y: int, r: int, value: float, label: str = "", color: str = "#00FF00", bg: str = "#000000", track: str = "#333333") -> str:
    """Draw a circular gauge (arc) on the display. Great for showing percentages or progress.

    Args:
        x: Center X
        y: Center Y
        r: Outer radius
        value: Value from 0.0 to 1.0
        label: Text shown in center of gauge
        color: Gauge fill color
        bg: Background color
        track: Unfilled track color
    """
    ir = max(r - 8, 0)
    return post("/gauge", {"x": x, "y": y, "r": r, "ir": ir, "value": value, "label": label, "color": color, "bg": bg, "track": track})


@mcp.tool()
def display_bar(x: int, y: int, w: int, h: int, value: float, color: str = "#00FF00", track: str = "#333333", radius: int = 4) -> str:
    """Draw a horizontal progress bar.

    Args:
        x: X position
        y: Y position
        w: Total width
        h: Height
        value: Fill amount from 0.0 to 1.0
        color: Fill color
        track: Background track color
        radius: Corner radius
    """
    return post("/bar", {"x": x, "y": y, "w": w, "h": h, "value": value, "color": color, "track": track, "radius": radius})


@mcp.tool()
def display_brightness(level: int = 200) -> str:
    """Set display backlight brightness.

    Args:
        level: Brightness 0 (off) to 255 (max)
    """
    return post("/brightness", {"level": level})


@mcp.tool()
def display_batch(commands: list[dict]) -> str:
    """Send multiple drawing commands in one request for flicker-free updates.
    Each command is a dict with a "type" field matching an endpoint name
    (clear, text, rect, circle, line, bar, gauge) plus that command's parameters.

    Example: [{"type":"clear","color":"#000000"}, {"type":"text","text":"Hi","x":0,"y":0,"size":2,"color":"#FFFFFF"}]

    Args:
        commands: List of drawing command dicts
    """
    return post("/batch", {"commands": commands})


@mcp.tool()
def display_status() -> str:
    """Get ESP32 display device status (IP, WiFi signal, uptime, free memory)."""
    try:
        r = requests.get(f"{BASE}/status", timeout=5)
        return r.text
    except Exception as e:
        return f"Error: {e}"


def main():
    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
