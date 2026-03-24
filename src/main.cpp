#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "config.h"

Preferences prefs;

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite fb = TFT_eSprite(&tft);
WebServer server(HTTP_PORT);

// ---- Screensaver state ----
#define SS_IDLE_MS    (5UL * 60 * 1000)  // 5 minutes
#define SS_FRAME_MS   40                  // ~25fps
#define MASCOT_W      56                  // 14 cols * 4px
#define MASCOT_H      35                  // 5 rows  * 7px
#define TRAIL_SEGS    6                   // one ghost per rainbow color
#define STAR_COUNT    30                  // background stars
#define RAINBOW_BANDS 6                   // must match TRAIL_SEGS

unsigned long lastDashboardMs = 0;
bool          ssActive        = false;

// Position & velocity
float ssX = 92.0f, ssY = 102.0f;
float ssDx = 1.3f, ssDy = 0.9f;

// Continuous squish cycle
int   ssSqFrame    = 0;
int   ssSqTick     = 0;
static const int SS_SQUISH_CYCLE[] = {0,0,0,0,1,2,1,0};
static const int SS_SQUISH_CYCLE_LEN = 8;

// Squish on bounce
int   ssBounceFrames = 0;
static const int SS_BOUNCE_SEQ[]  = {1,2,2,1,0};
static const int SS_BOUNCE_LEN    = 5;

// Trail: circular buffer of past positions
struct TrailPos { int x, y; };
TrailPos ssTrail[TRAIL_SEGS];
int      ssTrailHead  = 0;
bool     ssTrailFull  = false;
int      ssSampleTick = 0;
int      ssColorOff   = 0;  // cycles each frame to shift rainbow along tail
#define  TRAIL_SAMPLE 8     // sample position every N frames (~20px spacing)

// Stars
struct Star { int x, y; uint16_t col; };
Star ssStars[STAR_COUNT];

// Rainbow stripe colors (RGB565) — R,O,Y,G,B,V
static const uint16_t RAINBOW[RAINBOW_BANDS] = {
  0xF800, // red
  0xFC60, // orange
  0xFFE0, // yellow
  0x07E0, // green
  0x001F, // blue
  0x781F, // violet
};

unsigned long ssLastFrameMs = 0;

// ---- Helpers ----

uint16_t parseColor(const char* hex) {
  if (!hex || hex[0] != '#' || strlen(hex) != 7) return TFT_WHITE;
  long rgb = strtol(hex + 1, NULL, 16);
  uint8_t r = (rgb >> 16) & 0xFF;
  uint8_t g = (rgb >> 8) & 0xFF;
  uint8_t b = rgb & 0xFF;
  return tft.color565(r, g, b);
}

uint16_t getColor(JsonObject& obj, const char* key, uint16_t fallback) {
  if (obj[key].is<const char*>()) return parseColor(obj[key].as<const char*>());
  return fallback;
}

void sendOk(const char* msg = "ok") {
  server.send(200, "application/json", String("{\"status\":\"") + msg + "\"}");
}

void sendErr(const char* msg) {
  server.send(400, "application/json", String("{\"error\":\"") + msg + "\"}");
}

// ---- Handlers ----

void handleClear() {
  if (server.method() != HTTP_POST) { sendErr("POST only"); return; }
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  JsonObject obj = doc.as<JsonObject>();
  uint16_t color = getColor(obj, "color", TFT_BLACK);
  tft.fillScreen(color);
  sendOk();
}

void handleText() {
  if (server.method() != HTTP_POST) { sendErr("POST only"); return; }
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  JsonObject obj = doc.as<JsonObject>();

  const char* text = obj["text"] | "";
  int x = obj["x"] | 0;
  int y = obj["y"] | 0;
  int size = obj["size"] | 2;
  uint16_t fg = getColor(obj, "color", TFT_WHITE);
  uint16_t bg = getColor(obj, "bg", TFT_BLACK);

  tft.setTextColor(fg, bg);
  tft.setTextSize(size);

  // Optional: use built-in font number (1-8) or default
  int font = obj["font"] | 2;
  tft.setCursor(x, y);
  tft.setTextFont(font);
  tft.print(text);
  sendOk();
}

void handleRect() {
  if (server.method() != HTTP_POST) { sendErr("POST only"); return; }
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  JsonObject obj = doc.as<JsonObject>();

  int x = obj["x"] | 0;
  int y = obj["y"] | 0;
  int w = obj["w"] | 10;
  int h = obj["h"] | 10;
  uint16_t color = getColor(obj, "color", TFT_WHITE);
  bool filled = obj["filled"] | true;
  int radius = obj["radius"] | 0;

  if (radius > 0) {
    if (filled) tft.fillRoundRect(x, y, w, h, radius, color);
    else tft.drawRoundRect(x, y, w, h, radius, color);
  } else {
    if (filled) tft.fillRect(x, y, w, h, color);
    else tft.drawRect(x, y, w, h, color);
  }
  sendOk();
}

