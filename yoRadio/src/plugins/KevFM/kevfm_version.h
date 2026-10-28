#ifndef KEVFM_VERSION_H
#define KEVFM_VERSION_H

/*  KevFM radio firmware version. Bump it for every release published to GitHub: the radio
    offers an update when update.json's "version" is higher than this.  */
#define KEVFM_VERSION        "0.4.3"

/*  Where "Check for update" looks: the public GitHub repo, read as plain raw files
    (the same simple pattern as the Bubs stock ticker). update.json, the .bin and the
    web files are all fetched from here.  */
#ifndef KEVFM_UPDATE_BASE
  #define KEVFM_UPDATE_BASE  "https://raw.githubusercontent.com/LordKev007/kevfm-radio/main/"
#endif
#define KEVFM_UPDATE_MANIFEST  KEVFM_UPDATE_BASE "update.json"
#define KEVFM_UPDATE_WEBDIR    "yoRadio/data/www/"   /* web files' folder in the repo */

#endif
