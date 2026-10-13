#pragma once
// Wi-Fi (stage 2): saved network, background reconnect, scans and trial
// connections for the setup screen, internet time (SNTP, US Eastern).
//
// Setup policy (docs/Provisioning.md):
//   - no saved network                         -> setup wanted
//   - saved network missing for WIFI_MISSING_S -> setup wanted
//   - "Not now"                                -> asks again after WIFI_SNOOZE_S (1 h), until connected
#include <stdbool.h>
#include <stdint.h>

#define WIFI_MISSING_S   3600        // 1 hour
#define WIFI_SNOOZE_S    3600        // 1 hour: keeps asking until a network connects
#define WIFI_MAX_APS     24

typedef enum {
    WIFI_ST_NO_CREDS = 0,   // nothing saved
    WIFI_ST_CONNECTING,     // trying the saved network
    WIFI_ST_CONNECTED,      // has an IP
    WIFI_ST_WAITING,        // saved network missing, will retry
} wifi_state_t;

typedef enum {
    WIFI_TRY_NONE = 0,
    WIFI_TRY_BUSY,
    WIFI_TRY_OK,
    WIFI_TRY_FAIL,
} wifi_try_t;

typedef struct {
    wifi_state_t state;
    bool has_creds;
    char ssid[33];          // saved network
    int rssi;
    char ip[16];
    bool time_ok;           // clock set from SNTP at least once
    uint32_t missing_s;     // seconds the saved network has been missing (0 when connected)
    wifi_try_t try_state;   // result of the latest wifi_try()
    char try_msg[48];       // why a try failed
    bool scanning;
    uint32_t scan_gen;      // bumps when new scan results are ready
} wifi_status_t;

typedef struct {
    char ssid[33];
    int rssi;
    bool secure;
} wifi_ap_t;

void wifi_start(void);                         // after nvs_flash_init()
void wifi_get_status(wifi_status_t *out);
void wifi_scan(void);                          // async; results via wifi_scan_results()
int  wifi_scan_results(wifi_ap_t *out, int max);
void wifi_try(const char *ssid, const char *pass);   // async; saved only if it connects
bool wifi_setup_wanted(void);
void wifi_snooze(void);
