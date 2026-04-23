/*
  ============================================================
  Smart Fridge Expiry Alert System — ESP32 FINAL FIXED CODE
  ============================================================

  FIX: Blynk.notify() removed in new Blynk IoT library.
       Using Blynk.logEvent() correctly with proper event names.

  IMPORTANT — Create these events in Blynk Console:
  Go to: Template → Events & Notifications → Add Event
  
  Event Code          | Notification
  --------------------|---------------------------
  product_scanned     | Enable Push Notification
  product_removed     | Enable Push Notification
  expiry_set          | Enable Push Notification
  expiry_alert        | Enable Push Notification
  product_expired     | Enable Push Notification
  expiry_error        | Enable Push Notification

  BLYNK DATASTREAMS:
  V0 → Product Code   (String)
  V1 → Product Label  (String)
  V2 → Expiry Date    (String)
  V3 → Days Left      (Integer)
  V4 → Status         (String)
  V5 → Enter Expiry   (String) ← USER INPUT

  LIBRARIES NEEDED:
  - Blynk by Volodymyr Shymanskyy
  - NTPClient by Fabrice Weinberg
  ============================================================
*/

// ── Blynk Config ─────────────────────────────────────────────
#define BLYNK_TEMPLATE_ID    "TMPL3gygemwHR"
#define BLYNK_TEMPLATE_NAME  "ExpiryTracker"
#define BLYNK_AUTH_TOKEN     "ZtyERiPV37EyapC_h9elORuhO7DwxYHo"
#define BLYNK_PRINT Serial

// ── Includes ─────────────────────────────────────────────────
#include <WiFi.h>
#include <WebServer.h>
#include <BlynkSimpleEsp32.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>

// ── WiFi (your hotspot) ───────────────────────────────────────
const char* WIFI_SSID     = "Siddhi";
const char* WIFI_PASSWORD = "siddhi25";

// ── HTTP Server ───────────────────────────────────────────────
WebServer server(80);

// ── NTP (IST = UTC+5:30 = 19800s) ────────────────────────────
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);

// ── Product Structure ─────────────────────────────────────────
struct Product {
  String code;
  String expiryDate;
  bool   active;
  bool   expirySet;
  time_t expiryEpoch;
};

Product currentProduct;

// ── Blynk Timer ──────────────────────────────────────────────
BlynkTimer blynkTimer;

// ── Notification timestamp tracker ───────────────────────────
// Blynk allows ~1 logEvent per second per event type
// We space out writes using millis()
unsigned long lastEventTime = 0;

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Smart Fridge ESP32 FINAL ===");

  resetProduct();
  connectWiFi();

  Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASSWORD);
  Serial.println("Blynk connected.");

  timeClient.begin();
  syncTime();

  server.on("/scan",   HTTP_GET, handleScan);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/remove", HTTP_GET, handleManualRemove);
  server.begin();

  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());

  // Check expiry every 1 hour
  blynkTimer.setInterval(3600000UL, checkExpiryAlert);

  updateAllPins();
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  Blynk.run();
  server.handleClient();
  blynkTimer.run();
  timeClient.update();
}

// ============================================================
// SAFE LOG EVENT
// Waits 1 second between events to avoid Blynk rate limiting
// ============================================================
void safeLogEvent(const char* eventCode, String message) {
  unsigned long now = millis();
  if (now - lastEventTime < 1000) {
    delay(1000 - (now - lastEventTime));
  }
  Blynk.logEvent(eventCode, message);
  lastEventTime = millis();
  Serial.println("[EVENT] " + String(eventCode) + ": " + message);
}

// ============================================================
// WiFi
// ============================================================
void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi failed. Restarting...");
    ESP.restart();
  }
}

