#include "failsafe.h"

#include <math.h>

bool seq_is_newer(uint32_t seq, uint32_t last) {
    if (seq > last) {
        return true;
    }
    return last > 0xF0000000u && seq < 0x10000000u;
}

float haversine_m(float lat1, float lon1, float lat2, float lon2) {
    const float r = 6371000.0f;
    const float deg2rad = 0.01745329252f;
    float p1 = lat1 * deg2rad;
    float p2 = lat2 * deg2rad;
    float dphi = (lat2 - lat1) * deg2rad;
    float dlmb = (lon2 - lon1) * deg2rad;
    float a = sinf(dphi / 2) * sinf(dphi / 2) +
              cosf(p1) * cosf(p2) * sinf(dlmb / 2) * sinf(dlmb / 2);
    if (a > 1.0f) {
        a = 1.0f;
    }
    return 2.0f * r * asinf(sqrtf(a));
}

bool gps_fix_valid(int32_t lat_e7, int32_t lon_e7) {
    return lat_e7 != 0 || lon_e7 != 0;
}

bool gps_is_fresh(int64_t now_us, int64_t last_gps_us, uint32_t timeout_ms) {
    if (last_gps_us <= 0) {
        return false;
    }
    return (now_us - last_gps_us) <= ((int64_t)timeout_ms * 1000);
}

failsafe_action_t failsafe_evaluate(const failsafe_input_t* in, failsafe_reason_t* reason) {
    if (reason) {
        *reason = FAILSAFE_REASON_NONE;
    }
    if (in == NULL) {
        return FAILSAFE_ACTION_NONE;
    }

    bool gps_ok = in->gps_valid &&
                  gps_is_fresh(in->now_us, in->last_gps_us, in->gps_loss_timeout_ms);

    if (in->geofence_radius_m > 0.0f && gps_ok &&
        in->geofence_distance_m > in->geofence_radius_m) {
        if (reason) {
            *reason = FAILSAFE_REASON_GEOFENCE;
        }
        return FAILSAFE_ACTION_RTL;
    }

    if (in->battery_v > 0.5f && in->battery_v < in->low_battery_v) {
        if (reason) {
            *reason = FAILSAFE_REASON_LOW_BATTERY;
        }
        return FAILSAFE_ACTION_RTL;
    }

    if (in->armed_known && in->armed && in->last_gps_us > 0 &&
        !gps_is_fresh(in->now_us, in->last_gps_us, in->gps_loss_timeout_ms)) {
        if (reason) {
            *reason = FAILSAFE_REASON_GPS_LOSS;
        }
        return FAILSAFE_ACTION_RTL;
    }

    if (in->armed_known && in->armed && !in->gcs_ever_seen &&
        (in->now_us - in->boot_us) > ((int64_t)in->lost_link_timeout_ms * 1000)) {
        if (reason) {
            *reason = FAILSAFE_REASON_NO_GCS;
        }
        return FAILSAFE_ACTION_RTL;
    }

    if (in->gcs_ever_seen &&
        (in->now_us - in->last_gcs_us) > ((int64_t)in->lost_link_timeout_ms * 1000)) {
        if (reason) {
            *reason = FAILSAFE_REASON_LOST_LINK;
        }
        return FAILSAFE_ACTION_RTL;
    }

    return FAILSAFE_ACTION_NONE;
}