void handleCircle() {
  if (server.method() != HTTP_POST) { sendErr("POST only"); return; }
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  JsonObject obj = doc.as<JsonObject>();

  int x = obj["x"] | 120;
  int y = obj["y"] | 120;
  int r = obj["r"] | 10;
  uint16_t color = getColor(obj, "color", TFT_WHITE);
  bool filled = obj["filled"] | true;

  if (filled) tft.fillCircle(x, y, r, color);
  else tft.drawCircle(x, y, r, color);
  sendOk();
}

void handleLine() {
  if (server.method() != HTTP_POST) { sendErr("POST only"); return; }
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  JsonObject obj = doc.as<JsonObject>();

  int x0 = obj["x0"] | 0;
  int y0 = obj["y0"] | 0;
  int x1 = obj["x1"] | 240;
  int y1 = obj["y1"] | 240;
  uint16_t color = getColor(obj, "color", TFT_WHITE);

  tft.drawLine(x0, y0, x1, y1, color);
  sendOk();
}

void handleArc() {
  if (server.method() != HTTP_POST) { sendErr("POST only"); return; }
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  JsonObject obj = doc.as<JsonObject>();

  int x = obj["x"] | 120;
  int y = obj["y"] | 120;
  int r = obj["r"] | 50;
  int ir = obj["ir"] | 40;  // inner radius
  int startAngle = obj["start"] | 0;
  int endAngle = obj["end"] | 270;
  uint16_t fg = getColor(obj, "color", TFT_WHITE);
  uint16_t bg = getColor(obj, "bg", TFT_BLACK);
  bool smooth = obj["smooth"] | true;

  if (smooth) {
    tft.drawSmoothArc(x, y, r, ir, startAngle, endAngle, fg, bg, true);
  } else {
    tft.drawArc(x, y, r, ir, startAngle, endAngle, fg, bg, true);
  }
  sendOk();
}

void handleGauge() {
  if (server.method() != HTTP_POST) { sendErr("POST only"); return; }
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  JsonObject obj = doc.as<JsonObject>();

  int x = obj["x"] | 120;
  int y = obj["y"] | 120;
  int r = obj["r"] | 60;
  int ir = obj["ir"] | 50;
  float value = obj["value"] | 0.0f;  // 0.0 - 1.0
  const char* label = obj["label"] | "";
  uint16_t fg = getColor(obj, "color", TFT_GREEN);
  uint16_t bg = getColor(obj, "bg", TFT_BLACK);
  uint16_t track = getColor(obj, "track", 0x2104); // dark grey

  // Draw background track (full arc)
  tft.drawSmoothArc(x, y, r, ir, 0, 360, track, bg, true);
  // Draw value arc
  int endAngle = (int)(value * 360.0f);
  if (endAngle > 0) {
    tft.drawSmoothArc(x, y, r, ir, 0, endAngle, fg, bg, true);
  }
  // Draw label centered
  if (strlen(label) > 0) {
    tft.setTextColor(TFT_WHITE, bg);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4);
    tft.setTextSize(1);
    tft.drawString(label, x, y);
  }
  sendOk();
}

void handleBar() {
  if (server.method() != HTTP_POST) { sendErr("POST only"); return; }
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  JsonObject obj = doc.as<JsonObject>();

  int x = obj["x"] | 10;
  int y = obj["y"] | 100;
  int w = obj["w"] | 220;
  int h = obj["h"] | 20;
  float value = obj["value"] | 0.0f;  // 0.0 - 1.0
  uint16_t fg = getColor(obj, "color", TFT_GREEN);
  uint16_t bg = getColor(obj, "bg", TFT_BLACK);
  uint16_t track = getColor(obj, "track", 0x2104);
  int radius = obj["radius"] | 4;

  // Background
  tft.fillRoundRect(x, y, w, h, radius, track);
  // Fill
  int fw = (int)(w * value);
  if (fw > 0) {
    tft.fillRoundRect(x, y, fw, h, radius, fg);
  }
  sendOk();
}

void handleBrightness() {
  if (server.method() != HTTP_POST) { sendErr("POST only"); return; }
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  JsonObject obj = doc.as<JsonObject>();

  int level = obj["level"] | DEFAULT_BRIGHTNESS;
  level = constrain(level, 0, 255);
  ledcWrite(BACKLIGHT_PWM_CHANNEL, level);
  sendOk();
}

