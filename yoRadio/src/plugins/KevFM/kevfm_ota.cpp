/*
 * KevFM radio firmware updates, the same way as the Bubs stock ticker: nothing happens
 * unless someone presses a button on the radio's Firmware page.
 *
 *   POST /kevfm/update/check     read update.json from GitHub (raw file), compare versions
 *   POST /kevfm/update/install   download the newer .bin (+ any web files listed), restart
 *   POST /kevfm/update/upload    install a .bin file sent from the browser (no GitHub needed)
 *   GET  /kevfm/update/status    state, message, versions, progress
 *
 * update.json:  {"version": "0.2.1", "file": "kevfm-radio.bin", "size": 1484051,
 *                "web": ["kevfm.html", "setup.html", "firmware.html"]}
 *   "file" is a plain file name in the repo root; "size" and "web" are optional. Web files
 *   come from the repo's yoRadio/data/www/ and replace the radio's copies (saved Wi-Fi is
 *   never touched).
 *
 * Check and install run on the main loop (the web request only queues them and returns),
 * with the stream stopped while installing: this board has no PSRAM, and https needs the
 * memory the stream buffer uses.
 */
#include "kevfm.h"
#include "kevfm_ota.h"
#include "kevfm_version.h"
#include "../../core/options.h"
#include "../../core/config.h"
#include "../../core/player.h"
#include "../../core/network.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <SPIFFS.h>
#include <errno.h>

enum otaState_e : uint8_t { OTA_IDLE, OTA_CHECKING, OTA_CHECKED, OTA_INSTALLING, OTA_DONE, OTA_FAILED };
static const char *const kStateName[] = {"idle", "checking", "checked", "installing", "done", "failed"};

static portMUX_TYPE otaMux = portMUX_INITIALIZER_UNLOCKED;
static volatile otaState_e otaState = OTA_IDLE;
static volatile bool checkQueued = false, installQueued = false, uploadActive = false;
static volatile uint8_t otaProgress = 0;
static volatile uint32_t otaRestartAt = 0;
static char otaMessage[160] = "";
static char latestVersion[24] = "";
static char latestFile[64] = "";
static uint32_t latestSize = 0;
static bool updateAvailable = false;
static const uint8_t KEVFM_MAX_WEB = 8;
static String webFiles[KEVFM_MAX_WEB];
static uint8_t webCount = 0;
static bool uploadOk = false;
static char uploadError[96] = "";

static void setMsg(otaState_e st, const char *fmt, ...) {
  char buf[sizeof(otaMessage)];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  portENTER_CRITICAL(&otaMux);
  otaState = st;
  strlcpy(otaMessage, buf, sizeof(otaMessage));
  portEXIT_CRITICAL(&otaMux);
  Serial.printf("##[KevFM]#\tupdate: %s\n", buf);
}

bool kevfmOtaBusy() {
  return otaState == OTA_CHECKING || otaState == OTA_INSTALLING || checkQueued || installQueued || uploadActive || otaRestartAt;
}

/* ---------------------------------------------------------------- tiny JSON readers */
static bool jsonString(const String &js, const char *key, String &out) {
  String k = String("\"") + key + "\"";
  int i = js.indexOf(k); if (i < 0) return false;
  i = js.indexOf(':', i + k.length()); if (i < 0) return false;
  i = js.indexOf('"', i); if (i < 0) return false;
  int j = js.indexOf('"', i + 1); if (j < 0) return false;
  out = js.substring(i + 1, j);
  return true;
}

static bool jsonUInt(const String &js, const char *key, uint32_t &out) {
  String k = String("\"") + key + "\"";
  int i = js.indexOf(k); if (i < 0) return false;
  i = js.indexOf(':', i + k.length()); if (i < 0) return false;
  i++;
  while (i < (int)js.length() && js[i] == ' ') i++;
  if (i >= (int)js.length() || !isDigit(js[i])) return false;
  out = (uint32_t)js.substring(i).toInt();
  return true;
}

