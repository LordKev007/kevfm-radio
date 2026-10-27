/*
 * KevFM radio web pages and endpoints (see README.md, "Web pages").
 *
 *   /                 KevFM player page (setup page while the radio is in hotspot mode)
 *   /setup            Wi-Fi setup page (scan list + type it in), also reachable when connected
 *   /firmware         version, check GitHub for an update, upload a .bin (kevfm_ota.cpp)
 *   /settings         KevFM Settings: radio name, knob, tone, restart, factory reset (kevfm_settings.cpp)
 *   /settings.html    yoRadio's own pages: not linked anywhere, kept for Kevin (type the address)
 *   GET  /kevfm/status    now playing, volume, sleep timer, light, Wi-Fi
 *   POST /kevfm/set       volume=0..254 | sleep=0/10/15/30/60 | light=0..2
 *   GET  /kevfm/scan      Wi-Fi networks nearby (starts a scan; ?refresh=1 rescans)
 *   GET  /kevfm/networks  saved network names (never the passwords)
 *   POST /kevfm/wifi      ssid, pass: save as first choice, restart
 *   POST /kevfm/forget    ssid: remove a saved network
 *
 * Handlers run on the web server's task: they only read state or leave a request for the
 * main loop (kevfm.webSleep / webLight / restartAt), except volume, which goes through
 * yoRadio's player queue like the knob.
 */
#include "kevfm.h"
#include "kevfm_station.h"
#include "kevfm_ota.h"
#include "kevfm_settings.h"
#include "kevfm_version.h"
#include "kevfm_pages.h"
#include "../../core/options.h"
#include "../../core/config.h"
#include "../../core/player.h"
#include "../../core/network.h"
#include "../../AsyncWebServer/ESPAsyncWebServer.h"
#include <SPIFFS.h>
#include <WiFi.h>
#include <DNSServer.h>

void handleIndex(AsyncWebServerRequest *request);   /* yoRadio, netserver.cpp */

static DNSServer dns;
static bool dnsStarted = false;
static bool firstScanStarted = false;
static String scanJson;                             /* last finished scan, as a JSON array */
static portMUX_TYPE scanMux = portMUX_INITIALIZER_UNLOCKED;

static const uint8_t KEVFM_MAX_NETWORKS = 5;        /* yoRadio's config.ssids[5] */
static const uint8_t KEVFM_MAX_SSID = 29;           /* yoRadio's ssid[30] */
static const uint8_t KEVFM_MAX_PASS = 39;           /* yoRadio's password[40] */

/* ---------------------------------------------------------------- helpers */
static void jsonEscape(String &out, const char *s) {
  out += '"';
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
    else out += c;
  }
  out += '"';
}

/* The KevFM pages live in the firmware (kevfm_pages.h, gzipped), so they always match it
   and an update never has to write them to the radio's small file storage. */
enum kevfmPage_e { PAGE_PLAYER, PAGE_SETUP, PAGE_FIRMWARE, PAGE_SETTINGS };

static void sendPage(AsyncWebServerRequest *request, kevfmPage_e page) {
  const uint8_t *data = kevfm_page_kevfm_html;
  size_t len = kevfm_page_kevfm_html_len;
  if (page == PAGE_SETUP)    { data = kevfm_page_setup_html;    len = kevfm_page_setup_html_len; }
  if (page == PAGE_FIRMWARE) { data = kevfm_page_firmware_html; len = kevfm_page_firmware_html_len; }
  if (page == PAGE_SETTINGS) { data = kevfm_page_settings_html; len = kevfm_page_settings_html_len; }
  AsyncWebServerResponse *r = request->beginResponse_P(200, "text/html", data, len);
  r->addHeader("Content-Encoding", "gzip");
  r->addHeader("Cache-Control", "no-cache");
  request->send(r);
}

/* Copies of the pages that 0.2.0 kept in storage: no longer used, and storage is tight */
static void removeOldPageFiles() {
  static const char *const old[] = {"/www/kevfm.html", "/www/setup.html", "/www/firmware.html"};
  for (const char *path : old) {
    if (SPIFFS.exists(path) && SPIFFS.remove(path)) Serial.printf("##[KevFM]#\tremoved old %s from storage\n", path);
  }
}

