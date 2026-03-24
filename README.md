# ESP32 Display Server for Claude Code

A physical ambient display that shows real-time Claude Code session stats on a 240x240 TFT screen, powered by an ESP32-S3. Features animated token counters, cost tracking, context window usage, and a bouncy pixel-art Claude mascot.

![Claude Code Display](claudebb.jpg)

## What it does

After every Claude Code response, the display updates with:
- **Estimated API cost** (animated count-up, correct per-model pricing)
- **Billed-equivalent input/output token counts** (animated)
- **Turn counter**
- **Context window usage** (progress bar, color-coded green/yellow/red)
- **Project folder name** in the header
- **Current model** in the footer (updates dynamically as you switch models)
- **Pixel-art Claude mascot** that does a double-bounce squish animation
- **Multi-session aware** — updates for whichever session last responded

## Hardware

- **ESP32-S3 Super Mini** (with 2MB PSRAM)
- **1.54" ST7789 240x240 TFT display**
- **3D printed case** from [BambuHelper by Keralots](https://github.com/Keralots/BambuHelper) (designed for a Bambu Lab printer monitor, works great as a general-purpose ESP32 display enclosure)

### Wiring

| Display Pin | ESP32-S3 Pin |
|-------------|-------------|
| MOSI (SDA)  | GPIO 11     |
| SCLK (SCL)  | GPIO 12     |
| CS          | GPIO 10     |
| DC          | GPIO 9      |
| RST         | GPIO 8      |
| BL          | GPIO 13     |
| VCC         | 3.3V        |
| GND         | GND         |

## Architecture

```
Claude Code
    |
    |-- Stop Hook (update_display.py)
    |       Fires after each response, reads transcript,
    |       calculates stats, sends single POST to /dashboard
    |
    |-- MCP Server (mcp_server.py)
    |       Exposes drawing tools so Claude can draw on the display
    |
    v
ESP32-S3 Firmware (main.cpp)
    |-- HTTP server on port 80
    |-- /dashboard endpoint: receives stats, runs animation onboard
    |-- Drawing primitives: text, rect, circle, line, bar, gauge
    |-- /batch endpoint with PSRAM framebuffer (flicker-free)
    |-- /update endpoint for OTA firmware updates
    |-- AP mode WiFi provisioning (no hardcoded credentials)
    |-- mDNS: esp32-display.local
    |
    v
ST7789 240x240 TFT Display
```

## Quick Start (pre-built firmware)

No toolchain needed. Grab the latest merged binary from the `releases/` folder and flash with the [Espressif web flasher](https://espressif.github.io/esptool-js/):

1. Connect ESP32-S3 via USB
2. Open [https://espressif.github.io/esptool-js/](https://espressif.github.io/esptool-js/)
3. **Erase flash** first
4. Flash `releases/firmware-2.9-ap-mode-merged.bin` at address `0x0`
5. Unplug USB (important — usbipd can prevent boot on WSL2)
6. Power on — the display will show WiFi setup instructions

### WiFi Setup (AP Mode)

On first boot the ESP32 starts a setup hotspot:

1. Connect your phone or laptop to **`ESP32-Display-Setup`**
2. Open a browser and go to **`192.168.4.1`**
3. Enter your WiFi SSID and password, hit Save
4. The device reboots and connects — the IP and `esp32-display.local` are shown on the display

WiFi credentials are stored in NVS (non-volatile storage) and survive firmware updates.

To reset credentials: `curl -X POST http://esp32-display.local/wifi/clear`

---

## Setup (hooks + MCP)

### 1. MCP Server

Requires Python 3 with `requests` and `mcp` packages:

```bash
pip install requests mcp
```

Add to your Claude Code MCP config (`~/.claude/.mcp.json` for global, or `.mcp.json` in project root):

```json
{
  "mcpServers": {
    "esp32-display": {
      "type": "stdio",
      "command": "python3",
      "args": ["/path/to/esp32-display-server/mcp_server.py"]
    }
  }
}
```

### 2. Dashboard Hook

Add the Stop hook to Claude Code settings (`~/.claude/settings.json` for global, or `.claude/settings.local.json` per project):

```json
{
  "hooks": {
    "Stop": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "python3 /path/to/esp32-display-server/update_display.py",
            "timeout": 10,
            "async": true
          }
        ]
      }
    ]
  }
}
```

### 3. IP / mDNS

By default `update_display.py` tries `esp32-display.local` first and falls back to the hardcoded IP. Update `ESP32_IP` in `update_display.py` and `mcp_server.py` if your network doesn't support mDNS.

---

## Building from Source

Install [PlatformIO](https://platformio.org/).

```bash
# config.h is gitignored — copy the example
cp include/config.example.h include/config.h
# No WiFi creds needed — provisioned via AP mode at runtime

# Build (versioned bin created automatically)
pio run --target clean && pio run

# Firmware bin: .pio/build/esp32s3/firmware-<version>.bin
```

### OTA Updates (after first flash)

```bash
curl -F "firmware=@.pio/build/esp32s3/firmware-<version>.bin" http://esp32-display.local/update
# or visit http://esp32-display.local/update in your browser
```

---

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/status` | GET | Device info (IP, RSSI, uptime, version) |
| `/help` | GET | API documentation |
| `/dashboard` | POST | Full session stats — ESP animates the display |
| `/batch` | POST | Multiple draw commands, framebuffered |
| `/clear` | POST | Fill screen with color |
| `/text` | POST | Draw text |
| `/rect` | POST | Draw rectangle |
| `/circle` | POST | Draw circle |
| `/line` | POST | Draw line |
| `/bar` | POST | Horizontal progress bar |
| `/gauge` | POST | Circular gauge |
| `/brightness` | POST | Set backlight (0-255) |
| `/wifi` | POST | Update WiFi credentials: `{"ssid":"x","pass":"y"}` |
| `/wifi/clear` | POST | Wipe credentials, reboot into AP mode |
| `/update` | GET/POST | OTA firmware upload |

## Pricing

Token costs are calculated per-model from the transcript:

| Model | Input | Output | Cache Write | Cache Read |
|-------|-------|--------|-------------|------------|
| Claude Opus 4.6 | $5/M | $25/M | $6.25/M | $0.50/M |
| Claude Sonnet 4.6 | $3/M | $15/M | $3.75/M | $0.30/M |
| Claude Haiku 4.5 | $1/M | $5/M | $1.25/M | $0.10/M |

Mixed-model sessions are billed accurately per turn. The displayed IN token count is normalized to a billed-equivalent value so it correlates with the dollar amount shown.

## Customization

- **Display layout / pricing**: Edit `update_display.py` — no firmware flash needed
- **Draw anything**: Use the MCP tools from Claude, or POST directly to the HTTP API
- **Pin assignments**: Edit `include/config.h` and TFT_eSPI flags in `platformio.ini`
- **mDNS hostname**: Change `MDNS_HOSTNAME` in `include/config.h`

## License

MIT