static uint8_t jsonStringArray(const String &js, const char *key, String out[], uint8_t max) {
  String k = String("\"") + key + "\"";
  int i = js.indexOf(k); if (i < 0) return 0;
  i = js.indexOf('[', i + k.length()); if (i < 0) return 0;
  int end = js.indexOf(']', i); if (end < 0) return 0;
  uint8_t n = 0;
  while (n < max) {
    int a = js.indexOf('"', i); if (a < 0 || a > end) break;
    int b = js.indexOf('"', a + 1); if (b < 0 || b > end) break;
    out[n++] = js.substring(a + 1, b);
    i = b + 1;
  }
  return n;
}

static bool plainName(const String &s) {
  return s.length() > 0 && s.length() < 48 && s.indexOf("..") < 0 && s.indexOf('/') < 0 &&
         s.indexOf('\\') < 0 && s.indexOf(':') < 0;
}

/* "0.2.10" > "0.2.9"; a leading v and anything after '-' are ignored */
static int compareVersions(String a, String b) {
  for (String *s : {&a, &b}) {
    s->trim();
    if (s->startsWith("v") || s->startsWith("V")) s->remove(0, 1);
    int d = s->indexOf('-'); if (d >= 0) *s = s->substring(0, d);
  }
  int pa = 0, pb = 0;
  for (int part = 0; part < 3; part++) {
    int ea = a.indexOf('.', pa), eb = b.indexOf('.', pb);
    long va = a.substring(pa, ea < 0 ? a.length() : ea).toInt();
    long vb = b.substring(pb, eb < 0 ? b.length() : eb).toInt();
    if (va != vb) return va < vb ? -1 : 1;
    pa = ea < 0 ? a.length() : ea + 1;
    pb = eb < 0 ? b.length() : eb + 1;
  }
  return 0;
}

/* ---------------------------------------------------------------- stream pause */
static bool stopStream() {
  bool was = player.status() == PLAYING;
  if (was) {
    player.sendCommand({PR_STOP, 0});
    player.loop();                                  /* we are on the main loop: run it now */
    delay(300);
  }
  return was;
}

/* ---------------------------------------------------------------- check */
static void doCheck() {
  setMsg(OTA_CHECKING, "Checking GitHub for new firmware...");
  if (WiFi.status() != WL_CONNECTED) { setMsg(OTA_FAILED, "The radio is not connected to the Internet."); return; }
  uint32_t big = ESP.getMaxAllocHeap();
  bool paused = false;
  if (big < 45000) paused = stopStream();           /* not enough room for https while streaming */
  Serial.printf("##[KevFM]#\tupdate check: largest free block %u bytes%s\n", big, paused ? ", stream paused" : "");

  WiFiClientSecure client;
  client.setInsecure();                             /* as the ticker: plain raw.githubusercontent.com */
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(12000);
  String payload;
  int code = -1;
  if (http.begin(client, KEVFM_UPDATE_MANIFEST)) {
    code = http.GET();
    if (code == HTTP_CODE_OK) payload = http.getString();
    http.end();
  }
  client.stop();
  if (paused) player.sendCommand({PR_PLAY, 1});

  if (code != HTTP_CODE_OK) {
    if (code < 0) setMsg(OTA_FAILED, "Couldn't reach GitHub (%s). You can still upload a .bin file below.", HTTPClient::errorToString(code).c_str());
    else setMsg(OTA_FAILED, "GitHub answered HTTP %d for update.json. You can still upload a .bin file below.", code);
    return;
  }
  String ver, file;
  uint32_t size = 0;
  if (!jsonString(payload, "version", ver) || !ver.length()) { setMsg(OTA_FAILED, "update.json has no version."); return; }
  if (!jsonString(payload, "file", file)) file = "kevfm-radio.bin";
  file.trim();
  if (!plainName(file) || !file.endsWith(".bin")) { setMsg(OTA_FAILED, "update.json has an invalid file name."); return; }
  jsonUInt(payload, "size", size);
  String web[KEVFM_MAX_WEB];
  uint8_t nweb = jsonStringArray(payload, "web", web, KEVFM_MAX_WEB);
  for (uint8_t i = 0; i < nweb; i++) if (!plainName(web[i])) { setMsg(OTA_FAILED, "update.json has an invalid web file name."); return; }

  int cmp = compareVersions(ver, KEVFM_VERSION);
  portENTER_CRITICAL(&otaMux);
  strlcpy(latestVersion, ver.c_str(), sizeof(latestVersion));
  strlcpy(latestFile, file.c_str(), sizeof(latestFile));
  latestSize = size;
  updateAvailable = cmp > 0;
  portEXIT_CRITICAL(&otaMux);
  webCount = nweb;
  for (uint8_t i = 0; i < nweb; i++) webFiles[i] = web[i];
  if (cmp > 0) setMsg(OTA_CHECKED, "Version %s is available (this radio has %s).", ver.c_str(), KEVFM_VERSION);
  else if (cmp == 0) setMsg(OTA_CHECKED, "This radio already has the latest firmware (%s).", KEVFM_VERSION);
  else setMsg(OTA_CHECKED, "This radio (%s) is newer than the published firmware (%s).", KEVFM_VERSION, ver.c_str());
}