static void sendJson(AsyncWebServerRequest *request, const String &json, int code = 200) {
  AsyncWebServerResponse *r = request->beginResponse(code, "application/json", json);
  r->addHeader("Cache-Control", "no-store");
  request->send(r);
}

static String param(AsyncWebServerRequest *request, const char *name) {
  if (request->hasParam(name, true)) return request->getParam(name, true)->value();
  if (request->hasParam(name)) return request->getParam(name)->value();
  return String();
}

/* saved networks as (ssid, pass) lines, in file order */
static uint8_t readSaved(String ssids[], String passes[], uint8_t max) {
  uint8_t n = 0;
  File f = SPIFFS.open(SSIDS_PATH, "r");
  if (!f) return 0;
  while (f.available() && n < max) {
    String line = f.readStringUntil('\n');
    line.replace("\r", "");
    int tab = line.indexOf('\t');
    if (tab <= 0) continue;
    ssids[n] = line.substring(0, tab);
    passes[n] = line.substring(tab + 1);
    n++;
  }
  f.close();
  return n;
}

static bool writeSaved(String ssids[], String passes[], uint8_t n) {
  File f = SPIFFS.open(SSIDS_PATH, "w");
  if (!f) return false;
  for (uint8_t i = 0; i < n; i++) { f.print(ssids[i]); f.print('\t'); f.print(passes[i]); f.print('\n'); }
  f.close();
  return true;
}

/* ---------------------------------------------------------------- Wi-Fi scan */
static void startScan() {
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) return;
  WiFi.scanDelete();
  WiFi.scanNetworks(true, false);                   /* async, no hidden networks */
}

/* if a scan has finished, turn it into JSON (strongest first, one entry per name) */
static void collectScan() {
  int n = WiFi.scanComplete();
  if (n < 0) return;
  struct Net { String ssid; int rssi; bool open; };
  Net *nets = new Net[n > 0 ? n : 1];
  int count = 0;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length() || s.length() > KEVFM_MAX_SSID) continue;
    if (s.startsWith("KevFM-Radio")) continue;       /* our own or another radio's setup hotspot */
    int rssi = WiFi.RSSI(i);
    bool open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    int j = 0;
    for (; j < count; j++) if (nets[j].ssid == s) break;
    if (j < count) { if (rssi > nets[j].rssi) nets[j].rssi = rssi; continue; }
    nets[count++] = {s, rssi, open};
  }
  for (int i = 1; i < count; i++)                   /* strongest first */
    for (int j = i; j > 0 && nets[j].rssi > nets[j - 1].rssi; j--) { Net t = nets[j]; nets[j] = nets[j - 1]; nets[j - 1] = t; }
  String out = "[";
  for (int i = 0; i < count; i++) {
    if (i) out += ',';
    out += "{\"ssid\":"; jsonEscape(out, nets[i].ssid.c_str());
    out += ",\"rssi\":"; out += nets[i].rssi;
    out += ",\"open\":"; out += nets[i].open ? "true" : "false";
    out += '}';
  }
  out += ']';
  delete[] nets;
  WiFi.scanDelete();
  portENTER_CRITICAL(&scanMux);
  scanJson = out;
  portEXIT_CRITICAL(&scanMux);
}

/* main loop: captive DNS and a first scan while in hotspot mode, so the list is ready
   by the time a phone opens the page */
void kevfmWebLoop() {
  static bool cleaned = false;
  if (!cleaned) { cleaned = true; removeOldPageFiles(); }
  if (network.status != SOFT_AP) return;
  if (!dnsStarted) {
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dnsStarted = dns.start(53, "*", WiFi.softAPIP());
    Serial.printf("##[KevFM]#\tsetup hotspot, captive page %s\n", dnsStarted ? "on" : "FAILED");
  }
  dns.processNextRequest();
  if (!firstScanStarted) { firstScanStarted = true; startScan(); }
}

/* ---------------------------------------------------------------- handlers */
static void onRoot(AsyncWebServerRequest *request) {
  /* yoRadio's own cases: empty file system (Web Board Uploader) and ?command=value calls */
  if (config.emptyFS || request->params() > 0 || request->method() != HTTP_GET) { handleIndex(request); return; }
  sendPage(request, network.status == CONNECTED ? PAGE_PLAYER : PAGE_SETUP);
}

