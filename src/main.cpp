/*
 * claude-usage
 * Board : ESP32 Cheap Yellow Display (ESP32-2432S028R), ILI9341 240x320
 *
 * Standalone: joins Wi-Fi, polls the Claude OAuth usage endpoint over HTTPS and
 * draws the session (5h) and weekly bars with their reset times. A small web page
 * on port 80 (IP shown on the TFT) lets you log in again without a PC tool.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <mbedtls/sha256.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "secrets.h"

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

static const char *USAGE_URL   = "https://api.anthropic.com/api/oauth/usage";
static const char *REFRESH_URL = "https://platform.claude.com/v1/oauth/token";
static const char *CLIENT_ID   = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
static const char *AUTHORIZE_URL = "https://claude.com/cai/oauth/authorize";
static const char *REDIRECT_URI  = "https://platform.claude.com/oauth/code/callback";
static const char *OAUTH_SCOPE   = "user:profile user:inference user:sessions:claude_code user:mcp_servers";
static const char *TZ_BRAZIL   = "<-03>3";
static const uint32_t POLL_MS  = 60000;

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;
WebServer server(80);
String pkceVerifier, pkceState;
bool pollNow = false;
bool loginNeeded = false;  // refresh token rejected: only a new web login can fix it

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

void drawFooter(uint16_t color = TFT_DARKGREY)
{
  tft.fillRect(0, 215, SCREEN_WIDTH, 25, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextDatum(BL_DATUM);
  tft.drawString(status, 6, SCREEN_HEIGHT - 4, 2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextDatum(BR_DATUM);
  tft.drawString(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("no wifi"),
                 SCREEN_WIDTH - 6, SCREEN_HEIGHT - 4, 2);
}

void drawStatus(const char *msg, uint16_t color = TFT_DARKGREY)
{
  snprintf(status, sizeof status, "%s", msg);
  drawFooter(color);
}

void drawLoginNeeded()
{
  tft.fillRect(0, 22, SCREEN_WIDTH, 190, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.drawString("Login expired", SCREEN_WIDTH / 2, 55, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Open in your browser:", SCREEN_WIDTH / 2, 110, 2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("http://" + WiFi.localIP().toString() + "/", SCREEN_WIDTH / 2, 135, 4);
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

// POSTs a token request and, on success, stores the new pair in RAM and NVS.
// On failure errBody carries the server's answer (never contains tokens).
bool postToken(const String &payload, int &httpCode, String &errBody)
{
  WiFiClientSecure client;
  client.setInsecure();  // no CA bundle on device; token travels over TLS but the server is not verified
  HTTPClient http;
  http.begin(client, REFRESH_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "claude-code/2.0");

  httpCode = http.POST(payload);
  String body = http.getString();
  http.end();
  if (httpCode != 200) {
    errBody = body;
    return false;
  }
  JsonDocument res;
  if (deserializeJson(res, body) || !res["access_token"].is<const char *>()) {
    errBody = "bad token response";
    return false;
  }

  accessToken = res["access_token"].as<String>();
  if (res["refresh_token"].is<const char *>()) refreshToken = res["refresh_token"].as<String>();
  expiresAtMs = (int64_t)time(nullptr) * 1000 + res["expires_in"].as<int64_t>() * 1000;
  prefs.putString("at", accessToken);
  prefs.putString("rt", refreshToken);
  prefs.putLong64("exp", expiresAtMs);
  return true;
}

bool refreshAccessToken()
{
  drawStatus("refreshing token");
  JsonDocument body;
  body["grant_type"] = "refresh_token";
  body["refresh_token"] = refreshToken;
  body["client_id"] = CLIENT_ID;
  String payload, err;
  serializeJson(body, payload);

  int code;
  if (postToken(payload, code, err)) return true;
  Serial.printf("refresh failed: %d %s\n", code, err.c_str());
  if (code == 400 || code == 401 || code == 403) {
    loginNeeded = true;
    drawLoginNeeded();
    drawStatus("login expired", TFT_RED);
  } else {
    char b[32];
    snprintf(b, sizeof b, "refresh error %d", code);
    drawStatus(b, TFT_RED);
  }
  return false;
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
    if (code == 401) return refreshAccessToken() && fetchUsage();
    char b[32];
    snprintf(b, sizeof b, "usage error %d", code);
    drawStatus(b, TFT_RED);
    return false;
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

String b64url(const uint8_t *d, size_t n)
{
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  String out;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    out += T[(v >> 18) & 63];
    out += T[(v >> 12) & 63];
    if (i + 1 < n) out += T[(v >> 6) & 63];
    if (i + 2 < n) out += T[v & 63];
  }
  return out;
}

String urlEncode(const char *s)
{
  String out;
  char b[4];
  for (; *s; s++) {
    if (isalnum((unsigned char)*s) || strchr("-._~", *s)) out += *s;
    else { snprintf(b, sizeof b, "%%%02X", (unsigned char)*s); out += b; }
  }
  return out;
}

// New PKCE pair; returns the authorization URL (already HTML-attribute safe: & -> &amp;).
String newLoginUrl()
{
  uint8_t rnd[32], hash[32];
  esp_fill_random(rnd, sizeof rnd);
  pkceVerifier = b64url(rnd, sizeof rnd);
  esp_fill_random(rnd, sizeof rnd);
  pkceState = b64url(rnd, sizeof rnd);
  mbedtls_sha256((const uint8_t *)pkceVerifier.c_str(), pkceVerifier.length(), hash, 0);

  return String(AUTHORIZE_URL) + "?code=true&amp;client_id=" + CLIENT_ID +
         "&amp;response_type=code&amp;redirect_uri=" + urlEncode(REDIRECT_URI) +
         "&amp;scope=" + urlEncode(OAUTH_SCOPE) +
         "&amp;code_challenge=" + b64url(hash, sizeof hash) +
         "&amp;code_challenge_method=S256&amp;state=" + pkceState;
}

void handleRoot()
{
  String h = F("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>Claude usage</title><style>body{font-family:sans-serif;max-width:34em;margin:2em auto;padding:0 1em}"
               "input[type=text]{width:100%;padding:.5em;box-sizing:border-box}button{padding:.5em 1em;margin-top:.5em}</style>"
               "<h2>Claude usage login</h2><p>Session: ");
  h += sess.pct >= 0 ? String(sess.pct) + "%" : String("--");
  h += F(" &middot; Weekly: ");
  h += week.pct >= 0 ? String(week.pct) + "%" : String("--");
  h += F("</p><p><b>1.</b> <a href=\"");
  h += newLoginUrl();
  h += F("\" target=_blank rel=noopener>Open Claude login</a>, authorize, and copy the code shown.</p>"
         "<form method=POST action=/code><p><b>2.</b> Paste the code:</p>"
         "<input type=text name=code autocomplete=off required><br><button>Save</button></form>");
  server.send(200, "text/html", h);
}

void handleCode()
{
  String pasted = server.arg("code");
  pasted.trim();
  int hash = pasted.indexOf('#');
  String code = hash >= 0 ? pasted.substring(0, hash) : pasted;
  String gotState = hash >= 0 ? pasted.substring(hash + 1) : String();
  if (pkceVerifier.isEmpty() || code.isEmpty() || (gotState.length() && gotState != pkceState)) {
    server.send(400, "text/plain", "Missing/invalid code or state. Go back and open a fresh login link.");
    return;
  }

  drawStatus("exchanging code");
  JsonDocument body;
  body["grant_type"] = "authorization_code";
  body["code"] = code;
  body["redirect_uri"] = REDIRECT_URI;
  body["client_id"] = CLIENT_ID;
  body["code_verifier"] = pkceVerifier;
  body["state"] = pkceState;
  String payload, err;
  serializeJson(body, payload);

  int httpCode;
  if (!postToken(payload, httpCode, err)) {
    char b[32];
    snprintf(b, sizeof b, "login error %d", httpCode);
    drawStatus(b, TFT_RED);
    server.send(502, "text/plain", String("Token exchange failed: ") + httpCode + " " + err);
    return;
  }
  pkceVerifier = "";
  loginNeeded = false;
  render();
  pollNow = true;
  drawStatus("login ok");
  server.sendHeader("Location", "/");
  server.send(303);
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
  drawFooter();
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

  server.on("/", HTTP_GET, handleRoot);
  server.on("/code", HTTP_POST, handleCode);
  server.begin();
  Serial.printf("web login at http://%s/\n", WiFi.localIP().toString().c_str());
}

void pollUsage()
{
  // Refresh proactively when the access token is within 2 minutes of expiring.
  if ((int64_t)time(nullptr) * 1000 + 120000 > expiresAtMs) refreshAccessToken();
  if (loginNeeded) return;

  if (fetchUsage()) {
    render();
    struct tm lt;
    getLocalTime(&lt);
    char b[32];
    strftime(b, sizeof b, "updated %H:%M:%S", &lt);
    drawStatus(b);
  }
}

void loop()
{
  static uint32_t nextPollMs = 0;
  if (WiFi.status() != WL_CONNECTED) connectWifi();

  server.handleClient();
  if (pollNow || (!loginNeeded && (int32_t)(millis() - nextPollMs) >= 0)) {
    pollNow = false;
    pollUsage();
    nextPollMs = millis() + POLL_MS;
  }
  delay(5);
}