void handleBatch() {
  if (server.method() != HTTP_POST) { sendErr("POST only"); return; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) { sendErr("invalid json"); return; }

  JsonArray cmds = doc["commands"].as<JsonArray>();
  if (cmds.isNull()) { sendErr("missing commands array"); return; }

  for (JsonObject cmd : cmds) {
    const char* type = cmd["type"] | "";

    if (strcmp(type, "clear") == 0) {
      uint16_t color = getColor(cmd, "color", TFT_BLACK);
      fb.fillSprite(color);
    }
    else if (strcmp(type, "text") == 0) {
      const char* text = cmd["text"] | "";
      int x = cmd["x"] | 0;
      int y = cmd["y"] | 0;
      int size = cmd["size"] | 2;
      int font = cmd["font"] | 2;
      uint16_t fg = getColor(cmd, "color", TFT_WHITE);
      uint16_t bg = getColor(cmd, "bg", TFT_BLACK);
      fb.setTextColor(fg, bg);
      fb.setTextSize(size);
      fb.setTextFont(font);
      fb.setCursor(x, y);
      fb.print(text);
    }
    else if (strcmp(type, "rect") == 0) {
      int x = cmd["x"] | 0;
      int y = cmd["y"] | 0;
      int w = cmd["w"] | 10;
      int h = cmd["h"] | 10;
      uint16_t color = getColor(cmd, "color", TFT_WHITE);
      bool filled = cmd["filled"] | true;
      int radius = cmd["radius"] | 0;
      if (radius > 0) {
        if (filled) fb.fillRoundRect(x, y, w, h, radius, color);
        else fb.drawRoundRect(x, y, w, h, radius, color);
      } else {
        if (filled) fb.fillRect(x, y, w, h, color);
        else fb.drawRect(x, y, w, h, color);
      }
    }
    else if (strcmp(type, "circle") == 0) {
      int x = cmd["x"] | 120;
      int y = cmd["y"] | 120;
      int r = cmd["r"] | 10;
      uint16_t color = getColor(cmd, "color", TFT_WHITE);
      bool filled = cmd["filled"] | true;
      if (filled) fb.fillCircle(x, y, r, color);
      else fb.drawCircle(x, y, r, color);
    }
    else if (strcmp(type, "line") == 0) {
      int x0 = cmd["x0"] | 0;
      int y0 = cmd["y0"] | 0;
      int x1 = cmd["x1"] | 240;
      int y1 = cmd["y1"] | 240;
      uint16_t color = getColor(cmd, "color", TFT_WHITE);
      fb.drawLine(x0, y0, x1, y1, color);
    }
    else if (strcmp(type, "bar") == 0) {
      int x = cmd["x"] | 10;
      int y = cmd["y"] | 100;
      int w = cmd["w"] | 220;
      int h = cmd["h"] | 20;
      float value = cmd["value"] | 0.0f;
      uint16_t fg = getColor(cmd, "color", TFT_GREEN);
      uint16_t track = getColor(cmd, "track", 0x2104);
      int radius = cmd["radius"] | 4;
      fb.fillRoundRect(x, y, w, h, radius, track);
      int fw = (int)(w * value);
      if (fw > 0) fb.fillRoundRect(x, y, fw, h, radius, fg);
    }
    else if (strcmp(type, "gauge") == 0) {
      int x = cmd["x"] | 120;
      int y = cmd["y"] | 120;
      int r = cmd["r"] | 60;
      int ir = cmd["ir"] | 50;
      float value = cmd["value"] | 0.0f;
      const char* label = cmd["label"] | "";
      uint16_t fg = getColor(cmd, "color", TFT_GREEN);
      uint16_t bg = getColor(cmd, "bg", TFT_BLACK);
      uint16_t track = getColor(cmd, "track", 0x2104);
      fb.drawSmoothArc(x, y, r, ir, 0, 360, track, bg, true);
      int endAngle = (int)(value * 360.0f);
      if (endAngle > 0) fb.drawSmoothArc(x, y, r, ir, 0, endAngle, fg, bg, true);
      if (strlen(label) > 0) {
        fb.setTextColor(TFT_WHITE, bg);
        fb.setTextDatum(MC_DATUM);
        fb.setTextFont(4);
        fb.setTextSize(1);
        fb.drawString(label, x, y);
      }
    }
  }
  fb.pushSprite(0, 0);
  sendOk();
}

// ---- Dashboard helpers ----

static const uint16_t C_BLACK   = TFT_BLACK;
// Footer background — computed at runtime to avoid RGB565 conversion errors
#define C_BG_FOOT parseColor("#0a0a15")

// Scale RGB565 color brightness: scale 0.0=black, 1.0=full
uint16_t dimColor(uint16_t col, float scale) {
  uint8_t r = ((col >> 11) & 0x1F);
  uint8_t g = ((col >> 5)  & 0x3F);
  uint8_t b = ( col        & 0x1F);
  r = (uint8_t)(r * scale);
  g = (uint8_t)(g * scale);
  b = (uint8_t)(b * scale);
  return (r << 11) | (g << 5) | b;
}

