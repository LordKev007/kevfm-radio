/*
 * KevFM gift radio plugin for yoRadio. See kevfm.h and README.md.
 */
#include "kevfm.h"
#include "kevfm_station.h"
#include "kevfm_ota.h"
#include "kevfm_version.h"
#include "../../core/options.h"
#include "../../core/config.h"
#include "../../core/player.h"
#include "../../core/network.h"
#include "../../core/controls.h"
#include <Preferences.h>
#include <SPIFFS.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_mac.h"

KevFM kevfm;

/* ---------------------------------------------------------------- tuning */
static const uint8_t  kSleepSteps[]       = {0, 10, 15, 30, 60};   /* minutes, 0 = off */
static const uint32_t KEVFM_COALESCE_MS   = 1000;  /* presses closer than this become one change  */
static const uint32_t KEVFM_LONGPRESS_MS  = 800;   /* sleep button: hold this long = "time left"  */
static const uint32_t KEVFM_DEBOUNCE_MS   = 30;
static const uint32_t KEVFM_TIMER_FADE_MS = 6000;  /* sleep timer reached zero: music fades down  */
static const uint32_t KEVFM_POWER_FADE_MS = 800;   /* power button held: quick fade               */
static const uint32_t KEVFM_TOUCH_GLOW_MS = 10000; /* "glow when touched": stays lit this long    */
static const uint32_t KEVFM_FADE_IN_MS    = 300;
static const uint32_t KEVFM_FADE_OUT_MS   = 1500;
static const uint32_t KEVFM_BREATH_MS     = 6000;  /* one breath while the sleep timer runs       */
static const float    KEVFM_LED_MAX       = 0.60f; /* brightest glow, fraction of full PWM        */
static const float    KEVFM_BREATH_MIN    = 0.08f;
static const uint32_t KEVFM_REPLAY_SECS   = 15;    /* stream stopped this long -> play again       */
static const uint8_t  KEVFM_LED_BITS      = 12;
static const uint32_t KEVFM_LED_FREQ      = 5000;

static const char* const kClipText[] = {
  "Sleep timer, ten minutes", "Sleep timer, fifteen minutes", "Sleep timer, thirty minutes",
  "Sleep timer, sixty minutes", "Sleep timer off", "minutes left",
  "Light: when touched", "Light on", "Light off"
};

static inline bool reached(uint32_t now, uint32_t when) { return (int32_t)(now - when) >= 0; }

/* ---------------------------------------------------------------- earliest boot
   Runs at the end of initArduino(), before setup() and before yoRadio reads any button.
   The software grounds must be LOW before the encoder / sleep button / LED are used.  */
extern "C" void initVariant(void) {
  gpio_deep_sleep_hold_dis();                     /* release the amp-off hold from deep sleep */
  gpio_hold_dis((gpio_num_t)KEVFM_PIN_AMP_SD);
  pinMode(KEVFM_PIN_SW_GND_SLEEPBTN, OUTPUT); digitalWrite(KEVFM_PIN_SW_GND_SLEEPBTN, LOW);
  pinMode(KEVFM_PIN_SW_GND_LED,      OUTPUT); digitalWrite(KEVFM_PIN_SW_GND_LED,      LOW);
  pinMode(KEVFM_PIN_SW_GND_ENCODER,  OUTPUT); digitalWrite(KEVFM_PIN_SW_GND_ENCODER,  LOW);
  pinMode(KEVFM_PIN_AMP_SD, INPUT);               /* amp on, (L+R)/2 via the board's divider */
  pinMode(KEVFM_PIN_LED, OUTPUT); digitalWrite(KEVFM_PIN_LED, LOW);
  pinMode(KEVFM_PIN_SLEEPBTN, INPUT_PULLUP);
  rtc_gpio_deinit((gpio_num_t)KEVFM_PIN_POWER);   /* wake pin back to a normal GPIO */
  /* yoRadio's OneButton objects set these pull-ups in their global constructors, before
     initArduino() resets the pins, so they are lost. Without them an idle power button reads
     as held. Set them again here. */
  pinMode(KEVFM_PIN_POWER, INPUT_PULLUP);
  pinMode(ENC_BTNB, INPUT_PULLUP);
  /* Register here, not in the constructor: global constructors run in link order, and if
     ours runs before pm's, pm's std::vector constructor wipes the registration. All global
     constructors have finished by the time initVariant() runs. */
  kevfm.registerNow();
}

