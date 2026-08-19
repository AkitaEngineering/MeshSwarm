#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define MAVLINK_STX_V1                 0xFE
#define MAVLINK_STX_V2                 0xFD
#define MAVLINK_MSG_ID_HEARTBEAT       0
#define MAVLINK_MSG_ID_SYS_STATUS      1
#define MAVLINK_MSG_ID_ATTITUDE        30
#define MAVLINK_MSG_ID_GLOBAL_POSITION_INT 33
#define MAVLINK_MSG_ID_COMMAND_LONG    76
#define MAVLINK_MSG_ID_COMMAND_ACK     77
#define MAV_CMD_NAV_RETURN_TO_LAUNCH   20
#define MAV_CMD_NAV_LAND               21
#define MAV_TYPE_GCS                   6
#define MAV_AUTOPILOT_INVALID          8
#define MAV_STATE_ACTIVE               4

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t msgid;
    uint8_t sysid;
    uint8_t compid;
    uint8_t payload[64];
    uint8_t payload_len;
} mavlink_min_message_t;

void mavlink_min_init(uint8_t sysid, uint8_t compid);
size_t mavlink_min_pack_heartbeat(uint8_t* out, size_t out_len);
size_t mavlink_min_pack_command_long(uint8_t* out, size_t out_len, uint8_t target_sys,
                                     uint8_t target_comp, uint16_t command,
                                     uint8_t confirmation);
bool mavlink_min_parse_byte(uint8_t byte, mavlink_min_message_t* msg);

#ifdef __cplusplus
}
#endif