/* ---------------------------------------------------------------- install */
static bool download(const String &url, const char *what, uint32_t expect, bool toFlash, const char *spiffsPath) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(15000);
  if (!http.begin(client, url)) { setMsg(OTA_FAILED, "Couldn't open the %s download.", what); return false; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    if (code < 0) setMsg(OTA_FAILED, "%s download failed: %s.", what, HTTPClient::errorToString(code).c_str());
    else setMsg(OTA_FAILED, "GitHub answered HTTP %d for the %s.", code, what);
    http.end(); client.stop(); return false;
  }
  int len = http.getSize();
  uint32_t room = ESP.getFreeSketchSpace() & 0xFFFFF000;
  File f;
  if (toFlash) {
    if ((len > 0 && (uint32_t)len > room) || (expect && expect > room)) { setMsg(OTA_FAILED, "The new firmware is too big for this radio."); http.end(); client.stop(); return false; }
    if (!Update.begin(len > 0 ? (uint32_t)len : room, U_FLASH)) { setMsg(OTA_FAILED, "Couldn't start writing the firmware (error %u).", Update.getError()); http.end(); client.stop(); return false; }
  } else {
    f = SPIFFS.open(spiffsPath, "w");
    if (!f) { setMsg(OTA_FAILED, "Couldn't save %s.", what); http.end(); client.stop(); return false; }
  }
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  size_t written = 0;
  uint32_t lastData = millis();
  bool failed = false;
  int lastPct = -1;
  while ((http.connected() || stream->available()) && (len < 0 || written < (size_t)len)) {
    size_t avail = stream->available();
    if (!avail) {
      if (millis() - lastData > 15000) { setMsg(OTA_FAILED, "The %s download timed out.", what); failed = true; break; }
      delay(1);
      continue;
    }
    int n = stream->readBytes((char *)buf, avail > sizeof(buf) ? sizeof(buf) : avail);
    if (n <= 0) { setMsg(OTA_FAILED, "The %s download stopped.", what); failed = true; break; }
    size_t w = toFlash ? Update.write(buf, n) : f.write(buf, n);
    if (w != (size_t)n) {
      if (toFlash) setMsg(OTA_FAILED, "Writing the firmware failed (error %u).", Update.getError());
      else setMsg(OTA_FAILED, "Saving %s failed after %u of %d bytes (wrote %u of %d; storage %u of %u bytes used, errno %d).",
                  what, (unsigned)written, len, (unsigned)w, n, (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes(), errno);
      failed = true;
      break;
    }
    written += w;
    lastData = millis();
    if (toFlash && len > 0) {
      int pct = (int)(written * 100ULL / (size_t)len);
      otaProgress = pct > 99 ? 99 : pct;
      if (pct / 10 != lastPct) { lastPct = pct / 10; setMsg(OTA_INSTALLING, "Downloading firmware: %d%%", pct); }
    }
  }
  http.end();
  client.stop();
  if (!failed && len > 0 && written != (size_t)len) { setMsg(OTA_FAILED, "The %s download ended early.", what); failed = true; }
  if (toFlash) {
    if (failed) { Update.abort(); return false; }
    if (!Update.end(true)) { setMsg(OTA_FAILED, "The new firmware didn't check out (error %u). Nothing was changed.", Update.getError()); return false; }
  } else {
    f.close();
    if (failed) { SPIFFS.remove(spiffsPath); return false; }
  }
  return true;
}

