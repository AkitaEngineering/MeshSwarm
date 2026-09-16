#include "mavlink_min.h"

#include <string.h>

static uint8_t s_sysid = 255;
static uint8_t s_compid = 1;
static uint8_t s_seq;

enum {
    ST_IDLE = 0,
    ST_LEN,
    ST_INCOMPAT,
    ST_COMPAT,
    ST_SEQ,
    ST_SYS,
    ST_COMP,
    ST_MSGID0,
    ST_MSGID1,
    ST_MSGID2,
    ST_PAYLOAD,
    ST_CRC1,
    ST_CRC2,
};

static uint8_t s_state;
static uint8_t s_len;
static uint8_t s_incompat;
static uint8_t s_compat;
static uint8_t s_seq_rx;
static uint8_t s_sys;
static uint8_t s_comp;
static uint32_t s_msgid;
static uint8_t s_msgid_bytes;
static uint8_t s_payload[64];
static uint8_t s_payload_i;
static uint16_t s_crc;
static bool s_mav2;

static uint8_t crc_extra_for(uint32_t msgid) {
    switch (msgid) {
        case MAVLINK_MSG_ID_HEARTBEAT:
            return 50;
        case MAVLINK_MSG_ID_SYS_STATUS:
            return 124;
        case MAVLINK_MSG_ID_ATTITUDE:
            return 39;
        case MAVLINK_MSG_ID_GLOBAL_POSITION_INT:
            return 104;
        case MAVLINK_MSG_ID_COMMAND_LONG:
            return 152;
        case MAVLINK_MSG_ID_COMMAND_ACK:
            return 143;
        default:
            return 0;
    }
}

static void crc_init(uint16_t* crc) {
    *crc = 0xFFFF;
}

static void crc_accumulate(uint16_t* crc, uint8_t data) {
    uint8_t tmp = data ^ (uint8_t)(*crc & 0xFF);
    tmp ^= (uint8_t)(tmp << 4);
    *crc = (uint16_t)((*crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4));
}

void mavlink_min_init(uint8_t sysid, uint8_t compid) {
    s_sysid = sysid;
    s_compid = compid;
    s_seq = 0;
    s_state = ST_IDLE;
}

static size_t pack_v2(uint8_t* out, size_t out_len, uint32_t msgid, const uint8_t* payload,
                      uint8_t payload_len) {
    size_t total = 10 + payload_len + 2;
    if (out_len < total) {
        return 0;
    }
    out[0] = MAVLINK_STX_V2;
    out[1] = payload_len;
    out[2] = 0;
    out[3] = 0;
    out[4] = s_seq++;
    out[5] = s_sysid;
    out[6] = s_compid;
    out[7] = (uint8_t)(msgid & 0xFF);
    out[8] = (uint8_t)((msgid >> 8) & 0xFF);
    out[9] = (uint8_t)((msgid >> 16) & 0xFF);
    if (payload_len) {
        memcpy(out + 10, payload, payload_len);
    }
    uint16_t crc;
    crc_init(&crc);
    size_t crc_end = 10 + (size_t)payload_len;
    for (size_t i = 1; i < crc_end; i++) {
        crc_accumulate(&crc, out[i]);
    }
    crc_accumulate(&crc, crc_extra_for(msgid));
    out[10 + payload_len] = (uint8_t)(crc & 0xFF);
    out[11 + payload_len] = (uint8_t)(crc >> 8);
    return total;
}

size_t mavlink_min_pack_heartbeat(uint8_t* out, size_t out_len) {
    uint8_t payload[9] = {0};
    payload[4] = MAV_TYPE_GCS;
    payload[5] = MAV_AUTOPILOT_INVALID;
    payload[6] = 0;
    payload[7] = MAV_STATE_ACTIVE;
    payload[8] = 3;
    return pack_v2(out, out_len, MAVLINK_MSG_ID_HEARTBEAT, payload, sizeof(payload));
}