static void onCaptive(AsyncWebServerRequest *request) {
  /* phones probe these on a new Wi-Fi; anything but the expected answer opens the sign-in
     sheet, which then shows the setup page */
  if (network.status == SOFT_AP) {
    request->redirect(String("http://") + WiFi.softAPIP().toString() + "/setup");
  } else {
    request->send(404, "text/plain", "Not found");
  }
}

static void onStatus(AsyncWebServerRequest *request) {
  char title[BUFLEN], name[BUFLEN];
  strlcpy(title, config.station.title, sizeof(title));
  strlcpy(name, config.station.name, sizeof(name));
  String out = "{\"station\":";
  jsonEscape(out, name[0] ? name : KEVFM_STATION_NAME);
  out += ",\"title\":"; jsonEscape(out, title);
  out += ",\"playing\":"; out += player.status() == PLAYING ? "true" : "false";
  out += ",\"volume\":"; out += config.store.volume;
  out += ",\"sleep\":"; out += kevfm.sleepSetting();
  out += ",\"left\":"; out += kevfm.sleepSecondsLeft();
  out += ",\"light\":"; out += kevfm.lightMode();
  out += ",\"mode\":"; jsonEscape(out, network.status == CONNECTED ? "connected" : "hotspot");
  out += ",\"ssid\":"; jsonEscape(out, network.status == CONNECTED ? WiFi.SSID().c_str() : "");
  out += ",\"rssi\":"; out += network.status == CONNECTED ? WiFi.RSSI() : 0;
  out += ",\"ip\":"; jsonEscape(out, (network.status == CONNECTED ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str());
  out += ",\"version\":"; jsonEscape(out, KEVFM_VERSION);
  out += '}';
  sendJson(request, out);
}

static void onSet(AsyncWebServerRequest *request) {
  String v = param(request, "volume");
  if (v.length()) {
    int vol = v.toInt();
    if (vol < 0) vol = 0;
    if (vol > 254) vol = 254;
    player.setVol((uint8_t)vol);
  }
  v = param(request, "sleep");
  if (v.length()) {
    int m = v.toInt();
    if (m == 0 || m == 10 || m == 15 || m == 30 || m == 60) kevfm.webSleep = (int8_t)m;
    else { sendJson(request, "{\"ok\":false,\"error\":\"sleep must be 0, 10, 15, 30 or 60\"}", 400); return; }
  }
  v = param(request, "light");
  if (v.length()) {
    int l = v.toInt();
    if (l >= 0 && l <= 2) kevfm.webLight = (int8_t)l;
    else { sendJson(request, "{\"ok\":false,\"error\":\"light must be 0, 1 or 2\"}", 400); return; }
  }
  sendJson(request, "{\"ok\":true}");
}

static void onScan(AsyncWebServerRequest *request) {
  collectScan();
  bool refresh = request->hasParam("refresh");
  String cached;
  portENTER_CRITICAL(&scanMux);
  cached = scanJson;
  portEXIT_CRITICAL(&scanMux);
  bool running = WiFi.scanComplete() == WIFI_SCAN_RUNNING;
  if ((refresh || !cached.length()) && !running) { startScan(); running = true; }
  String out = "{\"scanning\":";
  out += running ? "true" : "false";
  out += ",\"networks\":";
  out += cached.length() ? cached : String("[]");
  out += '}';
  sendJson(request, out);
}

static void onNetworks(AsyncWebServerRequest *request) {
  String ssids[KEVFM_MAX_NETWORKS], passes[KEVFM_MAX_NETWORKS];
  uint8_t n = readSaved(ssids, passes, KEVFM_MAX_NETWORKS);
  String out = "{\"max\":";
  out += KEVFM_MAX_NETWORKS;
  out += ",\"saved\":[";
  for (uint8_t i = 0; i < n; i++) { if (i) out += ','; jsonEscape(out, ssids[i].c_str()); }
  out += "]}";
  sendJson(request, out);
}

static bool validText(const String &s) {
  for (size_t i = 0; i < s.length(); i++) if (s[i] == '\t' || s[i] == '\n' || s[i] == '\r') return false;
  return true;
}

static void onWifi(AsyncWebServerRequest *request) {
  String ssid = param(request, "ssid"), pass = param(request, "pass");
  if (!ssid.length() || ssid.length() > KEVFM_MAX_SSID || !validText(ssid)) {
    sendJson(request, "{\"ok\":false,\"error\":\"The network name must be 1 to 29 characters.\"}", 400); return;
  }
  if (pass.length() > KEVFM_MAX_PASS || !validText(pass) || (pass.length() > 0 && pass.length() < 8)) {
    sendJson(request, "{\"ok\":false,\"error\":\"The password must be 8 to 39 characters (or empty for an open network).\"}", 400); return;
  }
  String ssids[KEVFM_MAX_NETWORKS + 1], passes[KEVFM_MAX_NETWORKS + 1];
  String oldS[KEVFM_MAX_NETWORKS], oldP[KEVFM_MAX_NETWORKS];
  uint8_t old = readSaved(oldS, oldP, KEVFM_MAX_NETWORKS);
  uint8_t n = 0;
  ssids[n] = ssid; passes[n] = pass; n++;          /* new one first: tried first after restart */
  for (uint8_t i = 0; i < old && n < KEVFM_MAX_NETWORKS; i++) {
    if (oldS[i] == ssid) continue;
    ssids[n] = oldS[i]; passes[n] = oldP[i]; n++;
  }
  if (!writeSaved(ssids, passes, n)) { sendJson(request, "{\"ok\":false,\"error\":\"Could not save.\"}", 500); return; }
  Serial.printf("##[KevFM]#\tWi-Fi saved: %s (%u networks)\n", ssid.c_str(), n);
  kevfm.restartAt = millis() + 2000;               /* let this answer reach the phone first */
  sendJson(request, "{\"ok\":true}");
}

static void onForget(AsyncWebServerRequest *request) {
  String ssid = param(request, "ssid");
  String ssids[KEVFM_MAX_NETWORKS], passes[KEVFM_MAX_NETWORKS];
  uint8_t old = readSaved(ssids, passes, KEVFM_MAX_NETWORKS), n = 0;
  for (uint8_t i = 0; i < old; i++) {
    if (ssids[i] == ssid) continue;
    ssids[n] = ssids[i]; passes[n] = passes[i]; n++;
  }
  if (n == old) { sendJson(request, "{\"ok\":false,\"error\":\"Not saved.\"}", 404); return; }
  if (!writeSaved(ssids, passes, n)) { sendJson(request, "{\"ok\":false,\"error\":\"Could not save.\"}", 500); return; }
  config.saveValue(&config.store.lastSSID, (uint8_t)0);   /* indexes moved; start from the top next boot */
  sendJson(request, "{\"ok\":true}");
}

/* called by the KevFM patch in NetServer::begin(), before yoRadio adds its own "/" */
void kevfm_web_begin(AsyncWebServer &server) {
  server.on("/", HTTP_ANY, onRoot);
  server.on("/setup", HTTP_GET, [](AsyncWebServerRequest *r) { sendPage(r, PAGE_SETUP); });
  server.on("/firmware", HTTP_GET, [](AsyncWebServerRequest *r) { sendPage(r, PAGE_FIRMWARE); });
  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *r) { sendPage(r, PAGE_SETTINGS); });
  kevfmOtaBegin(server);
  kevfmSettingsBegin(server);
  server.on("/kevfm/status", HTTP_GET, onStatus);
  server.on("/kevfm/set", HTTP_POST, onSet);
  server.on("/kevfm/scan", HTTP_GET, onScan);
  server.on("/kevfm/networks", HTTP_GET, onNetworks);
  server.on("/kevfm/wifi", HTTP_POST, onWifi);
  server.on("/kevfm/forget", HTTP_POST, onForget);
  static const char *const probes[] = {
    "/hotspot-detect.html", "/library/test/success.html",             /* Apple */
    "/generate_204", "/gen_204",                                      /* Android */
    "/connecttest.txt", "/ncsi.txt", "/redirect", "/fwlink",          /* Windows */
    "/canonical.html", "/success.txt"                                 /* Firefox, others */
  };
  for (const char *p : probes) server.on(p, HTTP_ANY, onCaptive);
}
