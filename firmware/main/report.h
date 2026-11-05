#pragma once
// Frame health reports (fw 14). The frame writes one line an hour to a PRIVATE GitHub repo
// (state.json "reports".repo, file log/YYYY-MM-DD.jsonl): firmware, uptime, Wi-Fi, what is on
// screen, sync/OTA status, and an hour of presence + distance statistics for remote tuning.
//
// Linking (once, at the frame): Diagnostics -> Reports -> Get code. The frame asks GitHub for a
// device code (OAuth device flow of a GitHub App whose client_id is in state.json), shows it, and
// waits while Tony enters it at github.com/login/device on his phone. The token GitHub then hands
// over is kept in NVS on the frame only; it never touches the public repo. The App should have
// only "Contents: read & write", be installed only on the reports repo, and have user-token
// expiry turned off (the frame cannot refresh tokens without a secret).
//
// state.json:
//   "reports": {
//     "enabled": true,            kill switch (false = nothing is sent; linking still possible)
//     "client_id": "Iv23...",     the GitHub App's client ID (public, not a secret)
//     "repo": "owner/name",       private repo the App is installed on
//     "every_min": 60             minutes between reports (10 .. 1440)
//   }
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    REP_NOT_SET_UP = 0,   // no client_id / repo in state.json yet
    REP_UNLINKED,         // ready to link
    REP_ASKING,           // asking GitHub for a code
    REP_CODE,             // showing the code, waiting for approval
    REP_LINKED,           // has a token
    REP_FAILED,           // linking failed (msg says why)
} rep_state_t;

typedef struct {
    rep_state_t state;
    char code[16];        // e.g. WDJB-MJHT
    char uri[64];         // e.g. https://github.com/login/device
    int expires_s;        // seconds left on the code
    char msg[96];         // last result, for the screen
} rep_info_t;

void report_start(void);          // after sync_start()
void report_get(rep_info_t *out);
void report_link(void);           // Diagnostics: Get code
void report_cancel(void);         // stop waiting for approval
void report_unlink(void);         // forget the token
void report_now(void);            // send a report now (if linked)
bool report_busy(void);           // linking in progress: keep the diagnostics screen open
