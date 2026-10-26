#ifndef KEVFM_OTA_H
#define KEVFM_OTA_H

#include "../../AsyncWebServer/ESPAsyncWebServer.h"

/* kevfm_ota.cpp: GitHub check/install and .bin upload (see the comment at its top) */
void kevfmOtaBegin(AsyncWebServer &server);
void kevfmOtaLoop();     /* main loop: runs a queued check/install, restarts when done */
bool kevfmOtaBusy();     /* checking, installing, receiving an upload, or about to restart */

#endif