size_t mavlink_min_pack_command_long(uint8_t* out, size_t out_len, uint8_t target_sys,
                                     uint8_t target_comp, uint16_t command,
                                     uint8_t confirmation) {
    uint8_t payload[33] = {0};
    payload[28] = (uint8_t)(command & 0xFF);
    payload[29] = (uint8_t)(command >> 8);
    payload[30] = target_sys;
    payload[31] = target_comp;
    payload[32] = confirmation;
    return pack_v2(out, out_len, MAVLINK_MSG_ID_COMMAND_LONG, payload, sizeof(payload));
}

bool mavlink_min_parse_byte(uint8_t byte, mavlink_min_message_t* msg) {
    switch (s_state) {
        case ST_IDLE:
            if (byte == MAVLINK_STX_V2) {
                s_mav2 = true;
                s_state = ST_LEN;
            } else if (byte == MAVLINK_STX_V1) {
                s_mav2 = false;
                s_state = ST_LEN;
            }
            return false;
        case ST_LEN:
            s_len = byte;
            if (s_len > sizeof(s_payload)) {
                s_state = ST_IDLE;
                return false;
            }
            s_state = s_mav2 ? ST_INCOMPAT : ST_SEQ;
            return false;
        case ST_INCOMPAT:
            s_incompat = byte;
            (void)s_incompat;
            s_state = ST_COMPAT;
            return false;
        case ST_COMPAT:
            s_compat = byte;
            s_state = ST_SEQ;
            return false;
        case ST_SEQ:
            s_seq_rx = byte;
            s_state = ST_SYS;
            return false;
        case ST_SYS:
            s_sys = byte;
            s_state = ST_COMP;
            return false;
        case ST_COMP:
            s_comp = byte;
            s_msgid = 0;
            s_msgid_bytes = 0;
            s_state = ST_MSGID0;
            return false;
        case ST_MSGID0:
            s_msgid = byte;
            s_state = s_mav2 ? ST_MSGID1 : (s_len ? ST_PAYLOAD : ST_CRC1);
            s_payload_i = 0;
            return false;
        case ST_MSGID1:
            s_msgid |= (uint32_t)byte << 8;
            s_state = ST_MSGID2;
            return false;
        case ST_MSGID2:
            s_msgid |= (uint32_t)byte << 16;
            s_payload_i = 0;
            s_state = s_len ? ST_PAYLOAD : ST_CRC1;
            return false;
        case ST_PAYLOAD:
            s_payload[s_payload_i++] = byte;
            if (s_payload_i >= s_len) {
                s_state = ST_CRC1;
            }
            return false;
        case ST_CRC1:
            s_crc = byte;
            s_state = ST_CRC2;
            return false;
        case ST_CRC2: {
            s_crc |= (uint16_t)byte << 8;
            uint16_t calc;
            crc_init(&calc);
            crc_accumulate(&calc, s_len);
            if (s_mav2) {
                crc_accumulate(&calc, s_incompat);
                crc_accumulate(&calc, s_compat);
            }
            crc_accumulate(&calc, s_seq_rx);
            crc_accumulate(&calc, s_sys);
            crc_accumulate(&calc, s_comp);
            crc_accumulate(&calc, (uint8_t)(s_msgid & 0xFF));
            if (s_mav2) {
                crc_accumulate(&calc, (uint8_t)((s_msgid >> 8) & 0xFF));
                crc_accumulate(&calc, (uint8_t)((s_msgid >> 16) & 0xFF));
            }
            for (uint8_t i = 0; i < s_len; i++) {
                crc_accumulate(&calc, s_payload[i]);
            }
            crc_accumulate(&calc, crc_extra_for(s_msgid));
            s_state = ST_IDLE;
            if (calc != s_crc) {
                return false;
            }
            if (msg == NULL) {
                return true;
            }
            msg->msgid = s_msgid;
            msg->sysid = s_sys;
            msg->compid = s_comp;
            msg->payload_len = s_len;
            memcpy(msg->payload, s_payload, s_len);
            return true;
        }
        default:
            s_state = ST_IDLE;
            return false;
    }
}
