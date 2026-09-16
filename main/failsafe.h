#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FAILSAFE_ACTION_NONE = 0,
    FAILSAFE_ACTION_RTL = 1,
} failsafe_action_t;

typedef enum {
    FAILSAFE_REASON_NONE = 0,
    FAILSAFE_REASON_GEOFENCE = 1,
    FAILSAFE_REASON_LOW_BATTERY = 2,
    FAILSAFE_REASON_LOST_LINK = 3,
    FAILSAFE_REASON_NO_GCS = 4,
    FAILSAFE_REASON_GPS_LOSS = 5,
} failsafe_reason_t;

typedef struct {
    bool armed_known;
    bool armed;
    bool gcs_ever_seen;
    bool gps_valid;
    int64_t now_us;
    int64_t boot_us;
    int64_t last_gcs_us;
    int64_t last_gps_us;
    float battery_v;
    float geofence_radius_m;
    float geofence_distance_m;
    float low_battery_v;
    uint32_t lost_link_timeout_ms;
    uint32_t gps_loss_timeout_ms;
} failsafe_input_t;

bool seq_is_newer(uint32_t seq, uint32_t last);
float haversine_m(float lat1, float lon1, float lat2, float lon2);
bool gps_fix_valid(int32_t lat_e7, int32_t lon_e7);
bool gps_is_fresh(int64_t now_us, int64_t last_gps_us, uint32_t timeout_ms);
failsafe_action_t failsafe_evaluate(const failsafe_input_t* in, failsafe_reason_t* reason);

#ifdef __cplusplus
}
#endif
