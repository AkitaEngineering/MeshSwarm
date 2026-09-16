#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "failsafe.h"
#include "mavlink_min.h"
#include "mesh_codec.h"

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

static failsafe_input_t base_input() {
    failsafe_input_t in = {};
    in.armed_known = false;
    in.armed = false;
    in.gcs_ever_seen = false;
    in.gps_valid = true;
    in.now_us = 40 * 1000000LL;
    in.boot_us = 0;
    in.last_gcs_us = 0;
    in.last_gps_us = 1 * 1000000LL;
    in.battery_v = 12.4f;
    in.geofence_radius_m = 0;
    in.geofence_distance_m = 0;
    in.low_battery_v = 11.0f;
    in.lost_link_timeout_ms = 30000;
    in.gps_loss_timeout_ms = 15000;
    return in;
}

static void test_seq() {
    CHECK(seq_is_newer(2, 1));
    CHECK(!seq_is_newer(1, 1));
    CHECK(!seq_is_newer(1, 2));
    CHECK(seq_is_newer(1, 0xF0000001u));
    CHECK(!seq_is_newer(0x10000001u, 0xF0000001u));
}

static void test_gps_and_distance() {
    CHECK(!gps_fix_valid(0, 0));
    CHECK(gps_fix_valid(1, 0));
    CHECK(gps_fix_valid(0, 1));
    CHECK(gps_is_fresh(2 * 1000000LL, 1 * 1000000LL, 15000));
    CHECK(!gps_is_fresh(20 * 1000000LL, 1 * 1000000LL, 15000));
    CHECK(!gps_is_fresh(1000, 0, 15000));

    float meters = haversine_m(0.0f, 0.0f, 1.0f, 0.0f);
    CHECK(meters > 110000.0f && meters < 112000.0f);
}

static void test_failsafe_geofence() {
    failsafe_input_t in = base_input();
    in.last_gps_us = in.now_us;
    in.geofence_radius_m = 50.0f;
    in.geofence_distance_m = 80.0f;
    failsafe_reason_t reason = FAILSAFE_REASON_NONE;
    CHECK(failsafe_evaluate(&in, &reason) == FAILSAFE_ACTION_RTL);
    CHECK(reason == FAILSAFE_REASON_GEOFENCE);

    in.geofence_distance_m = 10.0f;
    CHECK(failsafe_evaluate(&in, &reason) == FAILSAFE_ACTION_NONE);
}

static void test_failsafe_battery() {
    failsafe_input_t in = base_input();
    in.battery_v = 10.5f;
    failsafe_reason_t reason = FAILSAFE_REASON_NONE;
    CHECK(failsafe_evaluate(&in, &reason) == FAILSAFE_ACTION_RTL);
    CHECK(reason == FAILSAFE_REASON_LOW_BATTERY);
}

static void test_failsafe_lost_link() {
    failsafe_input_t in = base_input();
    in.gcs_ever_seen = true;
    in.last_gcs_us = 1 * 1000000LL;
    in.now_us = 40 * 1000000LL;
    failsafe_reason_t reason = FAILSAFE_REASON_NONE;
    CHECK(failsafe_evaluate(&in, &reason) == FAILSAFE_ACTION_RTL);
    CHECK(reason == FAILSAFE_REASON_LOST_LINK);

    in.now_us = 2 * 1000000LL;
    CHECK(failsafe_evaluate(&in, &reason) == FAILSAFE_ACTION_NONE);
}

static void test_failsafe_no_gcs_requires_armed() {
    failsafe_input_t in = base_input();
    in.now_us = 40 * 1000000LL;
    failsafe_reason_t reason = FAILSAFE_REASON_NONE;
    CHECK(failsafe_evaluate(&in, &reason) == FAILSAFE_ACTION_NONE);

    in.armed_known = true;
    in.armed = false;
    in.last_gps_us = 0;
    CHECK(failsafe_evaluate(&in, &reason) == FAILSAFE_ACTION_NONE);

    in.armed = true;
    CHECK(failsafe_evaluate(&in, &reason) == FAILSAFE_ACTION_RTL);
    CHECK(reason == FAILSAFE_REASON_NO_GCS);
}

static void test_failsafe_gps_loss() {
    failsafe_input_t in = base_input();
    in.armed_known = true;
    in.armed = true;
    in.gcs_ever_seen = true;
    in.last_gcs_us = in.now_us;
    in.last_gps_us = 1 * 1000000LL;
    in.now_us = 20 * 1000000LL;
    in.gps_valid = false;
    failsafe_reason_t reason = FAILSAFE_REASON_NONE;
    CHECK(failsafe_evaluate(&in, &reason) == FAILSAFE_ACTION_RTL);
    CHECK(reason == FAILSAFE_REASON_GPS_LOSS);

    in.last_gps_us = 0;
    CHECK(failsafe_evaluate(&in, &reason) == FAILSAFE_ACTION_NONE);
}

static void test_mesh_frame() {
    const uint8_t payload[] = {'s', 'w', 'a', 'r', 'm'};
    uint8_t frame[64];
    size_t n = mesh_encode_frame(MESH_MSG_CONTROL, payload, sizeof(payload), frame, sizeof(frame));
    CHECK(n == 5 + sizeof(payload) + 2);

    uint8_t type = 0;
    const uint8_t* inner = NULL;
    size_t inner_len = 0;
    size_t frame_len = 0;
    CHECK(mesh_decode_frame(frame, n, &type, &inner, &inner_len, &frame_len));
    CHECK(type == MESH_MSG_CONTROL);
    CHECK(inner_len == sizeof(payload));
    CHECK(memcmp(inner, payload, sizeof(payload)) == 0);

    frame[n - 1] ^= 0x01;
    CHECK(!mesh_decode_frame(frame, n, &type, &inner, &inner_len, &frame_len));
}

static void test_mavlink_roundtrip() {
    mavlink_min_init(255, 1);
    uint8_t buf[64];
    size_t n = mavlink_min_pack_heartbeat(buf, sizeof(buf));
    CHECK(n > 0);

    mavlink_min_message_t msg = {};
    bool parsed = false;
    for (size_t i = 0; i < n; i++) {
        if (mavlink_min_parse_byte(buf[i], &msg)) {
            parsed = true;
        }
    }
    CHECK(parsed);
    CHECK(msg.msgid == MAVLINK_MSG_ID_HEARTBEAT);
    CHECK(msg.sysid == 255);

    n = mavlink_min_pack_command_long(buf, sizeof(buf), 1, 1, MAV_CMD_NAV_RETURN_TO_LAUNCH, 0);
    CHECK(n > 0);
    parsed = false;
    for (size_t i = 0; i < n; i++) {
        if (mavlink_min_parse_byte(buf[i], &msg)) {
            parsed = true;
        }
    }
    CHECK(parsed);
    CHECK(msg.msgid == MAVLINK_MSG_ID_COMMAND_LONG);
}

int main() {
    test_seq();
    test_gps_and_distance();
    test_failsafe_geofence();
    test_failsafe_battery();
    test_failsafe_lost_link();
    test_failsafe_no_gcs_requires_armed();
    test_failsafe_gps_loss();
    test_mesh_frame();
    test_mavlink_roundtrip();

    if (g_failures) {
        std::fprintf(stderr, "%d host firmware checks failed\n", g_failures);
        return 1;
    }
    std::puts("host firmware tests passed");
    return 0;
}