uint16_t contextColor(float pct) {
  if (pct < 0.5f) return parseColor("#00FF88");
  if (pct < 0.75f) return parseColor("#F39C12");
  return parseColor("#E74C3C");
}

// Format integer with commas: 123456 -> "123,456"
void fmtComma(char* buf, int n) {
  char tmp[24]; sprintf(tmp, "%d", n);
  int len = strlen(tmp), out = 0, commas = (len - 1) / 3;
  int lead = len - commas * 3;
  for (int i = 0; i < len; i++) {
    if (i == lead && i > 0) buf[out++] = ',';
    else if (i > lead && (i - lead) % 3 == 0) buf[out++] = ',';
    buf[out++] = tmp[i];
  }
  buf[out] = '\0';
}

// Format abbreviated: 1234 -> "1.2k", 1234567 -> "1.2M"
void fmtK(char* buf, int n) {
  if (n < 1000) { sprintf(buf, "%d", n); }
  else if (n < 10000) { sprintf(buf, "%.1fk", n / 1000.0f); }
  else if (n < 1000000) { sprintf(buf, "%dk", n / 1000); }
  else { sprintf(buf, "%.1fM", n / 1000000.0f); }
}

void drawMascotAt(int ox, int oy, int squish, uint16_t col = 0, bool clearBg = true) {
  int sw = 4, sh = 7;
  if (col == 0) col = parseColor("#D97757");
  uint16_t blk = TFT_BLACK;

  // Each squish state: list of (col, row) block coords + eye coords
  // squish 0 = normal, 1 = slight, 2 = flat
  struct { int c, r; } body0[] = {
    {2,0},{3,0},{4,0},{5,0},{6,0},{7,0},{8,0},{9,0},{10,0},{11,0},
    {2,1},{3,1},{4,1},{5,1},{6,1},{7,1},{8,1},{9,1},{10,1},{11,1},
    {0,2},{1,2},{2,2},{3,2},{4,2},{5,2},{6,2},{7,2},{8,2},{9,2},{10,2},{11,2},{12,2},{13,2},
    {2,3},{3,3},{4,3},{5,3},{6,3},{7,3},{8,3},{9,3},{10,3},{11,3},
    {3,4},{5,4},{8,4},{10,4}
  };
  struct { int c, r; } eyes0[] = {{4,1},{9,1}};

  struct { int c, r; } body1[] = {
    {2,1},{3,1},{4,1},{5,1},{6,1},{7,1},{8,1},{9,1},{10,1},{11,1},
    {0,2},{1,2},{2,2},{3,2},{4,2},{5,2},{6,2},{7,2},{8,2},{9,2},{10,2},{11,2},{12,2},{13,2},
    {2,3},{3,3},{4,3},{5,3},{6,3},{7,3},{8,3},{9,3},{10,3},{11,3},
    {2,4},{3,4},{4,4},{5,4},{6,4},{7,4},{8,4},{9,4},{10,4},{11,4}
  };
  struct { int c, r; } eyes1[] = {{4,2},{9,2}};

  struct { int c, r; } body2[] = {
    {0,2},{1,2},{2,2},{3,2},{4,2},{5,2},{6,2},{7,2},{8,2},{9,2},{10,2},{11,2},{12,2},{13,2},
    {0,3},{1,3},{2,3},{3,3},{4,3},{5,3},{6,3},{7,3},{8,3},{9,3},{10,3},{11,3},{12,3},{13,3},
    {0,4},{1,4},{2,4},{3,4},{4,4},{5,4},{6,4},{7,4},{8,4},{9,4},{10,4},{11,4},{12,4},{13,4}
  };
  struct { int c, r; } eyes2[] = {{4,2},{9,2}};

  // clear mascot area (skip for ghost trail copies)
  if (clearBg) fb.fillRect(ox, oy, 14*sw, 5*sh, TFT_BLACK);

  if (squish == 0) {
    for (auto& b : body0) fb.fillRect(ox+b.c*sw, oy+b.r*sh, sw, sh, col);
    for (auto& e : eyes0) fb.fillRect(ox+e.c*sw+1, oy+e.r*sh+1, sw-2, sh-2, blk);
  } else if (squish == 1) {
    for (auto& b : body1) fb.fillRect(ox+b.c*sw, oy+b.r*sh, sw, sh, col);
    for (auto& e : eyes1) fb.fillRect(ox+e.c*sw+1, oy+e.r*sh+1, sw-2, sh-2, blk);
  } else {
    for (auto& b : body2) fb.fillRect(ox+b.c*sw, oy+b.r*sh, sw, sh, col);
    for (auto& e : eyes2) fb.fillRect(ox+e.c*sw+1, oy+e.r*sh+1, sw-2, sh-2, blk);
  }
}

