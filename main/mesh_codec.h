#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define MESH_MAGIC0              0xDB
#define MESH_MAGIC1              0x32
#define MESH_MSG_TELEMETRY       1
#define MESH_MSG_CONTROL         2
#define MESH_MSG_ACK             3
#define MESH_PORT_SERIAL_APP     64
#define MESH_PORT_PRIVATE_APP    256
#define MESH_START1              0x94
#define MESH_START2              0xC3
#define MESH_MAX_PB_LEN          512
#define MESH_MAX_FRAME_LEN       256
#define MESH_RX_BUF_LEN          1024

#ifdef __cplusplus
extern "C" {
#endif

uint16_t mesh_crc16_ccitt(const uint8_t* data, size_t len);
size_t mesh_encode_frame(uint8_t msg_type, const uint8_t* payload, size_t payload_len,
                         uint8_t* out, size_t out_len);
bool mesh_decode_frame(const uint8_t* data, size_t len, uint8_t* msg_type,
                       const uint8_t** payload, size_t* payload_len, size_t* frame_len);

bool mesh_encode_toradio(uint32_t portnum, const uint8_t* payload, size_t payload_len,
                         bool want_ack, uint8_t priority, uint32_t packet_id,
                         uint8_t* out, size_t out_len, size_t* written);
bool mesh_encode_want_config(uint32_t nonce, uint8_t* out, size_t out_len, size_t* written);
bool mesh_encode_heartbeat(uint32_t nonce, uint8_t* out, size_t out_len, size_t* written);
bool mesh_decode_fromradio(const uint8_t* pb, size_t pb_len, uint32_t* portnum,
                           const uint8_t** payload, size_t* payload_len);

void mesh_link_init(void);
bool mesh_link_send(uint32_t portnum, const uint8_t* payload, size_t payload_len,
                    bool want_ack, uint8_t priority);
bool mesh_link_write_raw(const uint8_t* data, size_t len);
void mesh_link_poll(void (*on_payload)(uint32_t portnum, const uint8_t* payload, size_t len));

#ifdef __cplusplus
}
#endif