/* ---------------------------------------------------------------- setup hotspot name */
const char *kevfm_ap_ssid() {
  static char name[20] = "";
  if (!name[0]) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(name, sizeof(name), "KevFM-Radio-%02X%02X", mac[4], mac[5]);
  }
  return name;
}

/* ---------------------------------------------------------------- hooks */
void ctrls_on_loop() { kevfm.loop(); }                                    /* controls.cpp */
bool kevfm_btn_event(int id, uint8_t kind) { return kevfm.btnEvent(id, kind); } /* KevFM patch */
void kevfm_on_turn(int8_t delta) { kevfm.onTurn(); }                      /* KevFM patch */

KevFM::KevFM() {}

void KevFM::registerNow() {
  for (size_t i = 0; i < pm.count(); i++) if (pm.get(i) == this) return;
  registerPlugin();
}

void KevFM::on_setup() {
  Preferences p;
  if (p.begin("kevfm", true)) {
    uint8_t m = p.getUChar("light", LIGHT_TOUCHED);
    _lightMode = m <= LIGHT_OFF ? (kevfmLight_e)m : LIGHT_TOUCHED;
    p.end();
  }
  ledcAttach(KEVFM_PIN_LED, KEVFM_LED_FREQ, KEVFM_LED_BITS);
  ledcWrite(KEVFM_PIN_LED, 0);
  Serial.printf("##[KevFM]#\tKevFM radio %s, light mode %d, station %s\n", KEVFM_VERSION, _lightMode, KEVFM_STREAM_URL);
  _touch();                                       /* glow at power-on, like the original GuSi */
}

void KevFM::on_connect() { _ensureStation(); }

void KevFM::on_end_setup() {
  /* KevFM always plays when awake. yoRadio already sent PLAY if smartstart==1. */
  if (network.status == CONNECTED && config.store.smartstart != 1) {
    player.sendCommand({PR_PLAY, 1});
  }
}

/* The radio has exactly one station. Rewrite the list at every boot if it differs, so the
   web page's playlist editor can't leave the radio on something else. */
void KevFM::_ensureStation() {
  if (config.getMode() != PM_WEB) return;
  char want[BUFLEN * 2];
  snprintf(want, sizeof(want), "%s\t%s\t0", KEVFM_STATION_NAME, KEVFM_STREAM_URL);
  String have;
  File f = SPIFFS.open(PLAYLIST_PATH, "r");
  if (f) { have = f.readString(); f.close(); }
  have.trim();
  if (have == want) return;
  f = SPIFFS.open(PLAYLIST_PATH, "w");
  if (!f) { Serial.println("##[KevFM]#\tcould not write playlist"); return; }
  f.println(want);
  f.close();
  SPIFFS.remove(INDEX_PATH);                       /* yoRadio re-indexes it in initPlaylist() */
  if (config.store.lastStation != 1) config.saveValue(&config.store.lastStation, (uint16_t)1);
  Serial.println("##[KevFM]#\tstation list set to KevFM");
}

/* 1 Hz, main loop (TimeKeeper::loop1) */
void KevFM::on_ticker() {
  /* keep playing: if the stream dropped (server restart, network blip), start it again */
  if (network.status == CONNECTED && !_fading && !kevfmOtaBusy() && player.status() == STOPPED) {
    if (++_stoppedSecs >= KEVFM_REPLAY_SECS) {
      _stoppedSecs = 0;
      Serial.println("##[KevFM]#\tstream stopped, playing again");
      player.sendCommand({PR_PLAY, 1});
    }
  } else {
    _stoppedSecs = 0;
  }
  /* the sleep timer must still put the radio to sleep if yoRadio has paused loopControls
     (e.g. while the network is lost) */
  if (_timerRunning() && reached(millis(), _timerEndMs + 2 * KEVFM_TIMER_FADE_MS)) {
    Serial.println("##[KevFM]#\tsleep timer overdue, sleeping now");
    _deepSleepNow();
  }
}