// Dashboard uses fixed position
void drawMascot(int squish) { drawMascotAt(170, 48, squish); }

// ---- Screensaver ----

void drawNyanFrame() {
  fb.fillSprite(TFT_BLACK);

  // Twinkle stars
  for (int i = 0; i < 4; i++) {
    int idx = random(0, STAR_COUNT);
    int r = random(0, 3);
    if (r == 0)      ssStars[idx].col = TFT_WHITE;
    else if (r == 1) ssStars[idx].col = 0x4208;
    else             ssStars[idx].col = TFT_BLACK;
  }
  for (int i = 0; i < STAR_COUNT; i++) {
    fb.fillRect(ssStars[i].x, ssStars[i].y, 2, 2, ssStars[i].col);
  }

  // Draw ghost trail — oldest first so newer ones paint over
  // Color offset shifts each frame so colors cycle along the tail (Mario Kart rainbow road)
  int trailCount = ssTrailFull ? TRAIL_SEGS : ssTrailHead;
  for (int s = trailCount - 1; s >= 0; s--) {
    int idx = (ssTrailHead - 1 - s + TRAIL_SEGS) % TRAIL_SEGS;
    // s=0 newest, s=trailCount-1 oldest
    // Color cycles: add ssColorOff so the whole tail shifts each frame
    uint16_t col = RAINBOW[(RAINBOW_BANDS - 1 - s + ssColorOff) % RAINBOW_BANDS];
    float brightness = 1.0f - (float)s / TRAIL_SEGS * 0.85f;  // 100% newest -> 15% oldest
    col = dimColor(col, brightness);
    drawMascotAt(ssTrail[idx].x, ssTrail[idx].y, 0, col, false);
  }

  // Advance squish
  int squish;
  if (ssBounceFrames > 0) {
    squish = SS_BOUNCE_SEQ[SS_BOUNCE_LEN - ssBounceFrames];
    ssBounceFrames--;
  } else {
    ssSqTick++;
    if (ssSqTick >= 3) { ssSqTick = 0; ssSqFrame = (ssSqFrame + 1) % SS_SQUISH_CYCLE_LEN; }
    squish = SS_SQUISH_CYCLE[ssSqFrame];
  }

  // Draw main mascot (Claude orange) on top
  drawMascotAt((int)ssX, (int)ssY, squish, 0, false);
  fb.pushSprite(0, 0);
}

void tickScreensaver() {
  unsigned long now = millis();
  if (now - ssLastFrameMs < SS_FRAME_MS) return;
  ssLastFrameMs = now;

  // Sample position into trail buffer every N frames for visible spacing
  ssSampleTick++;
  if (ssSampleTick >= TRAIL_SAMPLE) {
    ssSampleTick = 0;
    ssTrail[ssTrailHead] = {(int)ssX, (int)ssY};
    ssTrailHead = (ssTrailHead + 1) % TRAIL_SEGS;
    if (ssTrailHead == 0) ssTrailFull = true;
  }

  // Advance color offset every 2 frames
  static int ssColorTick = 0;
  if (++ssColorTick >= 2) { ssColorTick = 0; ssColorOff = (ssColorOff + 1) % RAINBOW_BANDS; }

  // Move
  ssX += ssDx;
  ssY += ssDy;

  // Bounce
  if (ssX <= 0)              { ssX = 0;               ssDx = fabsf(ssDx);  ssBounceFrames = SS_BOUNCE_LEN; }
  if (ssX >= 240 - MASCOT_W) { ssX = 240 - MASCOT_W;  ssDx = -fabsf(ssDx); ssBounceFrames = SS_BOUNCE_LEN; }
  if (ssY <= 0)              { ssY = 0;               ssDy = fabsf(ssDy);  ssBounceFrames = SS_BOUNCE_LEN; }
  if (ssY >= 240 - MASCOT_H) { ssY = 240 - MASCOT_H;  ssDy = -fabsf(ssDy); ssBounceFrames = SS_BOUNCE_LEN; }

  drawNyanFrame();
}

void startScreensaver() {
  ssActive       = true;
  ssTrailHead    = 0;
  ssTrailFull    = false;
  ssSampleTick   = 0;
  ssColorOff     = 0;
  ssSqFrame      = 0;
  ssSqTick       = 0;
  ssBounceFrames = 0;

  // Scatter stars randomly across the screen
  for (int i = 0; i < STAR_COUNT; i++) {
    ssStars[i].x   = random(0, 238);
    ssStars[i].y   = random(0, 238);
    // Stars twinkle between white and light grey
    ssStars[i].col = (random(0, 2) == 0) ? TFT_WHITE : 0xC618;
  }

  fb.fillSprite(TFT_BLACK);
  fb.pushSprite(0, 0);
  ssLastFrameMs = millis();
}