// ============================================================
// NTP TIME SYNC
// ============================================================
void syncTime() {
  Serial.print("Syncing time");
  int tries = 0;
  while (!timeClient.update() && tries < 10) {
    timeClient.forceUpdate();
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println("\nTime: " + timeClient.getFormattedTime());
}

time_t todayMidnight() {
  time_t now = timeClient.getEpochTime();
  struct tm* t = gmtime(&now);
  t->tm_hour = 0;
  t->tm_min  = 0;
  t->tm_sec  = 0;
  return mktime(t);
}

time_t parseDate(String d) {
  if (d.length() != 10 || d[2] != '/' || d[5] != '/') return 0;
  int day   = d.substring(0, 2).toInt();
  int month = d.substring(3, 5).toInt();
  int year  = d.substring(6, 10).toInt();
  if (day == 0 || month == 0 || year < 2024) return 0;
  struct tm t = {};
  t.tm_mday = day;
  t.tm_mon  = month - 1;
  t.tm_year = year - 1900;
  return mktime(&t);
}

// ============================================================
// HTTP: /scan?code=<barcode>
// ============================================================
void handleScan() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET");

  if (!server.hasArg("code") || server.arg("code").isEmpty()) {
    server.send(400, "text/plain", "ERROR: Missing code");
    return;
  }

  String scannedCode = server.arg("code");
  Serial.println("\n[SCAN] Code: " + scannedCode);

  // ── Ignore PING from web app ──────────────────────────────
  if (scannedCode == "PING") {
    Serial.println("[SCAN] PING ignored.");
    server.send(200, "text/plain", "PONG");
    return;
  }

  // ── Toggle Logic ──────────────────────────────────────────
  if (currentProduct.active && currentProduct.code == scannedCode) {
    // Same code → REMOVE
    String removed = currentProduct.code;
    resetProduct();
    updateAllPins();
    safeLogEvent("product_removed", "Removed: " + removed + " has been taken out of the fridge.");
    server.send(200, "text/plain", "REMOVED:" + removed);

  } else {
    // New product → ADD
    currentProduct.code        = scannedCode;
    currentProduct.active      = true;
    currentProduct.expirySet   = false;
    currentProduct.expiryDate  = "";
    currentProduct.expiryEpoch = 0;

    updateAllPins();

    safeLogEvent("product_scanned",
      "Scanned: " + scannedCode + " | Open Blynk, enter expiry date in V5 (DD/MM/YYYY)");

    server.send(200, "text/plain", "ADDED:" + scannedCode);
  }
}

// ============================================================
// HTTP: /status
// ============================================================
void handleStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String json = "{";
  json += "\"active\":"    + String(currentProduct.active    ? "true" : "false") + ",";
  json += "\"code\":\""    + currentProduct.code             + "\",";
  json += "\"expiry\":\""  + currentProduct.expiryDate       + "\",";
  json += "\"expirySet\":" + String(currentProduct.expirySet ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

// ============================================================
// HTTP: /remove
// ============================================================
void handleManualRemove() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!currentProduct.active) {
    server.send(200, "text/plain", "NO_PRODUCT");
    return;
  }
  String removed = currentProduct.code;
  resetProduct();
  updateAllPins();
  safeLogEvent("product_removed", "Manually removed: " + removed + " from fridge tracking.");
  server.send(200, "text/plain", "REMOVED:" + removed);
}

// ============================================================
// BLYNK V5 — User enters expiry date (DD/MM/YYYY)
// ============================================================
BLYNK_WRITE(V5) {
  String input = param.asStr();
  input.trim();
  Serial.println("[V5] Input: " + input);

  if (!currentProduct.active) {
    Blynk.virtualWrite(V4, "No product scanned yet! Scan first.");
    return;
  }

  // Validate format
  if (input.length() != 10 || input[2] != '/' || input[5] != '/') {
    Blynk.virtualWrite(V4, "Wrong format! Use DD/MM/YYYY");
    safeLogEvent("expiry_error", "Wrong date format entered. Use DD/MM/YYYY");
    return;
  }

  time_t parsed = parseDate(input);
  if (parsed == 0) {
    Blynk.virtualWrite(V4, "Invalid date! Check values.");
    safeLogEvent("expiry_error", "Invalid date values entered.");
    return;
  }

  time_t today = todayMidnight();
  if (parsed <= today) {
    Blynk.virtualWrite(V4, "Date already passed! Enter future date.");
    safeLogEvent("expiry_error", "Entered date is already in the past!");
    return;
  }

  // Store expiry
  currentProduct.expiryDate  = input;
  currentProduct.expiryEpoch = parsed;
  currentProduct.expirySet   = true;

  updateAllPins();

  long daysLeft = (parsed - today) / 86400;
  safeLogEvent("expiry_set",
    "Expiry set! " + currentProduct.code + " expires on " + input + " (" + String(daysLeft) + " days left)");
}