/* every main-loop pass (ctrls_on_loop) */
void KevFM::loop() {
  _webLoop();
  _sleepBtnLoop();
  _timerLoop();
  _fadeLoop();
  _ledLoop();
}

/* ---------------------------------------------------------------- touches */
void KevFM::_touch() {
  _lastTouchMs = millis();
}

void KevFM::onTurn() {
  _lastTurnMs = millis();
  _touch();
  if (_fading && _fadeCancelable) _cancelFade();          /* turning the knob cancels a timer fade */
}

/* Called by the KevFM patch in controls.cpp before yoRadio handles a button.
   Return true = KevFM handled it, yoRadio does nothing. */
bool KevFM::btnEvent(int id, uint8_t kind) {
  _touch();
  switch ((controlEvt_e)id) {
    case EVT_ENCBTNB:                               /* knob press: light mode (not play/pause) */
      if (kind == KEVFM_BTN_CLICK || kind == KEVFM_BTN_DOUBLE) {
        if (millis() - _lastTurnMs < 700) return true;   /* pressed while turning: ignore */
        _cycleLight();
        if (kind == KEVFM_BTN_DOUBLE) _cycleLight();
      }
      return true;
    case EVT_BTNMODE:                               /* power button */
      if (kind == KEVFM_BTN_LONG_START) {
        _powerHeld = true;
        Serial.println("##[KevFM]#\tpower button: going to sleep");
        _startFadeToSleep(KEVFM_POWER_FADE_MS, false);
      } else if (kind == KEVFM_BTN_LONG_STOP) {
        _powerHeld = false;                         /* _fadeLoop sleeps once released + faded */
      }
      return true;
    default:
      return false;
  }
}

/* ---------------------------------------------------------------- sleep timer button */
void KevFM::_sleepBtnLoop() {
  uint32_t now = millis();
  bool raw = digitalRead(KEVFM_PIN_SLEEPBTN) == LOW;
  if (raw != _sbRaw) { _sbRaw = raw; _sbChangeMs = now; }
  if (now - _sbChangeMs < KEVFM_DEBOUNCE_MS) return;
  if (raw && !_sbDown) {                            /* pressed */
    _sbDown = true; _sbLongDone = false; _sbDownMs = now;
    _touch();
  } else if (raw && _sbDown && !_sbLongDone && now - _sbDownMs >= KEVFM_LONGPRESS_MS) {
    _sbLongDone = true;
    _sleepBtnLong();
  } else if (!raw && _sbDown) {                     /* released */
    _sbDown = false;
    if (!_sbLongDone) _sleepBtnShort();
  }
}

void KevFM::_sleepBtnShort() {
  uint32_t now = millis();
  if (_fading && _fadeCancelable) _cancelFade();    /* a press during the timer fade keeps the radio on */
  _timerIdx = (_timerIdx + 1) % sizeof(kSleepSteps);
  if (_timerIdx) _timerEndMs = now + (uint32_t)kSleepSteps[_timerIdx] * 60000UL;
  _coalesceUntil = now + KEVFM_COALESCE_MS;          /* announce only the final setting */
  _blinkLeft = 0; _blinkOn = false;                  /* stop an older announcement */
  Serial.printf("##[KevFM]#\tsleep timer -> %u min\n", kSleepSteps[_timerIdx]);
}

void KevFM::_sleepBtnLong() {
  _coalesceUntil = 0;
  if (!_timerRunning()) { _say(CLIP_SLEEP_OFF); return; }
  uint32_t left = _timerEndMs - millis();
  if ((int32_t)left < 0) left = 0;
  _say(CLIP_TIME_LEFT, (uint16_t)((left + 59999UL) / 60000UL));
}

void KevFM::_timerLoop() {
  uint32_t now = millis();
  if (_coalesceUntil && reached(now, _coalesceUntil)) {
    _coalesceUntil = 0;
    static const kevfmClip_e clips[] = {CLIP_SLEEP_OFF, CLIP_SLEEP_10, CLIP_SLEEP_15, CLIP_SLEEP_30, CLIP_SLEEP_60};
    _say(clips[_timerIdx]);
  }
  if (_timerRunning() && !_fading && reached(now, _timerEndMs)) {
    _timerIdx = 0;
    Serial.println("##[KevFM]#\tsleep timer reached zero");
    _startFadeToSleep(KEVFM_TIMER_FADE_MS, true);
  }
}