void stopScreensaver() {
  ssActive = false;
}

void drawDashboardFrame(const char* project, const char* model,
                        int turns, int ctx_used, int ctx_max) {
  fb.fillSprite(TFT_BLACK);

  // Header
  char header[17]; strncpy(header, project[0] ? project : "Claude Session", 16); header[16] = '\0';
  fb.setTextColor(parseColor("#7B68EE"), TFT_BLACK);
  fb.setTextSize(2); fb.setTextFont(2);
  fb.setCursor(10, 8); fb.print(header);

  // Labels
  fb.setTextColor(parseColor("#666666"), TFT_BLACK);
  fb.setCursor(10, 88); fb.print("IN");
  fb.setCursor(10, 122); fb.print("OUT");

  // Turns
  char tbuf[32]; sprintf(tbuf, "Turns: %d", turns);
  fb.setTextColor(parseColor("#7B68EE"), TFT_BLACK);
  fb.setCursor(10, 152); fb.print(tbuf);

  // Context bar
  float ctx_pct = ctx_max > 0 ? (float)ctx_used / ctx_max : 0.0f;
  char ctxbuf[32]; char ckbuf[12]; char cmkbuf[12];
  fmtK(ckbuf, ctx_used); fmtK(cmkbuf, ctx_max);
  sprintf(ctxbuf, "Context %s/%s (%d%%)", ckbuf, cmkbuf, (int)(ctx_pct * 100));
  fb.setTextColor(TFT_WHITE, TFT_BLACK);
  fb.setTextSize(1); fb.setTextFont(2);
  fb.setCursor(10, 182); fb.print(ctxbuf);

  uint16_t barCol = contextColor(ctx_pct);
  fb.fillRoundRect(10, 196, 220, 18, 6, parseColor("#1a1a2e"));
  int bw = (int)(220 * ctx_pct);
  if (bw > 0) fb.fillRoundRect(10, 196, bw, 18, 6, barCol);

  // Footer
  fb.fillRect(0, 224, 240, 16, C_BG_FOOT);
  char footer[32]; sprintf(footer, "claude code | %s", model[0] ? model : "claude");
  fb.setTextColor(TFT_WHITE, C_BG_FOOT);
  fb.setCursor(30, 226); fb.print(footer);
}

void drawDashboardValues(float cost, int in_tok, int out_tok) {
  char cbuf[16]; sprintf(cbuf, "$%.2f", cost);
  char ibuf[20]; fmtComma(ibuf, in_tok);
  char obuf[20]; fmtComma(obuf, out_tok);

  // Clear value areas
  fb.fillRect(5, 40, 160, 46, TFT_BLACK);
  fb.fillRect(43, 85, 195, 34, TFT_BLACK);
  fb.fillRect(58, 119, 180, 30, TFT_BLACK);

  fb.setTextSize(3); fb.setTextFont(2);
  fb.setTextColor(parseColor("#00FF88"), TFT_BLACK);
  fb.setCursor(10, 45); fb.print(cbuf);

  fb.setTextSize(2); fb.setTextFont(2);
  fb.setTextColor(parseColor("#5DADE2"), TFT_BLACK);
  fb.setCursor(45, 88); fb.print(ibuf);

  fb.setTextColor(parseColor("#F39C12"), TFT_BLACK);
  fb.setCursor(60, 122); fb.print(obuf);
}

void handleDashboard() {
  if (server.method() != HTTP_POST) { sendErr("POST only"); return; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) { sendErr("invalid json"); return; }

  const char* project  = doc["project"]     | "";
  const char* model    = doc["model"]       | "claude";
  float  cost          = doc["cost"]        | 0.0f;
  float  cost_prev     = doc["cost_prev"]   | 0.0f;
  int    in_tok        = doc["in_tokens"]   | 0;
  int    in_prev       = doc["in_prev"]     | 0;
  int    out_tok       = doc["out_tokens"]  | 0;
  int    out_prev      = doc["out_prev"]    | 0;
  int    turns         = doc["turns"]       | 0;
  int    ctx_used      = doc["context_used"]| 0;
  int    ctx_max       = doc["context_max"] | 1000000;

  // Respond immediately so hook can exit
  sendOk();
  server.client().stop();

  lastDashboardMs = millis();
  stopScreensaver();

  // Draw static frame once
  drawDashboardFrame(project, model, turns, ctx_used, ctx_max);
  drawDashboardValues(cost_prev, in_prev, out_prev);
  drawMascot(0);
  fb.pushSprite(0, 0);

  // Animate count-up + mascot squish
  static const int squish_seq[] = {0,1,2,1,0,1,2,1,0,0};
  int frames = 10;
  for (int i = 1; i <= frames; i++) {
    float frac = (float)i / frames;
    float cur_cost = cost_prev + (cost - cost_prev) * frac;
    int   cur_in   = (int)(in_prev  + (in_tok  - in_prev)  * frac);
    int   cur_out  = (int)(out_prev + (out_tok  - out_prev) * frac);
    drawDashboardValues(cur_cost, cur_in, cur_out);
    drawMascot(squish_seq[i - 1]);
    fb.pushSprite(0, 0);
    if (i < frames) delay(80);
  }
}