static void doInstall() {
  char file[sizeof(latestFile)], ver[sizeof(latestVersion)];
  uint32_t size;
  bool avail;
  portENTER_CRITICAL(&otaMux);
  strlcpy(file, latestFile, sizeof(file)); strlcpy(ver, latestVersion, sizeof(ver));
  size = latestSize; avail = updateAvailable;
  portEXIT_CRITICAL(&otaMux);
  if (!avail) { setMsg(OTA_FAILED, "Check for an update first."); return; }
  if (WiFi.status() != WL_CONNECTED) { setMsg(OTA_FAILED, "The radio is not connected to the Internet."); return; }
  otaProgress = 0;
  setMsg(OTA_INSTALLING, "Stopping the music for the update...");
  kevfm.speakNow("update_start");                   /* "Updating the radio. Please don't unplug me." */
  stopStream();
  Serial.printf("##[KevFM]#\tupdate install: largest free block %u bytes, storage %u of %u bytes used\n",
                ESP.getMaxAllocHeap(), (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes());

  /* web files first, to temporary names; they replace the old ones only if the firmware
     installs too, so pages and firmware stay matched */
  for (uint8_t i = 0; i < webCount; i++) {
    setMsg(OTA_INSTALLING, "Downloading web page %s...", webFiles[i].c_str());
    String tmp = "/www/" + webFiles[i] + ".new";
    if (!download(String(KEVFM_UPDATE_BASE) + KEVFM_UPDATE_WEBDIR + webFiles[i], webFiles[i].c_str(), 0, false, tmp.c_str())) {
      for (uint8_t j = 0; j <= i; j++) SPIFFS.remove("/www/" + webFiles[j] + ".new");
      kevfm.speakNow("update_failed");              /* "The update didn't work. I'll keep playing." */
      player.sendCommand({PR_PLAY, 1});
      return;
    }
  }
  setMsg(OTA_INSTALLING, "Downloading firmware %s...", ver);
  if (!download(String(KEVFM_UPDATE_BASE) + file, "firmware", size, true, nullptr)) {
    for (uint8_t j = 0; j < webCount; j++) SPIFFS.remove("/www/" + webFiles[j] + ".new");
    kevfm.speakNow("update_failed");
    player.sendCommand({PR_PLAY, 1});
    return;
  }
  for (uint8_t i = 0; i < webCount; i++) {
    String fin = "/www/" + webFiles[i];
    SPIFFS.remove(fin);
    SPIFFS.rename(fin + ".new", fin);
  }
  otaProgress = 100;
  setMsg(OTA_DONE, "Version %s installed. The radio is restarting...", ver);
  otaRestartAt = millis() + 2500;                   /* let the page see "done" first */
}

/* main loop */
void kevfmOtaLoop() {
  if (checkQueued) { checkQueued = false; doCheck(); }
  if (installQueued) { installQueued = false; doInstall(); }
  if (otaRestartAt && (int32_t)(millis() - otaRestartAt) >= 0) {
    Serial.println("##[KevFM]#\tupdate: restarting");
    delay(100);
    ESP.restart();
  }
}

/* ---------------------------------------------------------------- web handlers */
static void sendJson(AsyncWebServerRequest *request, const String &json, int code = 200) {
  AsyncWebServerResponse *r = request->beginResponse(code, "application/json", json);
  r->addHeader("Cache-Control", "no-store");
  request->send(r);
}

static void addEscaped(String &out, const char *s) {
  out += '"';
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') out += '\\';
    if ((uint8_t)*s >= 0x20) out += *s;
  }
  out += '"';
}

