# KevFM radio firmware

Firmware for the hand-built KevFM gift radios (GuSi BM2K case remix, see
`E:\KevinFM\dev\gusi_radio_handwired`). It is a thin fork of
[yoRadio](https://github.com/e2002/yoradio) at commit `2fd3e38` (v0.9.720), built headless
(`DSP_DUMMY`), plus one plugin and a small, clearly marked patch.

Behaviour spec: `E:\KevinFM\dev\gusi_radio_handwired\firmware_spec.md`.

## Folder

| Path | What |
|---|---|
| `yoRadio/` | the sketch (yoRadio 2fd3e38 + KevFM changes). Open `yoRadio/yoRadio.ino` |
| `yoRadio/myoptions.h` | pins and options for the ELEGOO ESP32 |
| `yoRadio/src/plugins/KevFM/` | the KevFM plugin: sleep timer, status light, power button, KevFM-only, always-play |
| `yoRadio/src/plugins/KevFM/kevfm_station.h` | **the stream URL** |
| `yoRadio/src/plugins/KevFM/kevfm_web.cpp` | KevFM web pages' endpoints, Wi-Fi scan, captive portal |
| `web/kevfm.html`, `setup.html`, `firmware.html` | KevFM pages (listen.html look). Built INTO the firmware, gzipped, by `tools/embed_web.py` -> `kevfm_pages.h` |
| `yoRadio/data/www/kevfm-cover-256.jpg` | cover picture (in the radio's file storage) |
| `yoRadio/partitions.csv` | KevFM flash layout (from 0.3.0): 2 x 1.75 MB app, 384 KB file storage |
| `yoRadio/data/www/theme.css` | yoRadio's old pages: station list / editor / prev-next hidden |
| `patches/kevfm-yoradio.patch` | every change to upstream yoRadio files, as one diff |
| `upstream_yoradio/` | untouched upstream clone at 2fd3e38, for diffing / future merges |
| `build.cmd`, `flash.cmd` | build and flash (Windows) |
| `tools/build.sh`, `tools/make_spiffs.sh` | same build from Git Bash |

## Hardware (ELEGOO EL-SM-011: classic ESP32, 4 MB flash, no PSRAM, CP2102, USB-C)

| Function | Pin | Notes |
|---|---|---|
| I2S BCLK / LRC / DIN | D26 / D25 / D27 | MAX98357A |
| Amp SD | D21 | INPUT = on, (L+R)/2. OUTPUT LOW = off before sleep (held LOW while asleep). Never HIGH |
| Encoder CLK / DT / push | D32 / D33 / D14 | KY-040, GND on D22 |
| Sleep-timer button | D13 | to D18 |
| Power / wake button | D4 | to real GND, `BTN_MODE` + `WAKE_PIN` |
| Status LED | D2 | 220 Ω, cathode on D19 |
| Software grounds | D18, D19, D22 | driven OUTPUT LOW in `initVariant()`, before `setup()` |

## What it does now

- **Boot**: software grounds LOW first thing, amp on, light glows (in "when touched" mode), joins
  Wi-Fi, plays KevFM. It always plays when awake: if the stream drops it tries again every 15 s.
- **One station**: at every boot the station list is rewritten to exactly the line in
  `kevfm_station.h`, so nothing on the web page can leave the radio on another station.
  The web page hides the station list, editor and prev/next.
- **Knob**: turn = volume (also while pressed; upstream would change station). Press = light mode
  (glow when touched → always on → off), saved in flash. Upstream play/pause is disabled.
- **Sleep-timer button**: each press steps off → 10 → 15 → 30 → 60 → off; the first press already
  changes it; presses less than 1 s apart count as one change and only the final setting is
  announced. Hold (0.8 s): time left. At zero: music fades over 6 s, amp SD LOW, deep sleep.
  Turning the knob or pressing the sleep button during that fade keeps the radio on.
  (yoRadio's `sleepForAfter` is not used: its uint8 seconds overflow and it wakes on a timer.)
- **Power button** (0.4.1): a tap = quick fade, amp off, deep sleep (Kevin: "Dad will never think
  about holding it"); holding works too. A tap wakes it. Ignored for 4 s after start-up so the
  waking tap can't switch it straight back off.
- **Knob direction** (0.4.1): yoRadio's encoder pins are given as L = D33, R = D32 so clockwise = louder.
- **Status light**: breathes slowly while the sleep timer runs (modes "touched" and "always on").
- **Announcements**: the spoken clips are a later phase. Until then the light answers:
  - sleep timer: 1 / 2 / 3 / 4 short blinks = 10 / 15 / 30 / 60 min, one long blink = off
  - time left (hold): one blink per started 10 minutes (22 min = 3 blinks), long blink = off
  - light mode: 1 blink = when touched, 2 blinks = always on, long blink = off
  - every announcement is also printed on the serial monitor, e.g. `##[KevFM]# say: Light on`
- **Setup hotspot** (no Wi-Fi saved yet, or none in reach): `KevFM-Radio-XXXX` (last 4 hex
  digits of the board's MAC, e.g. `KevFM-Radio-4634`; built 2026-10-25), password
  `kevfmradio`. Joining it pops the setup page up on a phone (captive portal); otherwise
  http://192.168.4.1. The page lists the networks it can see (strongest first, signal bars,
  lock icon) and has a "type its name" option. Up to 5 networks are kept, newest tried first.
  yoRadio limits names to 29 characters and passwords to 39 (the page checks).

## Web pages

| Address | Page |
|---|---|
| `/` | KevFM player: cover, now playing, volume, sleep timer, light mode, Wi-Fi signal (setup page while in hotspot mode) |
| `/settings` | KevFM Settings: radio name (`<name>.local`), knob step and speed-up, bass/middle/treble, restart, factory settings (keeps Wi-Fi) |
| `/setup` | Wi-Fi setup: scan list, type-it-in, saved networks with Forget |
| `/firmware` | version, Check for update (GitHub), Install, or upload a `.bin` file |
| `/settings.html` | yoRadio's own settings pages: not linked from any KevFM page (almost nothing there applies to a radio with no screen, remote or station list); type the address if ever needed |
| `GET /kevfm/status` | JSON: station, title, playing, volume, sleep, left, light, ssid, rssi, ip |
| `POST /kevfm/set` | `volume=0..254`, `sleep=0/10/15/30/60`, `light=0..2` |
| `GET /kevfm/scan` | nearby networks (`?refresh=1` rescans) |
| `GET /kevfm/networks`, `POST /kevfm/wifi`, `POST /kevfm/forget` | saved network names (never passwords), add (restarts), remove |
| `POST /kevfm/update/check`, `/install`, `/upload`; `GET /kevfm/update/status` | firmware updates (`kevfm_ota.cpp`) |

The KevFM pages are compiled into the firmware (edit `web/*.html`; `build.cmd` runs
`tools/embed_web.py`), so they always match the firmware and updates never write them to
the file storage. (0.2.x kept them in storage: 128 KB, effectively full at ~78% because
SPIFFS keeps blocks in reserve; an OTA page write failed with errno 28. 0.2.2+ deletes those
old copies at boot.) The KevFM pages are sent with `Cache-Control: no-cache`. yoRadio's old `/` page was sent with
a one-year cache, so a phone that saw it once may show that stale copy: tap reload once.
Updating only the pages without erasing Wi-Fi: upload the changed files from `yoRadio/data/www`
through yoRadio's uploader (http://<radio-ip>/webboard).

## Firmware updates (like the Bubs stock ticker)

Nothing updates by itself. On the radio's page: **Firmware** (footer link, or `/firmware`):

- **Check for update** reads `update.json` from the public repo
  `LordKev007/kevfm-radio` (raw.githubusercontent.com) and compares its version with the
  radio's (`KEVFM_VERSION`). If newer, **Install** stops the music, downloads the web files
  listed in `update.json` and the `.bin`, writes them, and restarts. Saved Wi-Fi, volume and
  light mode stay. If anything fails, nothing is replaced and the music comes back.
- **Upload a firmware file**: pick a `.bin` (e.g. `kevfm-radio.bin`) on the phone/computer
  when GitHub can't be reached. Firmware only (no web files).
- Not protected by a password (the ticker scrolls one on its display; the radio has none).
  Anyone on the radio's home Wi-Fi could open it, same as yoRadio's own `/update`.
- No automatic rollback: a release that can't get online needs a USB flash. Install every
  release on one of your own radios before pushing it.

### Publishing an update

1. Bump `KEVFM_VERSION` in `yoRadio/src/plugins/KevFM/kevfm_version.h`.
2. `build.cmd`
3. `python tools\make_release.py` (copies the build to `kevfm-radio.bin`, writes `update.json`)
4. Test on your own radio (USB `flash.cmd COMx app`, or the Upload button).
5. Commit and push. Radios see it the next time someone presses Check for update.

The repo is this folder (`.gitignore` leaves out `build/`, `upstream_yoradio/`, logs). It is
public, so `kevfm_station.h` publishes the stream URL (already visible in listen.html) and the
setup hotspot password. yoRadio is GPL-3 (`LICENSE`), so publishing the source is right anyway.

## Build

Needs Arduino IDE 2.x installed (for its `arduino-cli`) and the **esp32 core 3.3.12**
(Boards Manager, "esp32 by Espressif"). No extra libraries: yoRadio carries its own.

1. The stream URL is in `yoRadio/src/plugins/KevFM/kevfm_station.h` (`KEVFM_STREAM_URL`); change it there when the domain name arrives.
2. Double-click `build.cmd` (or run it in a terminal). Output goes to `build\`:
   `yoRadio.ino.bin`, bootloader, partitions, and `spiffs.bin` (web page files).

Board settings (already in `build.cmd`; use the same if you build in the IDE):

| Setting | Value |
|---|---|
| Board | ESP32 Dev Module |
| Partition Scheme | from 0.3.0 `yoRadio/partitions.csv` is used automatically (2 x 1.75 MB app, 384 KB SPIFFS at 0x390000); the menu setting is ignored |
| PSRAM | Disabled |
| Flash Size / Mode / Freq | 4MB / QIO / 80MHz |
| CPU Frequency | 240MHz |
| Upload Speed | 921600 |
| Core Debug Level | None |

## Flash over USB-C

1. Plug the radio's ESP32 into the PC with a data USB-C cable. Device Manager → Ports shows
   "Silicon Labs CP210x USB to UART Bridge (COMx)". (No port? Install the CP210x driver.)
2. First time on a board: `flash.cmd COMx` (firmware + web files; this also clears saved Wi-Fi).
   Later updates: `flash.cmd COMx app` (firmware only; keeps Wi-Fi, volume, light mode).
3. If it says "Failed to connect": hold **BOOT**, run it again, let go when "Connecting..." shows.
   EN/BOOT are only for this; nobody needs them in normal life.
4. Serial monitor (Arduino IDE, 115200 baud) shows the boot log and the `##[KevFM]#` lines.
5. Join the `KevFM-Radio-XXXX` hotspot from a phone, open http://192.168.4.1, enter the Wi-Fi.
   It reboots, joins, and starts playing KevFM. Set the volume with the knob (it is saved
   3 s after the last turn).

Alternative without `flash.cmd`: in Arduino IDE 2 open `yoRadio/yoRadio.ino`, pick the
settings above and Upload. The IDE can't upload the web files, so on first boot the radio
shows yoRadio's "Web Board Uploader": upload the 11 files from `yoRadio/data/www` there.

After the first install, updates can also go over Wi-Fi: http://<radio-ip>/update with
`build\yoRadio.ino.bin` (OTA works because of the Minimal SPIFFS partition scheme).

## Status (2026-10-25)

**Compiles** with arduino-cli 1.5.1 + esp32 core 3.3.12, ESP32 Dev Module, Minimal SPIFFS:
app 1,516,934 bytes (v0.2.1) (74 % of 1.9 MB), static RAM 70,476 bytes (21 %). `spiffs.bin` built (11 web
files, ~29 KB). Linked symbols checked: `initVariant` (early software grounds), the KevFM hooks
and the plugin are in the image.

**On a bare board (2026-10-25, Kevin's ELEGOO, COM3, no buttons/LED/amp wired):** flashed with
`flash.cmd`, boots, mounts the web files, starts the `KevFM-Radio` hotspot, KevFM plugin starts.
Two bugs found and fixed on the board: the plugin was never registered (global-constructor
order, now registered from `initVariant()`), and D4/D14 lost their pull-ups (OneButton sets them
before `initArduino()` resets the pins), so an unwired power button read as held and started a
sleep at boot.

**KevFM web pages (2026-10-25, on the board, Kevin's iPhone):** setup page in hotspot mode
saved Hyrule, radio restarted, joined and played; player page "looks much better". Not yet
tried: captive pop-up on its own (unconfirmed whether it popped up or was opened by address),
Forget, the type-it-in form, volume/sleep/light buttons from the page.
Note: Kevin's PC (192.168.4.x) can't reach Hyrule's 192.168.5.x addresses; the iPhone can.
The radio's setup hotspot also uses 192.168.4.1, the same as Kevin's router (only matters
while the radio is in hotspot mode).

**Boards flashed:** both on 0.3.0 with the new 384 KB layout (full USB flash 2026-10-26).
#1 MAC 68:09:47:5f:e3:30, hotspot `KevFM-Radio-E330`; #2 MAC 68:09:47:5f:46:34, hotspot
`KevFM-Radio-4634`. Wi-Fi to be re-entered on each after the flash.

**Flash layout 0.3.0 (2026-10-25 night):** `partitions.csv` built and checked (app0/app1 0x1C0000,
spiffs 0x390000 + 0x60000, coredump 0x3F0000). Both boards still have the OLD layout until a
full USB flash (`flash.cmd COMx`, erases saved Wi-Fi, keeps volume/light). 0.3.0 also runs on
the old layout, so OTA from 0.2.0 to 0.3.0 works before the USB step.

**OTA test 2026-10-25 (board 2, 0.2.0 -> 0.2.1):** Check worked over https while streaming
(largest free block 55-61 KB). Install failed safely saving kevfm.html to storage (errno 28,
87,850 of 113,201 bytes used): nothing changed, music resumed. Fixed by moving the pages into
the firmware (0.2.2) and the bigger file area (0.3.0). Firmware download/write itself not yet
proven. Also seen: the KevFM stream returned 404 late at night (station off); the radio retried
every 15 s, now backs off 15 s -> 30 s -> 1 min -> 2 min (0.3.0).

**Firmware updates (0.2.x):** builds; board 2 flashed with 0.2.0 by USB, repo prepared at
0.2.1. Not yet tested: Check / Install from GitHub (repo not created yet), Upload a file, and
whether https fits in memory on this PSRAM-less board while it streams (check pauses the
stream if the largest free block is under 45 KB; the serial log prints it).

**Not tested on hardware yet:**
- Wi-Fi join, setup hotspot, web page, station rewrite, always-play (can be tested now, no amp needed)
- software grounds, buttons, encoder, light modes / breathing / blink answers (can be tested now)
- deep sleep + wake on D4, and that the amp SD hold keeps D21 LOW while asleep (GPIO21 is not an
  RTC pin; uses `gpio_hold_en` + `gpio_deep_sleep_hold_en`) - check with a meter on D21 while asleep
- anything audible (fade, volume, mono mix): amps arrive Oct 28
- `KEVFM_STREAM_URL` is set to the Lightsail Icecast mount (from `E:\KevinFM\stream_test\aws_listen_url.txt`, 2026-10-25). The stream is MP3 128k 44.1 kHz stereo (KevNUC `station.liq`). Not yet heard on a radio

Known yoRadio defaults worth knowing: first-boot volume is low (12 of 254), so turn it up and
let it save; the power button's hold time is yoRadio's 500 ms long-press.

## Settings page (0.4.0, 2026-10-26)

`web/settings.html` + `kevfm_settings.cpp`. Radio name defaults to `kevfm-xxxx` (matches the
hotspot) instead of yoRadio's `yoradio-<chipid>`. Every change and every startup prints one
`##[KevFM]# settings (...)` line on the USB log (name, volume, knob, tone, light).
yoRadio bug found and worked around: `Config::setTone()` writes bass/middle without committing
and skips the treble save (the one that commits) when treble is unchanged, so bass/middle-only
changes were lost at restart; `kevfm_settings.cpp` commits after setTone. Tested on board 1:
bass +1 survived a restart; Restart button works.

## Spoken clips (made 2026-10-26, approved by Kevin; played by the radio from 0.4.0)

Clip player (0.4.0, `kevfm.cpp` `_playClip` / `_clipLoop`, plus a marked patch in
`core/audiohandlers.h` so the end of a clip doesn't trigger yoRadio's "next track"): stops the
stream, plays `/clips/<name>.mp3` from storage, then reconnects the stream. Tested on board 1
2026-10-26 (log only, no amp yet): wifi_setup in hotspot mode ended after 2.9 s; from the phone
page light_on 1.5 s, light_off 1.2 s, sleep_60 2.0 s, sleep_off 1.6 s, each followed by the
stream reconnecting by itself. Not yet tried: hearing them (amp), time-left (sleep button),
goodnight before sleep (timer running out), station_off (stream down ~45 s).

24 clips in `clips/*.mp3` (196 KB: mono 32 kbps / 22 kHz, -16 LUFS target, peaks <= -1.5 dB),
lines in `clips/lines.tsv`, chosen takes in `clips/choices.json`. Voice: Kevin's ElevenLabs
"Computer Voice" sample (`clips_work/reference.mp3`, reading the test script word for word),
cloned with local Qwen3-TTS 0.6B-Base (same "steady" settings as the station's calm lines).

Remake or add a line: add it to `clips_work/lines.tsv`, then
`E:\KevinFM\LocalTTS\qwen-env\Scripts\python.exe -X utf8 tools\render_clips.py --takes 3`
and `tools\finish_clips.py` (picks a take that ends cleanly, is a normal length and is
closest in pitch to the reference; `clips_work/choices.json` overrides), then listen on
`clips/index.html`. Lessons: Qwen cuts about half of short takes off mid-sound, so takes are
checked for a clean ending; a fixed -45 dBFS silence trim cut the final "s" of "minutes",
so trimming is relative to each take's peak.

## Later phases (not built yet)

1. **Spoken clips player** (clips are made, see above). ~2 s MP3 clips for the sleep
   timer, time left and light modes. Design: `KevFM::_say()` is already the single place every
   announcement goes through; the clip player needs a player patch (stop the stream, play the
   clip from flash with `connecttoFS`, catch end-of-file, restart the stream). Flash room:
   the 128 KB SPIFFS holds the web files (~29 KB), so clips either go there at low bitrate
   (≈8 KB per 2 s clip at 32 kbps, ~12 clips) or into the app partition as PROGMEM arrays.
2. **Per-radio name + city** in the setup page, sent to Icecast as `?radio=<name>` and
   `User-Agent: KevFM-Radio (<name>)`. KevFM never names listeners on air; the weather may use
   their city.
3. **UofT / eduroam (WPA2-Enterprise PEAP)** for Bubs, reusing the stock-ticker code
   (`C:\Users\Kev\Downloads\BubsStockTicker_v4_0_46_DotC`). Watch the Arduino-core version (the
   ticker's PEAP CA bug and TLS-in-PSRAM fix) and the heap: this board has no PSRAM.
4. https stream once the domain name exists (costs heap on a PSRAM-less board; check first).