void handleStatus() {
  JsonDocument doc;
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["ssid"] = WiFi.SSID();
  doc["width"] = 240;
  doc["height"] = 240;
  doc["uptime_s"] = millis() / 1000;
  doc["free_heap"] = ESP.getFreeHeap();
  doc["version"] = FW_VERSION;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleHelp() {
  String help = R"HELP({
  "endpoints": {
    "GET /status": "Device info (IP, RSSI, uptime, display size)",
    "GET /help": "This help message",
    "POST /clear": {"color": "#000000"},
    "POST /text": {"text":"hello","x":0,"y":0,"size":2,"font":2,"color":"#FFFFFF","bg":"#000000"},
    "POST /rect": {"x":0,"y":0,"w":100,"h":50,"color":"#FFFFFF","filled":true,"radius":0},
    "POST /circle": {"x":120,"y":120,"r":50,"color":"#FFFFFF","filled":true},
    "POST /line": {"x0":0,"y0":0,"x1":240,"y1":240,"color":"#FFFFFF"},
    "POST /arc": {"x":120,"y":120,"r":60,"ir":50,"start":0,"end":270,"color":"#FFFFFF","bg":"#000000","smooth":true},
    "POST /gauge": {"x":120,"y":120,"r":60,"ir":50,"value":0.75,"label":"75%","color":"#00FF00","bg":"#000000","track":"#333333"},
    "POST /bar": {"x":10,"y":100,"w":220,"h":20,"value":0.5,"color":"#00FF00","track":"#333333","radius":4},
    "POST /brightness": {"level": 200},
    "POST /batch": {"commands": [{"type":"clear"}, {"type":"text","text":"hi","x":0,"y":0}]}
  },
  "notes": {
    "colors": "Hex format #RRGGBB",
    "display": "240x240 pixels",
    "batch": "Send multiple commands in one request for flicker-free updates"
  }
})HELP";
  server.send(200, "application/json", help);
}

