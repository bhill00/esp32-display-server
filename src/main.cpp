#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "config.h"

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite fb = TFT_eSprite(&tft);
WebServer server(HTTP_PORT);

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

void handleStatus() {
  JsonDocument doc;
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["ssid"] = WiFi.SSID();
  doc["width"] = 240;
  doc["height"] = 240;
  doc["uptime_s"] = millis() / 1000;
  doc["free_heap"] = ESP.getFreeHeap();
  doc["version"] = "2.3-ota-test";
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

  // WiFi connect
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[display-server] Connecting to %s", WIFI_SSID);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
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
    tft.println("GET /help for API");
  } else {
    Serial.println("\n[display-server] WiFi FAILED");
    tft.fillScreen(TFT_RED);
    tft.setCursor(10, 10);
    tft.setTextFont(4);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.println("WiFi Failed!");
    tft.setTextFont(2);
    tft.printf("SSID: %s\n", WIFI_SSID);
    tft.println("Check config.h");
    return;  // Don't start server
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
}
