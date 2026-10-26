#ifndef myoptions_h
#define myoptions_h

/*******************************************************
  KevFM gift radio - yoRadio options
  Board: ELEGOO EL-SM-011 ESP32 (30-pin, classic ESP32, no PSRAM)
  Pins match E:\KevinFM\dev\gusi_radio_handwired\layout2.py and firmware_spec.md
********************************************************/

#define L10N_LANGUAGE     EN
#define DSP_MODEL         DSP_DUMMY      /* headless: no display */

/*  I2S -> MAX98357A  */
#define I2S_DOUT          27             /* DIN  */
#define I2S_BCLK          26             /* BCLK */
#define I2S_LRC           25             /* LRC  */

/*  KY-040 encoder. The module has its own pull-ups on CLK/DT; its GND is D22 (software ground).
    The push switch (D14) has no pull-up on the module, so the internal pull-up stays on. */
#define ENC_BTNL          32             /* CLK */
#define ENC_BTNR          33             /* DT  */
#define ENC_BTNB          14             /* knob press -> light mode (KevFM plugin) */
#define ENC_INTERNALPULLUP true

/*  Power / wake button (right MX) to the real GND pin. Long press = sleep, press = wake.  */
#define BTN_MODE          4
#define WAKE_PIN          4
#define BTN_INTERNALPULLUP true

/*  The sleep-timer button (D13), the status LED (D2) and the software grounds
    (D18, D19, D22) are driven by the KevFM plugin, not by yoRadio.  */

/*  Amp SD (D21) is also owned by the plugin: INPUT = amp on with (L+R)/2 mix, OUTPUT LOW = off.
    Never drive it HIGH (3.3 V selects left-only). So no MUTE_PIN here.  */

/*  yoRadio's own LED blinking stays off: D2 belongs to the KevFM status light.  */
#ifdef LED_BUILTIN
  #undef LED_BUILTIN
#endif
#define LED_BUILTIN       255

/*  Mono speaker: mix both channels in software too, so nothing is lost if SD ever ends up
    in a one-channel mode.  */
#define PLAYER_FORCE_MONO true

/*  Setup hotspot shown when the radio has no Wi-Fi yet  */
/*  "KevFM-Radio-4634": the last 4 hex digits of the board's MAC, so two radios in setup
    mode at once can be told apart (kevfm.cpp builds it)  */
const char *kevfm_ap_ssid();
#define apSsid            kevfm_ap_ssid()
#define apPassword        "kevfmradio"

#endif