/* ---------------------------------------------------------------- fade, amp off, deep sleep */
void KevFM::_startFadeToSleep(uint32_t fadeMs, bool cancelable) {
  if (_fading) {                                    /* power button during a timer fade: finish faster */
    _fadeCancelable = _fadeCancelable && cancelable;
    if (fadeMs < _fadeMs) _fadeMs = fadeMs;
    return;
  }
  _fading = true;
  _fadeCancelable = cancelable;
  _fadeStartMs = millis();
  _fadeMs = fadeMs;
  _fadeFromVol = player.getVolume();
}

void KevFM::_cancelFade() {
  _fading = false;
  player.setVolume(_fadeFromVol);
  Serial.println("##[KevFM]#\tsleep fade cancelled");
}

void KevFM::_fadeLoop() {
  if (!_fading) return;
  uint32_t el = millis() - _fadeStartMs;
  if (el < _fadeMs) {
    player.setVolume((uint8_t)((uint32_t)_fadeFromVol * (_fadeMs - el) / _fadeMs));  /* not saved */
    return;
  }
  player.setVolume(0);
  if (_powerHeld || digitalRead(KEVFM_PIN_POWER) == LOW) return;   /* wait for release, or it wakes at once */
  _deepSleepNow();
}

void KevFM::_deepSleepNow() {
  Serial.println("##[KevFM]#\tamp off, deep sleep (power button wakes)");
  player.setVolume(0);
  ledcWrite(KEVFM_PIN_LED, 0);
  /* amp SD LOW = shutdown, no hiss. GPIO21 is not an RTC pin, so hold it as a digital pad. */
  pinMode(KEVFM_PIN_AMP_SD, OUTPUT);
  digitalWrite(KEVFM_PIN_AMP_SD, LOW);
  gpio_hold_en((gpio_num_t)KEVFM_PIN_AMP_SD);
  gpio_deep_sleep_hold_en();
  /* wake on the power button (to real GND); keep its pull-up alive while asleep */
  esp_sleep_enable_ext0_wakeup((gpio_num_t)KEVFM_PIN_POWER, 0);
  rtc_gpio_pullup_en((gpio_num_t)KEVFM_PIN_POWER);
  rtc_gpio_pulldown_dis((gpio_num_t)KEVFM_PIN_POWER);
  Serial.flush();
  esp_deep_sleep_start();
}

/* ---------------------------------------------------------------- status light */
void KevFM::_cycleLight() {
  _setLight((kevfmLight_e)((_lightMode + 1) % 3));
}

void KevFM::_setLight(kevfmLight_e mode) {
  _lightMode = mode;
  Preferences p;
  if (p.begin("kevfm", false)) { p.putUChar("light", _lightMode); p.end(); }
  static const kevfmClip_e clips[] = {CLIP_LIGHT_TOUCHED, CLIP_LIGHT_ON, CLIP_LIGHT_OFF};
  _say(clips[_lightMode]);
}

/* ---------------------------------------------------------------- web page requests */
uint8_t KevFM::kSleepMinutes(uint8_t idx) { return idx < sizeof(kSleepSteps) ? kSleepSteps[idx] : 0; }

uint32_t KevFM::sleepSecondsLeft() const {
  if (!_timerIdx) return 0;
  int32_t left = (int32_t)(_timerEndMs - millis());
  return left > 0 ? (uint32_t)(left + 999) / 1000 : 0;
}

void KevFM::_setSleep(uint8_t minutes) {
  for (uint8_t i = 0; i < sizeof(kSleepSteps); i++) {
    if (kSleepSteps[i] != minutes) continue;
    if (_fading && _fadeCancelable) _cancelFade();
    _timerIdx = i;
    if (i) _timerEndMs = millis() + (uint32_t)minutes * 60000UL;
    _coalesceUntil = 0;
    static const kevfmClip_e clips[] = {CLIP_SLEEP_OFF, CLIP_SLEEP_10, CLIP_SLEEP_15, CLIP_SLEEP_30, CLIP_SLEEP_60};
    _say(clips[i]);                                  /* the radio confirms, as for the button */
    Serial.printf("##[KevFM]#\tsleep timer -> %u min (web)\n", minutes);
    return;
  }
}

