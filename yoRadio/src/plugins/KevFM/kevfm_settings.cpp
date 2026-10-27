/*
 * KevFM radio Settings page endpoints: only the yoRadio settings that matter on a radio
 * with no screen, no remote and one station (see README, "Web pages").
 *
 *   GET  /kevfm/settings   {"name","volsteps","encacc","bass","middle","treble"}
 *   POST /kevfm/settings   any of: name, volsteps (1-10), encacc (0-700), bass/middle/treble (-16..16)
 *   POST /kevfm/restart    restart the radio
 *   POST /kevfm/factory    back to factory settings (keeps saved Wi-Fi), then restart
 *
 * Same stores as yoRadio's own settings commands (commandhandler.cpp); like yoRadio's web
 * socket handler, these run on the web server's task. Restarts wait for the main loop so the
 * answer reaches the phone first.
 */
#include "kevfm.h"
#include "kevfm_settings.h"
#include "../../core/options.h"
#include "../../core/config.h"
#include "../../core/controls.h"
#include <ESPmDNS.h>
#include <Preferences.h>
#include <EEPROM.h>
#include "esp_mac.h"

static volatile uint32_t restartAt = 0;
static volatile bool factoryPending = false;

/* "kevfm-e330": matches the setup hotspot KevFM-Radio-E330 */
static void defaultName(char *out, size_t len) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(out, len, "kevfm-%02x%02x", mac[4], mac[5]);
}

/* yoRadio's default network name is "yoradio-<chip id>": give the radio a KevFM one instead
   (called once connected, after yoRadio has started mDNS) */
void kevfmFixDefaultName() {
  if (strncmp(config.store.mdnsname, "yoradio", 7) != 0 && config.store.mdnsname[0]) return;
  char name[MDNS_LENGTH];
  defaultName(name, sizeof(name));
  config.saveValue(config.store.mdnsname, name, MDNS_LENGTH);
  MDNS.end();
  MDNS.begin(config.store.mdnsname);
  Serial.printf("##[KevFM]#\tnetwork name %s.local\n", config.store.mdnsname);
}

/* One line with every KevFM-relevant setting, for the USB log (at startup and after a change) */
static void printSettings(const char *why) {
  Serial.printf("##[KevFM]#\tsettings (%s): name %s.local, volume %u, volume step %u, knob speed-up %u, "
                "bass %+d, middle %+d, treble %+d, light mode %u\n",
                why, config.store.mdnsname, config.store.volume, config.store.volsteps, config.store.encacc,
                config.store.bass, config.store.middle, config.store.trebble, kevfm.lightMode());
}

static String param(AsyncWebServerRequest *request, const char *name) {
  if (request->hasParam(name, true)) return request->getParam(name, true)->value();
  if (request->hasParam(name)) return request->getParam(name)->value();
  return String();
}

static void sendJson(AsyncWebServerRequest *request, const String &json, int code = 200) {
  AsyncWebServerResponse *r = request->beginResponse(code, "application/json", json);
  r->addHeader("Cache-Control", "no-store");
  request->send(r);
}

static void onGet(AsyncWebServerRequest *request) {
  String out = "{\"name\":\"";
  out += config.store.mdnsname;                     /* letters, digits and '-' only */
  out += "\",\"volsteps\":"; out += config.store.volsteps;
  out += ",\"encacc\":"; out += config.store.encacc;
  out += ",\"bass\":"; out += config.store.bass;
  out += ",\"middle\":"; out += config.store.middle;
  out += ",\"treble\":"; out += config.store.trebble;
  out += '}';
  sendJson(request, out);
}

static bool inRange(const String &v, long lo, long hi, long &out) {
  if (!v.length()) return false;
  for (size_t i = 0; i < v.length(); i++) if (!(isDigit(v[i]) || (i == 0 && v[i] == '-'))) return false;
  out = v.toInt();
  return out >= lo && out <= hi;
}

static void onSet(AsyncWebServerRequest *request) {
  long n;
  String v = param(request, "name");
  if (v.length()) {
    v.trim();
    v.toLowerCase();
    bool ok = v.length() >= 1 && v.length() < MDNS_LENGTH && v[0] != '-' && v[v.length() - 1] != '-';
    for (size_t i = 0; ok && i < v.length(); i++) ok = isLowerCase(v[i]) || isDigit(v[i]) || v[i] == '-';
    if (!ok) { sendJson(request, "{\"ok\":false,\"error\":\"Use 1 to 23 letters, digits or dashes (not at the start or end).\"}", 400); return; }
    config.saveValue(config.store.mdnsname, v.c_str(), MDNS_LENGTH);
    MDNS.end();
    MDNS.begin(config.store.mdnsname);
    Serial.printf("##[KevFM]#\tnetwork name %s.local\n", config.store.mdnsname);
  }
  v = param(request, "volsteps");
  if (v.length()) {
    if (!inRange(v, 1, 10, n)) { sendJson(request, "{\"ok\":false,\"error\":\"Volume step must be 1 to 10.\"}", 400); return; }
    config.saveValue(&config.store.volsteps, (uint8_t)n);
  }
  v = param(request, "encacc");
  if (v.length()) {
    if (!inRange(v, 0, 700, n)) { sendJson(request, "{\"ok\":false,\"error\":\"Knob acceleration must be 0 to 700.\"}", 400); return; }
    setEncAcceleration((uint16_t)n);
  }
  String b = param(request, "bass"), m = param(request, "middle"), t = param(request, "treble");
  if (b.length() || m.length() || t.length()) {
    long bv = config.store.bass, mv = config.store.middle, tv = config.store.trebble;
    if ((b.length() && !inRange(b, -16, 16, bv)) || (m.length() && !inRange(m, -16, 16, mv)) || (t.length() && !inRange(t, -16, 16, tv))) {
      sendJson(request, "{\"ok\":false,\"error\":\"Tone settings must be -16 to 16.\"}", 400); return;
    }
    config.setTone((int8_t)bv, (int8_t)mv, (int8_t)tv);
    /* yoRadio's setTone() writes bass/middle without committing and relies on the treble save
       to commit, but skips that save when treble is unchanged: bass/middle-only changes were
       lost at the next restart. Commit here. */
    EEPROM.commit();
  }
  printSettings("changed on the Settings page");
  sendJson(request, "{\"ok\":true}");
}

static void onRestart(AsyncWebServerRequest *request) {
  restartAt = millis() + 1500;
  sendJson(request, "{\"ok\":true}");
}

static void onFactory(AsyncWebServerRequest *request) {
  factoryPending = true;
  restartAt = millis() + 1500;
  sendJson(request, "{\"ok\":true}");
}

/* main loop */
void kevfmSettingsLoop() {
  static bool printed = false;
  if (!printed) { printed = true; printSettings("startup"); }
  if (!restartAt || (int32_t)(millis() - restartAt) < 0) return;
  if (factoryPending) {
    Serial.println("##[KevFM]#\tfactory settings (Wi-Fi kept), restarting");
    Preferences p;
    if (p.begin("kevfm", false)) { p.clear(); p.end(); }   /* light mode back to "when touched" */
    config.reset();                                         /* yoRadio settings to defaults, restarts */
  }
  Serial.println("##[KevFM]#\trestarting (settings page)");
  delay(100);
  ESP.restart();
}

void kevfmSettingsBegin(AsyncWebServer &server) {
  server.on("/kevfm/settings", HTTP_GET, onGet);
  server.on("/kevfm/settings", HTTP_POST, onSet);
  server.on("/kevfm/restart", HTTP_POST, onRestart);
  server.on("/kevfm/factory", HTTP_POST, onFactory);
}
