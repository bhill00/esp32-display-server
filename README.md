# ESP32 Display Server for Claude Code

A physical ambient display that shows real-time Claude Code session stats on a 240x240 TFT screen, powered by an ESP32-S3. Features animated token counters, cost tracking, context window usage, and a bouncy pixel-art Claude mascot.

![Claude Code Display](https://img.shields.io/badge/ESP32--S3-Claude_Code-D97757)

## What it does

After every Claude Code response, the display updates with:
- **Estimated API cost** (animated count-up)
- **Input/Output token counts** (with comma formatting, animated)
- **Turn counter**
- **Context window usage** (progress bar, color-coded green/yellow/red)
- **Project folder name** in the header
- **Pixel-art Claude mascot** that does a double-bounce squish animation

## Hardware

- **ESP32-S3 Super Mini** (with 2MB PSRAM)
- **1.54" ST7789 240x240 TFT display**

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
    |       calculates stats, sends animated dashboard via HTTP
    |
    |-- MCP Server (mcp_server.py)
    |       Exposes drawing tools so Claude can draw on the display
    |
    v
ESP32-S3 Firmware (main.cpp)
    |-- HTTP server on port 80
    |-- Drawing primitives: text, rect, circle, line, bar, gauge
    |-- /batch endpoint with PSRAM framebuffer (flicker-free)
    |-- /update endpoint for OTA firmware updates
    |
    v
ST7789 240x240 TFT Display
```

## Setup

### 1. Firmware

Install [PlatformIO](https://platformio.org/).

```bash
# Configure WiFi
cp include/config.example.h include/config.h
# Edit include/config.h with your WiFi credentials

# Build
pio run

# First flash (USB) - merge all bins for web flasher
pio pkg exec -- esptool.py --chip esp32s3 merge_bin \
  -o merged_firmware.bin --flash_mode dio --flash_size 4MB \
  0x0 .pio/build/esp32s3/bootloader.bin \
  0x8000 .pio/build/esp32s3/partitions.bin \
  0x10000 .pio/build/esp32s3/firmware.bin

# Flash via https://espressif.github.io/esptool-js/
# Erase flash first, then flash merged_firmware.bin at address 0x0
```

After the first flash, update over WiFi:
```bash
# Build and OTA
pio run
curl -F "firmware=@.pio/build/esp32s3/firmware.bin" http://<ESP32_IP>/update

# Or visit http://<ESP32_IP>/update in your browser
```

### 2. MCP Server

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

### 3. Dashboard Hook

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

### 4. Configure IP

Update `ESP32_IP` in both `mcp_server.py` and `update_display.py` to match your ESP32's IP address.

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/status` | GET | Device info (IP, RSSI, uptime, free heap, version) |
| `/help` | GET | API documentation |
| `/clear` | POST | Fill screen with color |
| `/text` | POST | Draw text at position with size/color |
| `/rect` | POST | Draw rectangle (filled/outline, rounded) |
| `/circle` | POST | Draw circle (filled/outline) |
| `/line` | POST | Draw line between two points |
| `/bar` | POST | Horizontal progress bar |
| `/gauge` | POST | Circular gauge |
| `/batch` | POST | Multiple commands in one request (framebuffered) |
| `/brightness` | POST | Set backlight (0-255) |
| `/update` | GET/POST | OTA firmware update page |

## Customization

- **Display layout**: Edit `update_display.py` — no firmware flash needed
- **Token pricing**: Update the `PRICE_*` constants in `update_display.py`
- **Drawing on the display**: Use the MCP tools from Claude, or POST directly to the HTTP API
- **Pin assignments**: Edit `include/config.h` and TFT_eSPI flags in `platformio.ini`

## License

MIT