void KevFM::_webLoop() {
  int8_t v = webSleep;
  if (v >= 0) { webSleep = -1; _setSleep((uint8_t)v); _touch(); }
  v = webLight;
  if (v >= 0 && v <= LIGHT_OFF) { webLight = -1; _setLight((kevfmLight_e)v); _touch(); }
  if (restartAt && reached(millis(), restartAt)) {
    Serial.println("##[KevFM]#\tWi-Fi saved, restarting");
    config.saveValue(&config.store.lastSSID, (uint8_t)1);   /* try the network just added first */
    delay(100);
    ESP.restart();
  }
  kevfmWebLoop();
  kevfmOtaLoop();
}

/* Spoken clips are a later phase (voice not chosen). Until then the light answers instead:
   1-4 short blinks = 10/15/30/60 min, one long blink = off; light mode: 1 blink = when touched,
   2 = always on, long = off; time left: one blink per started 10 minutes. */
void KevFM::_say(kevfmClip_e clip, uint16_t minutes) {
  if (clip == CLIP_TIME_LEFT) Serial.printf("##[KevFM]#\tsay: %u %s\n", minutes, kClipText[clip]);
  else Serial.printf("##[KevFM]#\tsay: %s\n", kClipText[clip]);
  uint8_t n = 1; bool lng = false;
  switch (clip) {
    case CLIP_SLEEP_10: n = 1; break;
    case CLIP_SLEEP_15: n = 2; break;
    case CLIP_SLEEP_30: n = 3; break;
    case CLIP_SLEEP_60: n = 4; break;
    case CLIP_TIME_LEFT: n = (minutes + 9) / 10; if (n < 1) n = 1; break;
    case CLIP_LIGHT_TOUCHED: n = 1; break;
    case CLIP_LIGHT_ON: n = 2; break;
    case CLIP_SLEEP_OFF:
    case CLIP_LIGHT_OFF: n = 1; lng = true; break;
  }
  _blinkLeft = n; _blinkLong = lng; _blinkOn = false;
  _blinkNextMs = millis() + 300;                    /* short dark gap so the blinks stand out */
}

void KevFM::_ledLoop() {
  uint32_t now = millis();
  uint32_t dt = now - _lastLedMs;
  if (dt < 5) return;
  _lastLedMs = now;
  float target;
  bool blinking = _blinkLeft > 0 || _blinkOn;
  if (blinking) {
    if (reached(now, _blinkNextMs)) {
      if (_blinkOn) { _blinkOn = false; _blinkLeft--; _blinkNextMs = now + 300; }
      else if (_blinkLeft) { _blinkOn = true; _blinkNextMs = now + (_blinkLong ? 1000 : 250); }
    }
    _level = _blinkOn ? 1.0f : 0.0f;
    if (!_blinkLeft && !_blinkOn) _lastTouchMs = now;  /* settle back from a fresh "touch" */
  } else {
    if (_lightMode == LIGHT_OFF) target = 0;
    else if (_timerRunning()) {
      float ph = (float)(now % KEVFM_BREATH_MS) / KEVFM_BREATH_MS;
      target = KEVFM_BREATH_MIN + (1.0f - KEVFM_BREATH_MIN) * (0.5f - 0.5f * cosf(2 * PI * ph));
    }
    else if (_lightMode == LIGHT_ALWAYS) target = 1;
    else target = (now - _lastTouchMs < KEVFM_TOUCH_GLOW_MS) ? 1 : 0;
    if (target > _level) { _level += (float)dt / KEVFM_FADE_IN_MS;  if (_level > target) _level = target; }
    else                 { _level -= (float)dt / KEVFM_FADE_OUT_MS; if (_level < target) _level = target; }
  }
  float g = _level * _level * KEVFM_LED_MAX;          /* rough gamma so fades look even */
  ledcWrite(KEVFM_PIN_LED, (uint32_t)(g * ((1 << KEVFM_LED_BITS) - 1)));
}