// ============================================================
// BLYNK CONNECTED
// ============================================================
BLYNK_CONNECTED() {
  Serial.println("[BLYNK] Connected — refreshing pins");
  updateAllPins();
}

// ============================================================
// EXPIRY CHECK — every 1 hour
// ============================================================
void checkExpiryAlert() {
  Serial.println("[CHECK] Running expiry check...");

  if (!currentProduct.active || !currentProduct.expirySet) {
    Serial.println("[CHECK] Nothing to check.");
    return;
  }

  time_t today    = todayMidnight();
  long   daysLeft = (long)(currentProduct.expiryEpoch - today) / 86400;

  Serial.println("[CHECK] Days left: " + String(daysLeft));
  Blynk.virtualWrite(V3, daysLeft);

  if (daysLeft < 0) {
    safeLogEvent("product_expired",
      "EXPIRED! " + currentProduct.code
      + " expired on " + currentProduct.expiryDate + ". Remove from fridge now!");
    updateAllPins();

  } else if (daysLeft <= 2) {
    safeLogEvent("expiry_alert",
      "Expiring Soon! " + currentProduct.code
      + " expires in " + String(daysLeft) + " day(s) on " + currentProduct.expiryDate + ". Use it!");
    updateAllPins();

  } else {
    updateAllPins();
  }
}

// ============================================================
// UPDATE ALL BLYNK PINS
// ============================================================
void updateAllPins() {
  if (!currentProduct.active) {
    Blynk.virtualWrite(V0, "—");              delay(100);
    Blynk.virtualWrite(V1, "No product");     delay(100);
    Blynk.virtualWrite(V2, "—");              delay(100);
    Blynk.virtualWrite(V3, 0);               delay(100);
    Blynk.virtualWrite(V4, "Ready. Scan a product.");
    return;
  }

  Blynk.virtualWrite(V0, currentProduct.code); delay(100);

  if (!currentProduct.expirySet) {
    Blynk.virtualWrite(V1, "Awaiting expiry"); delay(100);
    Blynk.virtualWrite(V2, "Not set");         delay(100);
    Blynk.virtualWrite(V3, 0);                delay(100);
    Blynk.virtualWrite(V4, "Product scanned! Enter expiry date in V5 (DD/MM/YYYY)");
    return;
  }

  time_t today    = todayMidnight();
  long   daysLeft = (long)(currentProduct.expiryEpoch - today) / 86400;

  Blynk.virtualWrite(V2, currentProduct.expiryDate); delay(100);
  Blynk.virtualWrite(V3, daysLeft);                  delay(100);

  if (daysLeft < 0) {
    Blynk.virtualWrite(V1, "EXPIRED");
    Blynk.virtualWrite(V4, "EXPIRED on " + currentProduct.expiryDate + "! Discard now.");
  } else if (daysLeft <= 2) {
    Blynk.virtualWrite(V1, "Expiring Soon");
    Blynk.virtualWrite(V4, "Expires in " + String(daysLeft) + " day(s)! Use it now.");
  } else {
    Blynk.virtualWrite(V1, "Tracking Active");
    Blynk.virtualWrite(V4, currentProduct.code + " — " + String(daysLeft) + " days left.");
  }
}

// ============================================================
// RESET PRODUCT
// ============================================================
void resetProduct() {
  currentProduct.code        = "";
  currentProduct.expiryDate  = "";
  currentProduct.active      = false;
  currentProduct.expirySet   = false;
  currentProduct.expiryEpoch = 0;
}
