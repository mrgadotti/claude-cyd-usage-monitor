/*
 * claude-usage
 * Board : ESP32 Cheap Yellow Display (ESP32-2432S028R), ILI9341 240x320
 *
 * Standalone: joins Wi-Fi, polls the Claude OAuth usage endpoint over HTTPS and
 * draws the session (5h) and weekly bars with their reset times.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "secrets.h"

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

static const char *USAGE_URL   = "https://api.anthropic.com/api/oauth/usage";
static const char *REFRESH_URL = "https://platform.claude.com/v1/oauth/token";
static const char *CLIENT_ID   = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
static const char *TZ_BRAZIL   = "<-03>3";
static const uint32_t POLL_MS  = 60000;

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;

String accessToken, refreshToken;
int64_t expiresAtMs = 0;

struct Usage {
  int pct = -1;
  char reset[20] = "--";
};
Usage sess, week;
char status[40] = "starting";

uint16_t barColor(int pct)
{
  if (pct >= 90) return TFT_RED;
  if (pct >= 70) return TFT_ORANGE;
  return TFT_GREEN;
}

void drawBar(int y, const char *title, const Usage &u)
{
  const int x = 15, w = SCREEN_WIDTH - 30, h = 30;
  tft.fillRect(0, y, SCREEN_WIDTH, 90, TFT_BLACK);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(title, x, y, 4);

  char buf[32];
  if (u.pct >= 0) snprintf(buf, sizeof buf, "%d%%", u.pct);
  else snprintf(buf, sizeof buf, "--");
  tft.setTextDatum(TR_DATUM);
  tft.drawString(buf, x + w, y, 4);

  tft.drawRoundRect(x, y + 28, w, h, 6, TFT_DARKGREY);
  if (u.pct > 0) {
    int fill = constrain(u.pct, 0, 100) * (w - 4) / 100;
    tft.fillRoundRect(x + 2, y + 30, fill, h - 4, 4, barColor(u.pct));
  }

  snprintf(buf, sizeof buf, "Resets %s", u.reset);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(buf, x, y + 64, 2);
}

void drawStatus(const char *msg, uint16_t color = TFT_DARKGREY)
{
  snprintf(status, sizeof status, "%s", msg);
  tft.fillRect(0, 215, SCREEN_WIDTH, 25, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextDatum(BC_DATUM);
  tft.drawString(status, SCREEN_WIDTH / 2, SCREEN_HEIGHT - 4, 2);
}

void render()
{
  drawBar(22, "Session (5h)", sess);
  drawBar(120, "Weekly", week);
}

// "2026-10-29T19:59:59.778697+00:00" -> local "HH:MM" or "DD/MM HH:MM"
bool formatReset(const char *iso, bool withDate, char *out, size_t n)
{
  int Y, M, D, h, m, s;
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return false;

  int offSec = 0;
  const char *t = strchr(iso, 'T');
  const char *sign = t ? strpbrk(t, "+-") : nullptr;
  int oh, om;
  if (sign && sscanf(sign + 1, "%d:%d", &oh, &om) == 2)
    offSec = (oh * 3600 + om * 60) * (*sign == '-' ? -1 : 1);

  // days since epoch (Howard Hinnant's civil algorithm)
  int y = Y - (M <= 2);
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = y - era * 400;
  unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = (int64_t)era * 146097 + doe - 719468;
  time_t epoch = days * 86400 + h * 3600 + m * 60 + s - offSec;

  struct tm lt;
  localtime_r(&epoch, &lt);
  strftime(out, n, withDate ? "%d/%m %H:%M" : "%H:%M", &lt);
  return true;
}

void loadTokens()
{
  prefs.begin("claude", false);
  accessToken = prefs.getString("at", SEED_ACCESS_TOKEN);
  refreshToken = prefs.getString("rt", SEED_REFRESH_TOKEN);
  expiresAtMs = prefs.getLong64("exp", SEED_EXPIRES_AT_MS);
}

bool refreshAccessToken()
{
  drawStatus("refreshing token");
  WiFiClientSecure client;
  client.setInsecure();  // no CA bundle on device; token travels over TLS but the server is not verified
  HTTPClient http;
  http.begin(client, REFRESH_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "claude-code/2.0");

  JsonDocument body;
  body["grant_type"] = "refresh_token";
  body["refresh_token"] = refreshToken;
  body["client_id"] = CLIENT_ID;
  String payload;
  serializeJson(body, payload);

  int code = http.POST(payload);
  if (code != 200) {
    Serial.printf("refresh failed: %d %s\n", code, http.getString().c_str());
    char b[32];
    snprintf(b, sizeof b, "refresh error %d", code);
    drawStatus(b, TFT_RED);
    http.end();
    return false;
  }
  JsonDocument res;
  DeserializationError err = deserializeJson(res, http.getStream());
  http.end();
  if (err || !res["access_token"].is<const char *>()) return false;

  accessToken = res["access_token"].as<String>();
  if (res["refresh_token"].is<const char *>()) refreshToken = res["refresh_token"].as<String>();
  expiresAtMs = (int64_t)time(nullptr) * 1000 + res["expires_in"].as<int64_t>() * 1000;
  prefs.putString("at", accessToken);
  prefs.putString("rt", refreshToken);
  prefs.putLong64("exp", expiresAtMs);
  return true;
}

bool fetchUsage()
{
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, USAGE_URL);
  http.addHeader("Authorization", "Bearer " + accessToken);
  http.addHeader("anthropic-beta", "oauth-2025-04-20");
  http.addHeader("User-Agent", "claude-code/2.0");

  int code = http.GET();
  if (code != 200) {
    http.end();
    char b[32];
    snprintf(b, sizeof b, "usage error %d", code);
    drawStatus(b, TFT_RED);
    return code == 401 ? refreshAccessToken() && fetchUsage() : false;
  }

  JsonDocument filter;
  filter["five_hour"]["utilization"] = true;
  filter["five_hour"]["resets_at"] = true;
  filter["seven_day"]["utilization"] = true;
  filter["seven_day"]["resets_at"] = true;
  JsonDocument doc;
  String body = http.getString();
  http.end();
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  Serial.printf("usage: len=%u err=%s\n", body.length(), err.c_str());
  if (err) {
    drawStatus("json error", TFT_RED);
    return false;
  }

  serializeJson(doc, Serial);
  Serial.println();
  sess.pct = lround(doc["five_hour"]["utilization"].as<float>());
  week.pct = lround(doc["seven_day"]["utilization"].as<float>());
  formatReset(doc["five_hour"]["resets_at"] | "", false, sess.reset, sizeof sess.reset);
  formatReset(doc["seven_day"]["resets_at"] | "", true, week.reset, sizeof week.reset);
  return true;
}

void connectWifi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  drawStatus("connecting wifi");
  while (WiFi.status() != WL_CONNECTED) delay(250);

  configTzTime(TZ_BRAZIL, "pool.ntp.org", "time.google.com");
  drawStatus("syncing time");
  while (time(nullptr) < 1700000000) delay(250);
}

void setup()
{
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);  // landscape 320x240
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("Claude Code usage", SCREEN_WIDTH / 2, 0, 2);
  render();

  loadTokens();
  connectWifi();
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED) connectWifi();

  // Refresh proactively when the access token is within 2 minutes of expiring.
  if ((int64_t)time(nullptr) * 1000 + 120000 > expiresAtMs) refreshAccessToken();

  if (fetchUsage()) {
    render();
    struct tm lt;
    getLocalTime(&lt);
    char b[32];
    strftime(b, sizeof b, "updated %H:%M:%S", &lt);
    drawStatus(b);
  }
  delay(POLL_MS);
}