static void onStatus(AsyncWebServerRequest *request) {
  char msg[sizeof(otaMessage)], ver[sizeof(latestVersion)];
  bool avail; uint32_t size; otaState_e st;
  portENTER_CRITICAL(&otaMux);
  strlcpy(msg, otaMessage, sizeof(msg)); strlcpy(ver, latestVersion, sizeof(ver));
  avail = updateAvailable; size = latestSize; st = otaState;
  portEXIT_CRITICAL(&otaMux);
  if (checkQueued) st = OTA_CHECKING;
  if (installQueued) st = OTA_INSTALLING;
  String out = "{\"state\":\"";
  out += kStateName[st];
  out += "\",\"message\":"; addEscaped(out, msg);
  out += ",\"current\":\"" KEVFM_VERSION "\",\"latest\":"; addEscaped(out, ver);
  out += ",\"available\":"; out += avail ? "true" : "false";
  out += ",\"size\":"; out += size;
  out += ",\"progress\":"; out += otaProgress;
  out += ",\"repo\":\"" KEVFM_UPDATE_BASE "\"}";
  sendJson(request, out);
}

static void onCheck(AsyncWebServerRequest *request) {
  if (kevfmOtaBusy()) { sendJson(request, "{\"ok\":false,\"error\":\"An update is already running.\"}", 409); return; }
  checkQueued = true;
  sendJson(request, "{\"ok\":true}");
}

static void onInstall(AsyncWebServerRequest *request) {
  if (kevfmOtaBusy()) { sendJson(request, "{\"ok\":false,\"error\":\"An update is already running.\"}", 409); return; }
  if (!updateAvailable) { sendJson(request, "{\"ok\":false,\"error\":\"Check for an update first.\"}", 409); return; }
  installQueued = true;
  sendJson(request, "{\"ok\":true}");
}

/* manual .bin upload: runs on the web server's task, like yoRadio's own /update */
static void onUploadData(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (!index) {
    uploadOk = false; uploadError[0] = 0;
    if (otaState == OTA_INSTALLING || checkQueued || installQueued || otaRestartAt) { strlcpy(uploadError, "An update is already running.", sizeof(uploadError)); return; }
    uploadActive = true;
    otaProgress = 0;
    setMsg(OTA_INSTALLING, "Receiving %s...", filename.c_str());
    player.sendCommand({PR_STOP, 0});
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      snprintf(uploadError, sizeof(uploadError), "Couldn't start writing the firmware (error %u).", Update.getError());
      uploadActive = false;
      return;
    }
  }
  if (!uploadActive || uploadError[0]) return;
  if (len && Update.write(data, len) != len) {
    snprintf(uploadError, sizeof(uploadError), "Writing the firmware failed (error %u).", Update.getError());
    Update.abort();
    uploadActive = false;
    return;
  }
  size_t total = request->contentLength();
  if (total) otaProgress = (uint8_t)min((size_t)99, (index + len) * 100 / total);
  if (final) {
    if (Update.end(true)) uploadOk = true;
    else snprintf(uploadError, sizeof(uploadError), "That file isn't valid firmware for this radio (error %u). Nothing was changed.", Update.getError());
    uploadActive = false;
  }
}

static void onUploadDone(AsyncWebServerRequest *request) {
  if (uploadOk) {
    otaProgress = 100;
    setMsg(OTA_DONE, "Firmware file installed. The radio is restarting...");
    otaRestartAt = millis() + 2500;
    sendJson(request, "{\"ok\":true}");
  } else {
    const char *err = uploadError[0] ? uploadError : "The upload didn't arrive.";
    setMsg(OTA_FAILED, "%s", err);
    player.sendCommand({PR_PLAY, 1});
    String out = "{\"ok\":false,\"error\":"; addEscaped(out, err); out += '}';
    sendJson(request, out, 400);
  }
}

void kevfmOtaBegin(AsyncWebServer &server) {
  server.on("/kevfm/update/status", HTTP_GET, onStatus);
  server.on("/kevfm/update/check", HTTP_POST, onCheck);
  server.on("/kevfm/update/install", HTTP_POST, onInstall);
  server.on("/kevfm/update/upload", HTTP_POST, onUploadDone, onUploadData);
}
