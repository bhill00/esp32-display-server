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
#define SS_FRAME_MS   40                  // ~25fps
#define MASCOT_W      56                  // 14 cols * 4px
#define MASCOT_H      35                  // 5 rows  * 7px
#define TRAIL_SEGS    6                   // one ghost per rainbow color
#define STAR_COUNT    20                  // kept for size compat, replaced by STARS_PER_LAYER
#define RAINBOW_BANDS 6                   // must match TRAIL_SEGS

unsigned long lastDashboardMs = 0;
bool          ssActive        = false;

// Header scroll state
#define HDR_ICON_W   26   // folder icon + gap, text starts here
#define HDR_TEXT_W  209   // 235 - HDR_ICON_W, available text width
#define HDR_H        32   // header row height — tall enough for font2 textSize2 (~26px) + top offset
#define HDR_TEXT_Y    2   // y offset for text within header row (static and scroll must match)
#define HDR_SCROLL_MS 30  // ms per pixel scroll
char          hdrProject[64]   = "";
int           hdrTextPxW       = 0;     // pixel width of full project string
bool          hdrNeedsScroll   = false;
float         hdrScrollX       = 0.0f;  // current scroll offset (0 = start)
unsigned long hdrLastScrollMs  = 0;

enum SsMode { SS_NYAN, SS_DRIFT, SS_INVADERS, SS_RANDOM, SS_OFF };
SsMode ssMode       = SS_NYAN;
unsigned long ssIdleMs    = 5UL * 60 * 1000;  // idle timeout, 0 = never
bool          hdrScrollEnabled = true;          // false = truncate long names

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

// Drift mode: rotation + depth
float ssDriftAngle  = 0.0f;   // degrees, increments each frame
float ssDriftScale  = 1.0f;   // 0.25 (far) .. 1.0 (close)
int   ssBounceCount = 0;      // total bounces since last direction change
int   ssDriftDir    = -1;     // -1 shrinking (going away), +1 growing (coming toward)
#define DRIFT_SHRINK_BOUNCES  4
#define DRIFT_GROW_BOUNCES    8
#define DRIFT_SCALE_MIN  0.25f
#define DRIFT_SCALE_MAX  1.0f

// Trail: circular buffer of past positions, each ghost keeps its assigned color
struct TrailPos { int x, y; uint16_t color; };
TrailPos ssTrail[TRAIL_SEGS];
int      ssTrailHead  = 0;
bool     ssTrailFull  = false;
int      ssSampleTick = 0;
int      ssColorOff   = 0;  // advances by 1 each time a new ghost is born
#define  TRAIL_SAMPLE 8     // sample position every N frames (~20px spacing)

// Parallax starfield — 3 layers, each scrolling at different speeds
#define STAR_LAYERS   3
#define STARS_PER_LAYER 20

struct Star { float x; int y; };  // float x for sub-pixel scroll

// Layer config: speed (px/frame), size (px), brightness
static const float  STAR_SPEED[STAR_LAYERS]  = { 0.2f, 0.5f, 1.1f };
static const int    STAR_SIZE[STAR_LAYERS]   = { 1,    1,    2    };
static const uint16_t STAR_BRIGHT[STAR_LAYERS] = { 0x4208, 0x8410, 0xFFFF }; // dim, mid, bright

Star ssStars[STAR_LAYERS][STARS_PER_LAYER];

// UFO flyby
struct Ufo { float x; int y; bool active; float speed; };
Ufo ssUfo = { 0, 0, false, 0 };
unsigned long ssUfoNextMs = 0;  // when to spawn next UFO

// ---- Space Invaders screensaver ----
#define INV_COLS       8
#define INV_ROWS       4
#define INV_SCALE      0.45f          // mascot pixel size
#define INV_W          13             // alien width at scale (14*0.45 rounded)
#define INV_H          10             // alien height at scale (5*7*0.45 rounded... padded)
#define INV_XGAP       26             // horizontal spacing between alien centers
#define INV_YGAP       18             // vertical spacing
#define INV_XSTART     14             // left edge of grid
#define INV_YSTART     18             // top edge of grid
#define INV_MAX_ABULS  3              // max alien bullets active at once
#define INV_PLAYER_Y   226            // player ship y position
#define INV_PLAYER_W   16             // player ship width
#define INV_BULLET_SPD 4              // player bullet speed (px/frame)
#define INV_ABUL_SPD   2              // alien bullet speed

bool     invAlive[INV_ROWS][INV_COLS];
float    invGridX   = 0.0f;   // grid x offset from start
int      invGridY   = 0;      // grid y offset from start
float    invDx      = 0.4f;   // current horizontal step per frame
bool     invMoving  = true;
int      invAliveCount = INV_ROWS * INV_COLS;
int      invScore      = 0;
bool     invGameOver   = false;
int      invGameOverFrames = 0;  // countdown before reset

