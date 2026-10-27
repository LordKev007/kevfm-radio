/*
 * KevFM gift radio plugin for yoRadio.
 *
 * Owns: software grounds (D18/D19/D22), amp SD (D21), sleep-timer button (D13),
 * status light (D2), knob press (light mode), power-button sleep, KevFM-only station,
 * always-play. See E:\KevinFM\dev\gusi_radio_handwired\firmware_spec.md
 *
 * Needs the small "KevFM patch" in src/core/controls.cpp (kevfm_btn_event / kevfm_on_turn hooks).
 */
#ifndef KEVFM_H
#define KEVFM_H

#include "../../pluginsManager/pluginsManager.h"

/*  Pins  */
#define KEVFM_PIN_SW_GND_SLEEPBTN  18   /* sleep button's ground  */
#define KEVFM_PIN_SW_GND_LED       19   /* LED cathode            */
#define KEVFM_PIN_SW_GND_ENCODER   22   /* KY-040 GND             */
#define KEVFM_PIN_AMP_SD           21   /* INPUT = on, OUTPUT LOW = off, never HIGH */
#define KEVFM_PIN_SLEEPBTN         13   /* to D18, internal pull-up */
#define KEVFM_PIN_LED               2   /* -> 220R -> LED -> D19   */
#define KEVFM_PIN_POWER             4   /* power/wake, real GND (yoRadio BTN_MODE / WAKE_PIN) */

/*  Things the radio says: a spoken clip from /clips/<name>.mp3 in its file storage, plus a
    blink pattern on the status light (also the fallback if a clip file is missing).  */
enum kevfmClip_e : uint8_t {
  CLIP_SLEEP_10, CLIP_SLEEP_15, CLIP_SLEEP_30, CLIP_SLEEP_60, CLIP_SLEEP_OFF,
  CLIP_TIME_LEFT,                   /* "About twenty-five minutes left": minutes passed separately, 0 = under a minute */
  CLIP_LIGHT_TOUCHED, CLIP_LIGHT_ON, CLIP_LIGHT_OFF,
  CLIP_GOODNIGHT, CLIP_WIFI_SETUP, CLIP_STATION_OFF
};

enum kevfmLight_e : uint8_t { LIGHT_TOUCHED = 0, LIGHT_ALWAYS = 1, LIGHT_OFF = 2 };

class KevFM : public Plugin {
public:
  KevFM();
  void registerNow();               /* called from initVariant(), see kevfm.cpp */
  void on_setup();
  void on_connect();
  void on_end_setup();
  void on_ticker();

  /*  Called from the main loop (ctrls_on_loop) and from the controls.cpp patch  */
  void loop();
  bool btnEvent(int id, uint8_t kind);
  bool clipBusy() const { return _clipPlaying; }   /* also answers the KevFM patch in audiohandlers.h */
  void onTurn();

  /*  Web page (kevfm_web.cpp). Requests arrive on the web server's task, so they only leave
      a request here; loop() carries it out on the main loop.  */
  volatile int8_t   webSleep = -1;     /* minutes 0/10/15/30/60, -1 = none */
  volatile int8_t   webLight = -1;     /* kevfmLight_e, -1 = none */
  volatile uint32_t restartAt = 0;     /* millis() to restart at (after saving Wi-Fi), 0 = none */
  uint8_t  lightMode() const { return _lightMode; }
  uint8_t  sleepSetting() const { return kSleepMinutes(_timerIdx); }
  uint32_t sleepSecondsLeft() const;
  static uint8_t kSleepMinutes(uint8_t idx);

private:
  /* sleep timer */
  uint8_t  _timerIdx = 0;            /* index into kSleepSteps, 0 = off */
  uint32_t _timerEndMs = 0;
  uint32_t _coalesceUntil = 0;       /* announce the setting when this passes */
  bool     _sbDown = false, _sbLongDone = false;
  uint32_t _sbChangeMs = 0, _sbDownMs = 0;
  bool     _sbRaw = false;
  /* light */
  kevfmLight_e _lightMode = LIGHT_TOUCHED;
  uint32_t _lastTouchMs = 0;
  float    _level = 0;               /* 0..1 current brightness */
  uint32_t _lastLedMs = 0;
  /* blink announcement (stand-in for spoken clips) */
  uint8_t  _blinkLeft = 0;
  bool     _blinkLong = false;
  uint32_t _blinkNextMs = 0;
  bool     _blinkOn = false;
  /* knob */
  uint32_t _lastTurnMs = 0;
  /* sleep / fade */
  bool     _fading = false;
  bool     _fadeCancelable = false;   /* timer fade: any touch keeps the radio on */
  uint32_t _fadeStartMs = 0, _fadeMs = 0;
  uint8_t  _fadeFromVol = 0;
  bool     _powerHeld = false;
  /* keep playing */
  uint16_t _stoppedSecs = 0;
  uint8_t  _failedTries = 0;           /* stream retries without a minute of playing */
  bool     _stationOffSaid = false;
  /* spoken clips */
  bool     _clipPlaying = false;
  bool     _clipResume = false;        /* start the stream again after the clip */
  bool     _clipSleepAfter = false;    /* "Goodnight": deep sleep after the clip */
  uint32_t _clipStartMs = 0;
  uint32_t _lastWifiSayMs = 0;
  uint16_t _apSecs = 0;

  void _touch();
  void _sleepBtnLoop();
  void _sleepBtnShort();
  void _sleepBtnLong();
  void _timerLoop();
  void _ledLoop();
  void _say(kevfmClip_e clip, uint16_t minutes = 0);
  bool _playClip(const char *name, bool sleepAfter = false);
  void _clipLoop();
  void _cycleLight();
  void _startFadeToSleep(uint32_t fadeMs, bool cancelable);
  void _cancelFade();
  void _fadeLoop();
  void _deepSleepNow();
  void _ensureStation();
  void _setSleep(uint8_t minutes);
  void _setLight(kevfmLight_e mode);
  void _webLoop();
  bool _timerRunning() { return _timerIdx != 0; }
};

extern KevFM kevfm;

/* kevfm_web.cpp */
void kevfmWebLoop();     /* captive DNS + first Wi-Fi scan while in hotspot mode */

#endif