// ---- Setup & Loop ----

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[display-server] Starting...");

  // Backlight PWM
  ledcSetup(BACKLIGHT_PWM_CHANNEL, BACKLIGHT_PWM_FREQ, BACKLIGHT_PWM_RES);
  ledcAttachPin(BACKLIGHT_PIN, BACKLIGHT_PWM_CHANNEL);
  ledcWrite(BACKLIGHT_PWM_CHANNEL, DEFAULT_BRIGHTNESS);

  // Display init
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  // Create framebuffer in PSRAM for flicker-free batch updates
  fb.createSprite(240, 240);
  fb.fillSprite(TFT_BLACK);
  Serial.printf("[display-server] Framebuffer created, PSRAM free: %d\n", ESP.getFreePsram());

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextFont(2);
  tft.setCursor(10, 10);
  tft.println("Connecting WiFi...");
  Serial.println("[display-server] Display initialized");

  // Load WiFi creds from NVS
  prefs.begin("wifi", false);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();

  // Try STA connection only if we have creds
  if (ssid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("[display-server] Connecting to %s", ssid.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[display-server] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(10, 10);
    tft.setTextFont(2);
    tft.println("WiFi Connected!");
    tft.println();
    tft.setTextFont(4);
    tft.println(WiFi.localIP().toString());
    tft.println();
    tft.setTextFont(2);
    tft.printf("RSSI: %d dBm\n", WiFi.RSSI());
    tft.printf("Port: %d\n", HTTP_PORT);
    tft.println();
    tft.printf("%s.local\n", MDNS_HOSTNAME);
    tft.printf("v%s\n", FW_VERSION);

    // mDNS
    if (MDNS.begin(MDNS_HOSTNAME)) {
      Serial.printf("[display-server] mDNS: http://%s.local\n", MDNS_HOSTNAME);
    }
  } else {
    // No connection — start AP mode for configuration
    Serial.println("\n[display-server] WiFi failed, starting AP mode");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32-Display-Setup");

    bool had_creds = ssid.length() > 0;

    tft.fillScreen(TFT_BLACK);
    tft.setCursor(10, 10);
    tft.setTextFont(2);
    // Title line — yellow if no creds, orange if creds failed
    tft.setTextColor(had_creds ? TFT_ORANGE : TFT_YELLOW, TFT_BLACK);
    tft.println(had_creds ? "WiFi Failed - Setup Mode" : "No WiFi Creds - Setup");
    tft.println();
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    if (had_creds) {
      tft.printf("Failed SSID:\n");
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.println(ssid.c_str());
      tft.println();
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
    }
    tft.println("1. Connect to:");
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.println("  ESP32-Display-Setup");
    tft.println();
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.println("2. Open browser:");
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.println("  192.168.4.1");
    tft.println();
    tft.setTextColor(parseColor("#666666"), TFT_BLACK);
    tft.printf("v%s\n", FW_VERSION);

    // Serve config page in AP mode
    server.on("/", HTTP_GET, []() {
      server.send(200, "text/html",
        "<h2>ESP32 Display Setup</h2>"
        "<form method='POST' action='/wifi'>"
        "SSID: <input name='ssid' length=32><br><br>"
        "Password: <input name='pass' type='password' length=64><br><br>"
        "<input type='submit' value='Save &amp; Connect'>"
        "</form>");
    });
    server.on("/wifi", HTTP_POST, []() {
      String new_ssid = server.arg("ssid");
      String new_pass = server.arg("pass");
      if (new_ssid.length() == 0) {
        server.send(400, "text/plain", "SSID required");
        return;
      }
      prefs.begin("wifi", false);
      prefs.putString("ssid", new_ssid);
      prefs.putString("pass", new_pass);
      prefs.end();
      server.send(200, "text/html",
        "<h2>Saved!</h2><p>Rebooting to connect to <b>" + new_ssid + "</b>...</p>");
      delay(1500);
      ESP.restart();
    });
    server.begin();
    Serial.println("[display-server] AP config server running");
    // Loop forever in AP mode — reboot handles the rest
    while (true) { server.handleClient(); delay(10); }
  }

  // Register routes
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/help", HTTP_GET, handleHelp);
  server.on("/clear", HTTP_POST, handleClear);
  server.on("/text", HTTP_POST, handleText);
  server.on("/rect", HTTP_POST, handleRect);
  server.on("/circle", HTTP_POST, handleCircle);
  server.on("/line", HTTP_POST, handleLine);
  server.on("/arc", HTTP_POST, handleArc);
  server.on("/gauge", HTTP_POST, handleGauge);
  server.on("/bar", HTTP_POST, handleBar);
  server.on("/brightness", HTTP_POST, handleBrightness);
  server.on("/batch", HTTP_POST, handleBatch);
  server.on("/dashboard", HTTP_POST, handleDashboard);
  server.on("/wifi", HTTP_POST, []() {
    // Update WiFi credentials and reboot
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    String new_ssid = doc["ssid"] | "";
    String new_pass = doc["pass"] | "";
    if (new_ssid.length() == 0) { sendErr("ssid required"); return; }
    prefs.begin("wifi", false);
    prefs.putString("ssid", new_ssid);
    prefs.putString("pass", new_pass);
    prefs.end();
    sendOk("saved, rebooting");
    delay(500);
    ESP.restart();
  });
  server.on("/screensaver", HTTP_POST, []() {
    startScreensaver();
    sendOk("screensaver started");
  });
  server.on("/wifi/clear", HTTP_POST, []() {
    prefs.begin("wifi", false);
    prefs.clear();
    prefs.end();
    sendOk("wifi creds cleared, rebooting");
    delay(500);
    ESP.restart();
  });

  // OTA: POST firmware binary to /update, or GET /update for upload page
  server.on("/update", HTTP_POST, []() {
    bool ok = !Update.hasError();
    server.send(200, "text/plain", ok ? "OK, rebooting..." : "Update failed");
    if (ok) { delay(500); ESP.restart(); }
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.println("[OTA] Starting update...");
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_END) {
      if (!Update.end(true)) Update.printError(Serial);
      else Serial.println("[OTA] Update complete");
    }
  });
  server.on("/update", HTTP_GET, []() {
    server.send(200, "text/html",
      "<h2>ESP32 Display OTA</h2>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='firmware'><br><br>"
      "<input type='submit' value='Upload Firmware'></form>");
  });

  server.begin();
  Serial.printf("[display-server] HTTP server + OTA running on port %d\n", HTTP_PORT);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();
  }

  // Auto-start screensaver after idle timeout
  if (!ssActive && (millis() > SS_IDLE_MS) && (lastDashboardMs == 0 || (millis() - lastDashboardMs > SS_IDLE_MS))) {
    startScreensaver();
  }

  if (ssActive) {
    tickScreensaver();
  }
}
