#ifndef KEVFM_STATION_H
#define KEVFM_STATION_H

/*  The one and only station this radio plays.
    The radio rewrites its station list to exactly this line at every boot, so a change here
    plus a re-flash is all it takes (e.g. when the domain name replaces the IP).  */

#define KEVFM_STATION_NAME   "The Mighty KevFM"

/*  Icecast on AWS Lightsail (Montreal), port 8000. Plain http (the ESP32 decodes MP3 over http
    with far less memory than https).  */
#ifndef KEVFM_STREAM_URL
  #define KEVFM_STREAM_URL   "http://15.175.80.104:8000/k874b8cb4ef7a9d88"
#endif

#endif