float    invPlayerX = 112.0f; // player ship center x
int      invPlayerLives = 3;
bool     invPlayerHit   = false;
int      invPlayerHitFrames = 0;

// Player bullet
bool     invBulActive = false;
float    invBulX = 0, invBulY = 0;

// Alien bullets
struct InvBul { float x, y; bool active; };
InvBul   invAbuls[INV_MAX_ABULS];

int      invFireTick  = 0;    // countdown to next alien fire
int      invAiTick    = 0;    // AI movement tick
int      invWavePause = 0;    // frames to pause after wave clear

// Row colors
static const uint16_t INV_ROW_COLOR[INV_ROWS] = {
  0xF800,  // red    — top row
  0xFC60,  // orange
  0x07E0,  // green
  0x001F,  // blue   — bottom row
};

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

void drawMascotAt(int ox, int oy, int squish, uint16_t col = 0, bool clearBg = true, float scale = 1.0f) {
  int sw = max(1, (int)(4 * scale));
  int sh = max(1, (int)(7 * scale));
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
void drawInvadersFrame();  // forward declaration

void drawNyanFrame() {
  fb.fillSprite(TFT_BLACK);

  // Scroll and draw parallax star layers
  for (int l = 0; l < STAR_LAYERS; l++) {
    for (int i = 0; i < STARS_PER_LAYER; i++) {
      ssStars[l][i].x -= STAR_SPEED[l];
      if (ssStars[l][i].x < 0) ssStars[l][i].x += 240.0f;  // wrap
      // Subtle twinkle: 1 in 20 chance of dimming slightly
      uint16_t col = (random(0, 20) == 0) ? dimColor(STAR_BRIGHT[l], 0.3f) : STAR_BRIGHT[l];
      fb.fillRect((int)ssStars[l][i].x, ssStars[l][i].y, STAR_SIZE[l], STAR_SIZE[l], col);
    }
  }

  // UFO flyby
  unsigned long now2 = millis();
  if (!ssUfo.active && now2 >= ssUfoNextMs) {
    ssUfo.active = true;
    ssUfo.x      = 245.0f;  // start just off right edge
    ssUfo.y      = random(10, 220);
    ssUfo.speed  = 1.5f + random(0, 20) * 0.1f;  // 1.5–3.5 px/frame
  }
  if (ssUfo.active) {
    ssUfo.x -= ssUfo.speed;
    if (ssUfo.x < -30) {
      ssUfo.active  = false;
      ssUfoNextMs   = now2 + 15000 + random(0, 20000);  // 15–35s until next
    } else {
      int ux = (int)ssUfo.x, uy = ssUfo.y;
      // Body: 12x5 cyan oval-ish
      fb.fillRect(ux+2, uy,   8, 2, 0x07FF); // top dome
      fb.fillRect(ux,   uy+2, 12, 3, 0x07FF); // main body
      // Dome highlight
      fb.fillRect(ux+4, uy,   4, 2, 0xFFFF);
      // Underside lights — alternating yellow/red
      fb.fillRect(ux+1,  uy+5, 2, 1, 0xFFE0);
      fb.fillRect(ux+5,  uy+5, 2, 1, 0xF800);
      fb.fillRect(ux+9,  uy+5, 2, 1, 0xFFE0);
    }
  }

  // Draw ghost trail — oldest first so newer ones paint over
  // Each ghost keeps its birth color, just dims with age
  int trailCount = ssTrailFull ? TRAIL_SEGS : ssTrailHead;
  for (int s = trailCount - 1; s >= 0; s--) {
    int idx = (ssTrailHead - 1 - s + TRAIL_SEGS) % TRAIL_SEGS;
    // s=0 newest, s=trailCount-1 oldest
    float brightness = 1.0f - (float)s / (float)TRAIL_SEGS * 0.85f;  // 100% -> 15%
    uint16_t col = dimColor(ssTrail[idx].color, brightness);
    drawMascotAt(ssTrail[idx].x, ssTrail[idx].y, 0, col, false);
  }

  // Squish only on edge bounce
  int squish = 0;
  if (ssBounceFrames > 0) {
    squish = SS_BOUNCE_SEQ[SS_BOUNCE_LEN - ssBounceFrames];
    ssBounceFrames--;
  }

  // Draw main mascot (Claude orange) on top
  drawMascotAt((int)ssX, (int)ssY, squish, 0, false);
  fb.pushSprite(0, 0);
}

void drawDriftFrame() {
  // Draw star background into fb, push to TFT first
  fb.fillSprite(TFT_BLACK);
  for (int l = 0; l < STAR_LAYERS; l++) {
    for (int i = 0; i < STARS_PER_LAYER; i++) {
      ssStars[l][i].x -= STAR_SPEED[l];
      if (ssStars[l][i].x < 0) ssStars[l][i].x += 240.0f;
      uint16_t col = (random(0, 20) == 0) ? dimColor(STAR_BRIGHT[l], 0.3f) : STAR_BRIGHT[l];
      fb.fillRect((int)ssStars[l][i].x, ssStars[l][i].y, STAR_SIZE[l], STAR_SIZE[l], col);
    }
  }
  // Build mascot sprite at scaled pixel size
  int psw = max(1, (int)(4.0f * ssDriftScale));
  int psh = max(1, (int)(7.0f * ssDriftScale));
  int mw  = 14 * psw;
  int mh  =  5 * psh;

  // Sprite large enough to hold mascot at any rotation angle
  int sprSz = (int)(sqrtf((float)(mw*mw + mh*mh))) + 4;
  TFT_eSprite tmp = TFT_eSprite(&tft);
  tmp.createSprite(sprSz, sprSz);
  tmp.fillSprite(TFT_BLACK);

  // Draw mascot centered in tmp sprite
  int ox = (sprSz - mw) / 2;
  int oy = (sprSz - mh) / 2;
  uint16_t col = parseColor("#D97757");
  struct { int c, r; } body[] = {
    {2,0},{3,0},{4,0},{5,0},{6,0},{7,0},{8,0},{9,0},{10,0},{11,0},
    {2,1},{3,1},{4,1},{5,1},{6,1},{7,1},{8,1},{9,1},{10,1},{11,1},
    {0,2},{1,2},{2,2},{3,2},{4,2},{5,2},{6,2},{7,2},{8,2},{9,2},{10,2},{11,2},{12,2},{13,2},
    {2,3},{3,3},{4,3},{5,3},{6,3},{7,3},{8,3},{9,3},{10,3},{11,3},
    {3,4},{5,4},{8,4},{10,4}
  };
  struct { int c, r; } eyes[] = {{4,1},{9,1}};
  for (auto& b : body) tmp.fillRect(ox + b.c*psw, oy + b.r*psh, psw, psh, col);
  for (auto& e : eyes) tmp.fillRect(ox + e.c*psw+1, oy + e.r*psh+1, psw-2, psh-2, TFT_BLACK);

  // Composite rotated Claude into fb (sprite-to-sprite), then push fb once — no flicker
  tmp.setPivot(sprSz / 2, sprSz / 2);
  fb.setPivot((int)ssX + MASCOT_W/2, (int)ssY + MASCOT_H/2);
  tmp.pushRotated(&fb, (int)ssDriftAngle, TFT_BLACK);
  tmp.deleteSprite();

  fb.pushSprite(0, 0);  // single push — stars + Claude together
}

void redrawHeaderNow() {
  if (ssActive || hdrProject[0] == '\0') return;
  // Use a sprite for the full header text area — clears artifacts and redraws cleanly
  const int SPR_W = HDR_TEXT_W + 20;
  TFT_eSprite hdr = TFT_eSprite(&tft);
  hdr.createSprite(SPR_W, HDR_H);
  hdr.fillSprite(TFT_BLACK);
  hdr.setTextSize(2); hdr.setTextFont(2);
  hdr.setTextColor(parseColor("#7B68EE"), TFT_BLACK);

  if (hdrNeedsScroll) {
    // Draw at current scroll position
    int tx = -(int)hdrScrollX;
    int gap = HDR_TEXT_W;
    hdr.setCursor(tx, HDR_TEXT_Y);
    hdr.print(hdrProject);
    hdr.setCursor(tx + hdrTextPxW + gap, HDR_TEXT_Y);
    hdr.print(hdrProject);
  } else {
    // Truncated static
    char truncated[64];
    strncpy(truncated, hdrProject, 63); truncated[63] = '\0';
    int len = strlen(truncated);
    hdr.setTextSize(2); hdr.setTextFont(2);
    while (len > 0 && hdr.textWidth(truncated) > HDR_TEXT_W) truncated[--len] = '\0';
    hdr.setCursor(0, HDR_TEXT_Y);
    hdr.print(truncated);
  }

  hdr.pushSprite(HDR_ICON_W, 0, 0, 0, HDR_TEXT_W, HDR_H);
  hdr.deleteSprite();
}

void tickHeader() {
  if (!hdrNeedsScroll || !hdrScrollEnabled || ssActive || hdrProject[0] == '\0') return;
  unsigned long now = millis();
  if (now - hdrLastScrollMs < HDR_SCROLL_MS) return;
  hdrLastScrollMs = now;

  // Advance scroll — loop period = text width + full window width so second copy
  // only enters from the right edge column-by-column once first copy is gone
  hdrScrollX += 1.0f;
  int gap = HDR_TEXT_W;  // gap = full text area width ensures clean right-edge entry
  if (hdrScrollX >= hdrTextPxW + gap) hdrScrollX = 0.0f;

  // Sprite wider than visible area so characters partially off the right edge render fully
  // then we push only the visible HDR_TEXT_W columns — gives smooth pixel-column entry
  const int SPR_W = HDR_TEXT_W + 20;
  TFT_eSprite hdr = TFT_eSprite(&tft);
  hdr.createSprite(SPR_W, HDR_H);
  hdr.fillSprite(TFT_BLACK);
  hdr.setTextSize(2); hdr.setTextFont(2);
  hdr.setTextColor(parseColor("#7B68EE"), TFT_BLACK);

  int tx = -(int)hdrScrollX;
  hdr.setCursor(tx, HDR_TEXT_Y);
  hdr.print(hdrProject);
  hdr.setCursor(tx + hdrTextPxW + gap, HDR_TEXT_Y);
  hdr.print(hdrProject);

  // Push only the visible portion (left HDR_TEXT_W columns) to screen
  hdr.pushSprite(HDR_ICON_W, 0, 0, 0, HDR_TEXT_W, HDR_H);
  hdr.deleteSprite();
}

void tickScreensaver() {
  unsigned long now = millis();
  if (now - ssLastFrameMs < SS_FRAME_MS) return;
  ssLastFrameMs = now;

  if (ssMode == SS_NYAN) {
    // Sample position into trail buffer every N frames for visible spacing
    ssSampleTick++;
    if (ssSampleTick >= TRAIL_SAMPLE) {
      ssSampleTick = 0;
      ssTrail[ssTrailHead] = {(int)ssX, (int)ssY, RAINBOW[ssColorOff % RAINBOW_BANDS]};
      ssColorOff = (ssColorOff + 1) % RAINBOW_BANDS;
      ssTrailHead = (ssTrailHead + 1) % TRAIL_SEGS;
      if (ssTrailHead == 0) ssTrailFull = true;
    }
  }

  // Move (shared)
  ssX += ssDx;
  ssY += ssDy;

  // Bounce — shared, but drift mode also updates depth on each bounce
  bool bounced = false;
  if (ssX <= 0)              { ssX = 0;               ssDx = fabsf(ssDx);  ssBounceFrames = SS_BOUNCE_LEN; bounced = true; }
  if (ssX >= 240 - MASCOT_W) { ssX = 240 - MASCOT_W;  ssDx = -fabsf(ssDx); ssBounceFrames = SS_BOUNCE_LEN; bounced = true; }
  if (ssY <= 0)              { ssY = 0;               ssDy = fabsf(ssDy);  ssBounceFrames = SS_BOUNCE_LEN; bounced = true; }
  if (ssY >= 240 - MASCOT_H) { ssY = 240 - MASCOT_H;  ssDy = -fabsf(ssDy); ssBounceFrames = SS_BOUNCE_LEN; bounced = true; }

  if (ssMode == SS_DRIFT && bounced) {
    ssBounceCount++;
    int limit = (ssDriftDir < 0) ? DRIFT_SHRINK_BOUNCES : DRIFT_GROW_BOUNCES;
    if (ssBounceCount >= limit) {
      ssBounceCount = 0;
      ssDriftDir    = -ssDriftDir;  // flip direction
    }
  }

  if (ssMode == SS_DRIFT) {
    // Advance rotation and depth smoothly each frame
    ssDriftAngle += 2.5f;
    if (ssDriftAngle >= 360.0f) ssDriftAngle -= 360.0f;
    float step = (DRIFT_SCALE_MAX - DRIFT_SCALE_MIN) / (DRIFT_GROW_BOUNCES * 30.0f);
    ssDriftScale += ssDriftDir * step;
    ssDriftScale  = max(DRIFT_SCALE_MIN, min(DRIFT_SCALE_MAX, ssDriftScale));
    drawDriftFrame();
  } else if (ssMode == SS_INVADERS) {
    drawInvadersFrame();
  } else {
    drawNyanFrame();
  }
}

void invReset() {
  for (int r = 0; r < INV_ROWS; r++)
    for (int c = 0; c < INV_COLS; c++)
      invAlive[r][c] = true;
  invAliveCount    = INV_ROWS * INV_COLS;
  invGridX         = 0.0f;
  invGridY         = 0;
  invDx            = 0.4f;
  invScore         = 0;
  invGameOver      = false;
  invGameOverFrames = 0;
  invBulActive     = false;
  invPlayerLives   = 3;
  invPlayerHit     = false;
  invPlayerHitFrames = 0;
  invPlayerX       = 112.0f;
  invFireTick      = 40;
  invAiTick        = 0;
  invWavePause     = 0;
  for (int i = 0; i < INV_MAX_ABULS; i++) invAbuls[i].active = false;
}

void drawInvadersFrame() {
  fb.fillSprite(TFT_BLACK);

  // --- Game over screen ---
  if (invGameOver) {
    fb.setTextSize(2); fb.setTextFont(1);
    fb.setTextColor(TFT_WHITE, TFT_BLACK);
    fb.setCursor(68, 95);  fb.print("GAME");
    fb.setCursor(68, 115); fb.print("OVER");
    fb.setTextSize(1); fb.setTextFont(1);
    char sbuf[20]; sprintf(sbuf, "Score: %d", invScore);
    fb.setCursor(72, 140); fb.print(sbuf);
    fb.pushSprite(0, 0);
    invGameOverFrames--;
    if (invGameOverFrames <= 0) { invGameOver = false; invReset(); }
    return;
  }

  if (invWavePause > 0) {
    invWavePause--;
    if (invWavePause == 0) invReset();
    fb.pushSprite(0, 0);
    return;
  }

  // --- Move grid ---
  invGridX += invDx;
  // Find leftmost and rightmost alive column
  int leftCol = INV_COLS, rightCol = -1;
  for (int r = 0; r < INV_ROWS; r++)
    for (int c = 0; c < INV_COLS; c++)
      if (invAlive[r][c]) { leftCol = min(leftCol, c); rightCol = max(rightCol, c); }

  float gridLeft  = INV_XSTART + invGridX + leftCol  * INV_XGAP;
  float gridRight = INV_XSTART + invGridX + rightCol * INV_XGAP + INV_W;
  if (gridRight >= 238 || gridLeft <= 2) {
    invDx = -invDx;
    invGridY += 8;  // drop down
  }

  // Speed up as aliens die (base 0.4, up to ~1.6 when nearly cleared)
  float speedScale = 1.0f + (INV_ROWS * INV_COLS - invAliveCount) * 0.04f;
  float curDx = (invDx > 0 ? 1.0f : -1.0f) * 0.4f * speedScale;
  invDx = curDx;

  // --- Score at top ---
  fb.setTextSize(1); fb.setTextFont(2);
  fb.setTextColor(0xFFE0, TFT_BLACK);
  char scoreBuf[20]; sprintf(scoreBuf, "Score: %d", invScore);
  fb.setCursor(4, 2); fb.print(scoreBuf);

  // --- Draw aliens ---
  for (int r = 0; r < INV_ROWS; r++) {
    for (int c = 0; c < INV_COLS; c++) {
      if (!invAlive[r][c]) continue;
      int ax = INV_XSTART + (int)invGridX + c * INV_XGAP;
      int ay = INV_YSTART + invGridY      + r * INV_YGAP;
      drawMascotAt(ax, ay, 0, INV_ROW_COLOR[r], false, INV_SCALE);
    }
  }

  // --- Alien fire ---
  invFireTick--;
  if (invFireTick <= 0 && invAliveCount > 0) {
    invFireTick = max(10, 50 - (INV_ROWS * INV_COLS - invAliveCount));
    // Find a random alive alien in bottom-most row of each column
    int col = random(0, INV_COLS);
    for (int attempt = 0; attempt < INV_COLS; attempt++) {
      int c = (col + attempt) % INV_COLS;
      for (int r = INV_ROWS - 1; r >= 0; r--) {
        if (invAlive[r][c]) {
          // Find free bullet slot
          for (int i = 0; i < INV_MAX_ABULS; i++) {
            if (!invAbuls[i].active) {
              invAbuls[i].active = true;
              invAbuls[i].x = INV_XSTART + (int)invGridX + c * INV_XGAP + INV_W/2;
              invAbuls[i].y = INV_YSTART + invGridY + r * INV_YGAP + INV_H;
              break;
            }
          }
          break;
        }
      }
      break;
    }
  }

  // Move + draw alien bullets, check player hit
  for (int i = 0; i < INV_MAX_ABULS; i++) {
    if (!invAbuls[i].active) continue;
    invAbuls[i].y += INV_ABUL_SPD;
    if (invAbuls[i].y > 240) { invAbuls[i].active = false; continue; }
    fb.fillRect((int)invAbuls[i].x, (int)invAbuls[i].y, 2, 5, 0xF800);
    // Hit player?
    if (!invPlayerHit &&
        invAbuls[i].y >= INV_PLAYER_Y - 4 &&
        abs((int)invAbuls[i].x - (int)invPlayerX) < INV_PLAYER_W/2 + 2) {
      invAbuls[i].active = false;
      invPlayerHit = true;
      invPlayerHitFrames = 20;
      invPlayerLives--;
      if (invPlayerLives <= 0) { invGameOver = true; invGameOverFrames = 120; return; }
    }
  }

  if (invPlayerHit) {
    invPlayerHitFrames--;
    if (invPlayerHitFrames <= 0) invPlayerHit = false;
  }

  // --- AI: move player toward lowest alien in nearest column, fire when aligned ---
  invAiTick++;
  if (invAiTick >= 1) {
    invAiTick = 0;
    // Find lowest alive alien (highest y), use its column as target
    int targetCol = -1, lowestRow = -1;
    float bestDist = 9999;
    for (int c = 0; c < INV_COLS; c++) {
      for (int r = INV_ROWS - 1; r >= 0; r--) {
        if (invAlive[r][c]) {
          float alienX = INV_XSTART + invGridX + c * INV_XGAP + INV_W/2;
          float dist = fabsf(alienX - invPlayerX);
          if (r > lowestRow || (r == lowestRow && dist < bestDist)) {
            lowestRow = r; targetCol = c; bestDist = dist;
          }
          break;
        }
      }
    }
    if (targetCol >= 0) {
      float targetX = INV_XSTART + invGridX + targetCol * INV_XGAP + INV_W/2;
      float diff = targetX - invPlayerX;
      float step = min(fabsf(diff), 2.0f);
      invPlayerX += (diff > 0 ? step : -step);
      invPlayerX = max(INV_PLAYER_W/2.0f, min(240.0f - INV_PLAYER_W/2.0f, invPlayerX));

      // Fire if roughly aligned and no active bullet
      if (!invBulActive && fabsf(diff) < 6.0f) {
        invBulActive = true;
        invBulX = invPlayerX;
        invBulY = INV_PLAYER_Y - 6;
      }
    }
  }

  // --- Player bullet ---
  if (invBulActive) {
    invBulY -= INV_BULLET_SPD;
    if (invBulY < 0) { invBulActive = false; }
    else {
      fb.fillRect((int)invBulX - 1, (int)invBulY, 2, 5, 0xFFFF);
      // Hit alien?
      for (int r = 0; r < INV_ROWS && invBulActive; r++) {
        for (int c = 0; c < INV_COLS && invBulActive; c++) {
          if (!invAlive[r][c]) continue;
          int ax = INV_XSTART + (int)invGridX + c * INV_XGAP;
          int ay = INV_YSTART + invGridY      + r * INV_YGAP;
          if (invBulX >= ax && invBulX <= ax + INV_W &&
              invBulY >= ay && invBulY <= ay + INV_H) {
            invAlive[r][c] = false;
            invAliveCount--;
            invBulActive = false;
            invScore += 10 * (INV_ROWS - r);  // top rows worth more
            if (invAliveCount == 0) { invWavePause = 80; }
          }
        }
      }
    }
  }

  // --- Draw player ship ---
  uint16_t playerCol = invPlayerHit ? 0xF800 : 0x07FF;
  int px = (int)invPlayerX;
  fb.fillTriangle(px, INV_PLAYER_Y - 6, px - INV_PLAYER_W/2, INV_PLAYER_Y + 2,
                  px + INV_PLAYER_W/2, INV_PLAYER_Y + 2, playerCol);

  // --- Lives indicator ---
  for (int i = 0; i < invPlayerLives; i++)
    fb.fillTriangle(4 + i*10, 238, 4 + i*10 - 4, 240, 4 + i*10 + 4, 240, 0x07FF);

  // --- Check aliens reached bottom ---
  for (int r = 0; r < INV_ROWS; r++)
    for (int c = 0; c < INV_COLS; c++)
      if (invAlive[r][c] && INV_YSTART + invGridY + r * INV_YGAP + INV_H >= INV_PLAYER_Y - 4)
        { invGameOver = true; invGameOverFrames = 120; fb.pushSprite(0, 0); return; }

  fb.pushSprite(0, 0);
}

void startScreensaver(SsMode mode = SS_NYAN) {
  // Random: pick one of the three real modes
  if (mode == SS_RANDOM) {
    const SsMode choices[] = { SS_NYAN, SS_DRIFT, SS_INVADERS };
    mode = choices[random(0, 3)];
  }
  ssMode         = mode;
  ssActive       = true;
  ssTrailHead    = 0;
  ssTrailFull    = false;
  ssSampleTick   = 0;
  ssColorOff     = 0;
  ssSqFrame      = 0;
  ssSqTick       = 0;
  ssBounceFrames = 0;
  ssBounceCount  = 0;
  ssDriftDir     = -1;      // start by going away (shrinking)
  ssDriftScale   = DRIFT_SCALE_MAX;  // start at full size

  // Scatter stars across all layers
  for (int l = 0; l < STAR_LAYERS; l++) {
    for (int i = 0; i < STARS_PER_LAYER; i++) {
      ssStars[l][i].x = (float)random(0, 240);
      ssStars[l][i].y = random(0, 240);
    }
  }

  // UFO: start inactive, first flyby after 15–35s
  ssUfo.active  = false;
  ssUfoNextMs   = millis() + 15000 + random(0, 20000);

  // Invaders: reset game state
  if (mode == SS_INVADERS) invReset();

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

  // Header — folder icon + project name
  // Folder icon (~18x14px) centered vertically in HDR_H=28 row, at x=5
  uint16_t folderCol  = parseColor("#F0A030");
  uint16_t folderDark = parseColor("#C07820");
  fb.fillRect(5,  14, 18, 11, folderCol);   // main body
  fb.fillRect(5,  11,  8,  4, folderCol);   // tab top-left
  fb.fillRect(5,  14, 18,  1, folderDark);  // shadow line top of body

  // Store project for scroll ticker
  strncpy(hdrProject, project[0] ? project : "Claude Session", 63);
  hdrProject[63] = '\0';

  // Measure text width to decide static vs scroll
  fb.setTextSize(2); fb.setTextFont(2);
  hdrTextPxW     = fb.textWidth(hdrProject);
  hdrNeedsScroll = (hdrTextPxW > HDR_TEXT_W) && hdrScrollEnabled;
  hdrScrollX     = 0.0f;

  // Draw initial header text
  fb.setTextColor(parseColor("#7B68EE"), TFT_BLACK);
  if (!hdrNeedsScroll) {
    // Truncate string to fit — remove chars from the end until it fits
    char truncated[64];
    strncpy(truncated, hdrProject, 63); truncated[63] = '\0';
    int len = strlen(truncated);
    while (len > 0 && fb.textWidth(truncated) > HDR_TEXT_W) {
      truncated[--len] = '\0';
    }
    fb.setCursor(HDR_ICON_W, HDR_TEXT_Y);
    fb.print(truncated);
  }
  // scrolling version drawn by tickHeader()

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

  // Load display preferences from NVS
  prefs.begin("display", true);
  String savedMode  = prefs.getString("ssmode",   "nyan");
  int    savedIdle  = prefs.getInt("ssidle",      5);     // minutes, 0 = never
  bool   savedScroll = prefs.getBool("hdrscroll", true);
  prefs.end();
  ssMode         = (savedMode == "drift") ? SS_DRIFT : (savedMode == "invaders") ? SS_INVADERS : (savedMode == "random") ? SS_RANDOM : (savedMode == "off") ? SS_OFF : SS_NYAN;
  ssIdleMs       = (savedIdle > 0) ? (unsigned long)savedIdle * 60 * 1000 : 0;
  hdrScrollEnabled = savedScroll;

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
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    String mode = doc["mode"] | "nyan";
    SsMode newMode = (mode == "drift") ? SS_DRIFT : (mode == "invaders") ? SS_INVADERS : (mode == "random") ? SS_RANDOM : SS_NYAN;
    prefs.begin("display", false);
    prefs.putString("ssmode", mode);
    prefs.end();
    startScreensaver(newMode);
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

  // Settings web UI
  server.on("/settings", HTTP_GET, []() {
    String ssidStr, curMode, idleMins;
    prefs.begin("wifi", true);  ssidStr = prefs.getString("ssid", ""); prefs.end();
    if (ssMode == SS_DRIFT)         curMode = "drift";
    else if (ssMode == SS_INVADERS) curMode = "invaders";
    else if (ssMode == SS_RANDOM)   curMode = "random";
    else if (ssMode == SS_OFF)      curMode = "off";
    else                            curMode = "nyan";
    idleMins = String(ssIdleMs > 0 ? (int)(ssIdleMs / 60000) : 0);

    String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>ESP32 Display Settings</title>"
      "<style>body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:0 16px;background:#111;color:#eee}"
      "h1{color:#7B68EE;margin-bottom:4px}h2{color:#aaa;font-size:1em;font-weight:normal;margin-top:0}"
      ".card{background:#1a1a2e;border-radius:8px;padding:16px;margin:16px 0}"
      ".card h3{margin:0 0 12px;color:#7B68EE;font-size:0.95em;text-transform:uppercase;letter-spacing:1px}"
      "label{display:block;margin:8px 0 4px;color:#aaa;font-size:0.9em}"
      "input[type=text],input[type=password],input[type=number]{width:100%;padding:8px;box-sizing:border-box;"
      "background:#0d0d1a;border:1px solid #333;color:#eee;border-radius:4px}"
      ".radio-group label{display:inline-flex;align-items:center;gap:6px;margin-right:16px;color:#eee}"
      "button,input[type=submit]{background:#7B68EE;color:#fff;border:none;padding:10px 20px;"
      "border-radius:4px;cursor:pointer;font-size:0.95em;margin-top:8px}"
      "button.danger{background:#c0392b}"
      ".status{font-size:0.85em;color:#888;margin-bottom:0}"
      ".status span{color:#5DADE2}</style></head><body>"
      "<h1>ESP32 Display</h1>"
      "<p class='status'>v<span>" FW_VERSION "</span> &nbsp;|&nbsp; IP: <span>" + WiFi.localIP().toString() + "</span>"
      " &nbsp;|&nbsp; RSSI: <span>" + String(WiFi.RSSI()) + " dBm</span>"
      " &nbsp;|&nbsp; Free heap: <span>" + String(ESP.getFreeHeap()/1024) + " KB</span></p>"

      "<form method='POST' action='/settings'>"

      "<div class='card'><h3>Screensaver</h3>"
      "<label>Mode</label>"
      "<div class='radio-group'>"
      "<label><input type='radio' name='ssmode' value='nyan'" + String(curMode=="nyan"?" checked":"") + "> Nyan</label>"
      "<label><input type='radio' name='ssmode' value='drift'" + String(curMode=="drift"?" checked":"") + "> Drift</label>"
      "<label><input type='radio' name='ssmode' value='invaders'" + String(curMode=="invaders"?" checked":"") + "> Invaders</label>"
      "<label><input type='radio' name='ssmode' value='random'" + String(curMode=="random"?" checked":"") + "> Random</label>"
      "<label><input type='radio' name='ssmode' value='off'" + String(curMode=="off"?" checked":"") + "> Disabled</label>"
      "</div>"
      "<label>Idle timeout (minutes, 0 = never)</label>"
      "<input type='number' name='ssidle' min='0' max='60' value='" + idleMins + "'>"
      "</div>"

      "<div class='card'><h3>Header</h3>"
      "<label>Long folder names</label>"
      "<div class='radio-group'>"
      "<label><input type='radio' name='hdrscroll' value='1'" + String(hdrScrollEnabled?" checked":"") + "> Scroll</label>"
      "<label><input type='radio' name='hdrscroll' value='0'" + String(!hdrScrollEnabled?" checked":"") + "> Truncate</label>"
      "</div></div>"

      "<div class='card'><h3>WiFi</h3>"
      "<label>Current SSID: <span style='color:#5DADE2'>" + ssidStr + "</span></label>"
      "<label>New SSID</label><input type='text' name='ssid' placeholder='leave blank to keep current'>"
      "<label>Password</label><input type='password' name='pass' placeholder='leave blank to keep current'>"
      "</div>"

      "<input type='submit' value='Save Settings'>"
      "</form>"

      "<div class='card'><h3>Danger Zone</h3>"
      "<form method='POST' action='/wifi/clear'>"
      "<button class='danger' type='submit'>Clear WiFi &amp; Reboot to AP Mode</button>"
      "</form></div>"

      "<div class='card'><h3>Firmware</h3>"
      "<a href='/update' style='color:#7B68EE'>OTA Firmware Update</a></div>"

      "</body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/settings", HTTP_POST, []() {
    // Screensaver mode
    String newSsMode = server.arg("ssmode");
    if (newSsMode == "drift")          ssMode = SS_DRIFT;
    else if (newSsMode == "invaders")  ssMode = SS_INVADERS;
    else if (newSsMode == "random")    ssMode = SS_RANDOM;
    else if (newSsMode == "off")       ssMode = SS_OFF;
    else                               { ssMode = SS_NYAN; newSsMode = "nyan"; }

    // Idle timeout
    int newIdle = server.arg("ssidle").toInt();
    ssIdleMs = (newIdle > 0) ? (unsigned long)newIdle * 60 * 1000 : 0;

    // Header scroll
    hdrScrollEnabled = (server.arg("hdrscroll") == "1");
    hdrScrollX       = 0.0f;
    hdrNeedsScroll   = (hdrTextPxW > HDR_TEXT_W) && hdrScrollEnabled;
    redrawHeaderNow();

    // WiFi (only update if new ssid provided)
    String newSsid = server.arg("ssid");
    String newPass = server.arg("pass");

    prefs.begin("display", false);
    prefs.putString("ssmode",    newSsMode);
    prefs.putInt("ssidle",       newIdle);
    prefs.putBool("hdrscroll",   hdrScrollEnabled);
    prefs.end();

    if (newSsid.length() > 0) {
      prefs.begin("wifi", false);
      prefs.putString("ssid", newSsid);
      prefs.putString("pass", newPass);
      prefs.end();
      server.sendHeader("Location", "/settings");
      server.send(303);
      delay(500);
      ESP.restart();
      return;
    }

    server.sendHeader("Location", "/settings");
    server.send(303);
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

  // Auto-start screensaver after idle timeout (skip if disabled or SS_OFF)
  if (!ssActive && ssMode != SS_OFF && ssIdleMs > 0) {
    if ((millis() > ssIdleMs) && (lastDashboardMs == 0 || (millis() - lastDashboardMs > ssIdleMs))) {
      startScreensaver(ssMode);
    }
  }

  tickHeader();

  if (ssActive) {
    tickScreensaver();
  }
}
