#include "mesh_codec.h"

#include <string.h>

uint16_t mesh_crc16_ccitt(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

size_t mesh_encode_frame(uint8_t msg_type, const uint8_t* payload, size_t payload_len,
                         uint8_t* out, size_t out_len) {
    size_t total = 5 + payload_len + 2;
    if (out == NULL || payload == NULL || out_len < total || payload_len > 0xFFFF) {
        return 0;
    }
    out[0] = MESH_MAGIC0;
    out[1] = MESH_MAGIC1;
    out[2] = msg_type;
    out[3] = (uint8_t)(payload_len & 0xFF);
    out[4] = (uint8_t)((payload_len >> 8) & 0xFF);
    memcpy(out + 5, payload, payload_len);
    uint16_t crc = mesh_crc16_ccitt(out, 5 + payload_len);
    out[5 + payload_len] = (uint8_t)(crc & 0xFF);
    out[6 + payload_len] = (uint8_t)((crc >> 8) & 0xFF);
    return total;
}

bool mesh_decode_frame(const uint8_t* data, size_t len, uint8_t* msg_type,
                       const uint8_t** payload, size_t* payload_len, size_t* frame_len) {
    if (data == NULL || len < 7 || data[0] != MESH_MAGIC0 || data[1] != MESH_MAGIC1) {
        return false;
    }
    size_t plen = (size_t)data[3] | ((size_t)data[4] << 8);
    size_t total = 5 + plen + 2;
    if (len < total) {
        return false;
    }
    uint16_t crc_got = (uint16_t)data[5 + plen] | ((uint16_t)data[6 + plen] << 8);
    if (crc_got != mesh_crc16_ccitt(data, 5 + plen)) {
        return false;
    }
    if (msg_type) {
        *msg_type = data[2];
    }
    if (payload) {
        *payload = data + 5;
    }
    if (payload_len) {
        *payload_len = plen;
    }
    if (frame_len) {
        *frame_len = total;
    }
    return true;
}
