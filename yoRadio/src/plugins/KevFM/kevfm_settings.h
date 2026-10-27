#ifndef KEVFM_SETTINGS_H
#define KEVFM_SETTINGS_H

#include "../../AsyncWebServer/ESPAsyncWebServer.h"

/* kevfm_settings.cpp: the KevFM Settings page's endpoints */
void kevfmSettingsBegin(AsyncWebServer &server);
void kevfmSettingsLoop();      /* main loop: restart / factory reset once the answer is sent */
void kevfmFixDefaultName();    /* yoradio-xxxx -> kevfm-xxxx network name, once connected */

#endif
