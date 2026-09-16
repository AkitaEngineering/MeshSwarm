#include "mesh_codec.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifndef MESHTASTIC_UART_NUM
#define MESHTASTIC_UART_NUM UART_NUM_1
#endif

static SemaphoreHandle_t s_tx_lock;
static uint8_t s_rx[MESH_RX_BUF_LEN];
static size_t s_rx_len;

static size_t pb_put_varint(uint8_t* out, size_t max, uint32_t value) {
    size_t n = 0;
    while (value >= 0x80) {
        if (n >= max) {
            return 0;
        }
        out[n++] = (uint8_t)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    if (n >= max) {
        return 0;
    }
    out[n++] = (uint8_t)value;
    return n;
}

static bool pb_get_varint(const uint8_t* buf, size_t len, size_t* offset, uint32_t* value) {
    uint32_t result = 0;
    uint32_t shift = 0;
    while (*offset < len && shift <= 28) {
        uint8_t byte = buf[(*offset)++];
        result |= (uint32_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

static bool pb_skip(const uint8_t* buf, size_t len, size_t* offset, uint32_t wire) {
    if (wire == 0) {
        uint32_t dummy;
        return pb_get_varint(buf, len, offset, &dummy);
    }
    if (wire == 1) {
        if (*offset + 8 > len) {
            return false;
        }
        *offset += 8;
        return true;
    }
    if (wire == 2) {
        uint32_t slen = 0;
        if (!pb_get_varint(buf, len, offset, &slen)) {
            return false;
        }
        if (*offset + slen > len) {
            return false;
        }
        *offset += slen;
        return true;
    }
    if (wire == 5) {
        if (*offset + 4 > len) {
            return false;
        }
        *offset += 4;
        return true;
    }
    return false;
}

static size_t wrap_serial(const uint8_t* pb, size_t pb_len, uint8_t* out, size_t out_len) {
    if (pb_len > MESH_MAX_PB_LEN || out_len < pb_len + 4) {
        return 0;
    }
    out[0] = MESH_START1;
    out[1] = MESH_START2;
    out[2] = (uint8_t)((pb_len >> 8) & 0xFF);
    out[3] = (uint8_t)(pb_len & 0xFF);
    memcpy(out + 4, pb, pb_len);
    return pb_len + 4;
}

bool mesh_encode_toradio(uint32_t portnum, const uint8_t* payload, size_t payload_len,
                         bool want_ack, uint8_t priority, uint32_t packet_id,
                         uint8_t* out, size_t out_len, size_t* written) {
    uint8_t data_msg[8 + MESH_MAX_FRAME_LEN];
    size_t d = 0;
    data_msg[d++] = 0x08;  // Data.portnum field 1
    size_t n = pb_put_varint(data_msg + d, sizeof(data_msg) - d, portnum);
    if (n == 0) {
        return false;
    }
    d += n;
    data_msg[d++] = 0x12;  // Data.payload field 2
    n = pb_put_varint(data_msg + d, sizeof(data_msg) - d, (uint32_t)payload_len);
    if (n == 0 || d + n + payload_len > sizeof(data_msg)) {
        return false;
    }
    d += n;
    memcpy(data_msg + d, payload, payload_len);
    d += payload_len;

    uint8_t mesh[32 + sizeof(data_msg)];
    size_t m = 0;
    mesh[m++] = 0x15;  // MeshPacket.to fixed32 field 2 = broadcast
    mesh[m++] = 0xFF;
    mesh[m++] = 0xFF;
    mesh[m++] = 0xFF;
    mesh[m++] = 0xFF;
    mesh[m++] = 0x22;  // MeshPacket.decoded field 4
    n = pb_put_varint(mesh + m, sizeof(mesh) - m, (uint32_t)d);
    if (n == 0 || m + n + d > sizeof(mesh)) {
        return false;
    }
    m += n;
    memcpy(mesh + m, data_msg, d);
    m += d;
    mesh[m++] = 0x35;  // MeshPacket.id fixed32 field 6
    mesh[m++] = (uint8_t)(packet_id);
    mesh[m++] = (uint8_t)(packet_id >> 8);
    mesh[m++] = (uint8_t)(packet_id >> 16);
    mesh[m++] = (uint8_t)(packet_id >> 24);
    mesh[m++] = 0x48;  // hop_limit = 3
    mesh[m++] = 0x03;
    if (want_ack) {
        mesh[m++] = 0x50;
        mesh[m++] = 0x01;
    }
    if (priority) {
        mesh[m++] = 0x58;
        n = pb_put_varint(mesh + m, sizeof(mesh) - m, priority);
        if (n == 0) {
            return false;
        }
        m += n;
    }

    uint8_t toradio[8 + sizeof(mesh)];
    size_t t = 0;
    toradio[t++] = 0x0A;  // ToRadio.packet field 1
    n = pb_put_varint(toradio + t, sizeof(toradio) - t, (uint32_t)m);
    if (n == 0 || t + n + m > sizeof(toradio)) {
        return false;
    }
    t += n;
    memcpy(toradio + t, mesh, m);
    t += m;

    size_t wrapped = wrap_serial(toradio, t, out, out_len);
    if (wrapped == 0) {
        return false;
    }
    if (written) {
        *written = wrapped;
    }
    return true;
}

bool mesh_encode_want_config(uint32_t nonce, uint8_t* out, size_t out_len, size_t* written) {
    uint8_t pb[8];
    size_t t = 0;
    pb[t++] = 0x18;  // ToRadio.want_config_id field 3
    size_t n = pb_put_varint(pb + t, sizeof(pb) - t, nonce);
    if (n == 0) {
        return false;
    }
    t += n;
    size_t wrapped = wrap_serial(pb, t, out, out_len);
    if (wrapped == 0) {
        return false;
    }
    if (written) {
        *written = wrapped;
    }
    return true;
}

bool mesh_encode_heartbeat(uint32_t nonce, uint8_t* out, size_t out_len, size_t* written) {
    uint8_t hb[8];
    size_t h = 0;
    hb[h++] = 0x08;
    size_t n = pb_put_varint(hb + h, sizeof(hb) - h, nonce);
    if (n == 0) {
        return false;
    }
    h += n;
    uint8_t pb[16];
    size_t t = 0;
    pb[t++] = 0x3A;  // ToRadio.heartbeat field 7
    n = pb_put_varint(pb + t, sizeof(pb) - t, (uint32_t)h);
    if (n == 0 || t + n + h > sizeof(pb)) {
        return false;
    }
    t += n;
    memcpy(pb + t, hb, h);
    t += h;
    size_t wrapped = wrap_serial(pb, t, out, out_len);
    if (wrapped == 0) {
        return false;
    }
    if (written) {
        *written = wrapped;
    }
    return true;
}

static bool decode_data_msg(const uint8_t* buf, size_t len, uint32_t* portnum,
                            const uint8_t** payload, size_t* payload_len) {
    size_t offset = 0;
    bool have_payload = false;
    uint32_t port = 0;
    while (offset < len) {
        uint32_t tag = 0;
        if (!pb_get_varint(buf, len, &offset, &tag)) {
            return false;
        }
        uint32_t field = tag >> 3;
        uint32_t wire = tag & 7;
        if (field == 1 && wire == 0) {
            if (!pb_get_varint(buf, len, &offset, &port)) {
                return false;
            }
        } else if (field == 2 && wire == 2) {
            uint32_t slen = 0;
            if (!pb_get_varint(buf, len, &offset, &slen) || offset + slen > len) {
                return false;
            }
            *payload = buf + offset;
            *payload_len = slen;
            offset += slen;
            have_payload = true;
        } else if (!pb_skip(buf, len, &offset, wire)) {
            return false;
        }
    }
    if (portnum) {
        *portnum = port;
    }
    return have_payload;
}

static bool decode_mesh_packet(const uint8_t* buf, size_t len, uint32_t* portnum,
                               const uint8_t** payload, size_t* payload_len) {
    size_t offset = 0;
    while (offset < len) {
        uint32_t tag = 0;
        if (!pb_get_varint(buf, len, &offset, &tag)) {
            return false;
        }
        uint32_t field = tag >> 3;
        uint32_t wire = tag & 7;
        if (field == 4 && wire == 2) {
            uint32_t slen = 0;
            if (!pb_get_varint(buf, len, &offset, &slen) || offset + slen > len) {
                return false;
            }
            bool ok = decode_data_msg(buf + offset, slen, portnum, payload, payload_len);
            offset += slen;
            if (ok) {
                return true;
            }
        } else if (!pb_skip(buf, len, &offset, wire)) {
            return false;
        }
    }
    return false;
}

bool mesh_decode_fromradio(const uint8_t* pb, size_t pb_len, uint32_t* portnum,
                           const uint8_t** payload, size_t* payload_len) {
    size_t offset = 0;
    while (offset < pb_len) {
        uint32_t tag = 0;
        if (!pb_get_varint(pb, pb_len, &offset, &tag)) {
            return false;
        }
        uint32_t field = tag >> 3;
        uint32_t wire = tag & 7;
        if (field == 2 && wire == 2) {
            uint32_t slen = 0;
            if (!pb_get_varint(pb, pb_len, &offset, &slen) || offset + slen > pb_len) {
                return false;
            }
            bool ok = decode_mesh_packet(pb + offset, slen, portnum, payload, payload_len);
            offset += slen;
            if (ok) {
                return true;
            }
        } else if (!pb_skip(pb, pb_len, &offset, wire)) {
            return false;
        }
    }
    return false;
}

static bool uart_write_locked(const void* buf, size_t len) {
    if (s_tx_lock == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }
    int written = uart_write_bytes(MESHTASTIC_UART_NUM, buf, len);
    xSemaphoreGive(s_tx_lock);
    return written == (int)len;
}

bool mesh_link_write_raw(const uint8_t* data, size_t len) {
    return uart_write_locked(data, len);
}

void mesh_link_init(void) {
    if (s_tx_lock == NULL) {
        s_tx_lock = xSemaphoreCreateMutex();
    }
    s_rx_len = 0;

#ifndef MESH_SERIAL_SIMPLE
    uint8_t wakeup[4] = {MESH_START1, MESH_START1, MESH_START1, MESH_START1};
    uart_write_locked(wakeup, sizeof(wakeup));
    uint8_t cfg[16];
    size_t n = 0;
    if (mesh_encode_want_config((uint32_t)esp_random(), cfg, sizeof(cfg), &n)) {
        uart_write_locked(cfg, n);
    }
#endif
}

bool mesh_link_send(uint32_t portnum, const uint8_t* payload, size_t payload_len,
                    bool want_ack, uint8_t priority) {
#ifdef MESH_SERIAL_SIMPLE
    (void)portnum;
    (void)want_ack;
    (void)priority;
    return uart_write_locked(payload, payload_len);
#else
    uint8_t packet[MESH_MAX_PB_LEN + 8];
    size_t n = 0;
    uint32_t packet_id = esp_random();
    if (packet_id == 0) {
        packet_id = 1;
    }
    if (!mesh_encode_toradio(portnum, payload, payload_len, want_ack, priority,
                             packet_id, packet, sizeof(packet), &n)) {
        return false;
    }
    return uart_write_locked(packet, n);
#endif
}

static void drop_rx(size_t n) {
    if (n >= s_rx_len) {
        s_rx_len = 0;
        return;
    }
    memmove(s_rx, s_rx + n, s_rx_len - n);
    s_rx_len -= n;
}

void mesh_link_poll(void (*on_payload)(uint32_t portnum, const uint8_t* payload, size_t len)) {
    if (on_payload == NULL) {
        return;
    }

    uint8_t chunk[256];
    int n = uart_read_bytes(MESHTASTIC_UART_NUM, chunk, sizeof(chunk), pdMS_TO_TICKS(50));
    if (n > 0) {
        size_t room = MESH_RX_BUF_LEN - s_rx_len;
        size_t take = (size_t)n < room ? (size_t)n : room;
        memcpy(s_rx + s_rx_len, chunk, take);
        s_rx_len += take;
    }

    size_t guard = 0;
    while (s_rx_len >= 4 && guard++ < MESH_RX_BUF_LEN) {
        size_t i = 0;
        while (i + 1 < s_rx_len &&
               !(s_rx[i] == MESH_START1 && s_rx[i + 1] == MESH_START2) &&
               !(s_rx[i] == MESH_MAGIC0 && s_rx[i + 1] == MESH_MAGIC1)) {
            i++;
        }
        if (i > 0) {
            drop_rx(i);
            continue;
        }

        if (s_rx[0] == MESH_START1 && s_rx[1] == MESH_START2) {
            if (s_rx_len < 4) {
                return;
            }
            size_t pb_len = ((size_t)s_rx[2] << 8) | (size_t)s_rx[3];
            if (pb_len > MESH_MAX_PB_LEN) {
                drop_rx(1);
                continue;
            }
            if (s_rx_len < 4 + pb_len) {
                return;
            }
            uint32_t portnum = 0;
            const uint8_t* payload = NULL;
            size_t payload_len = 0;
            if (mesh_decode_fromradio(s_rx + 4, pb_len, &portnum, &payload, &payload_len) &&
                payload != NULL) {
                on_payload(portnum, payload, payload_len);
            }
            drop_rx(4 + pb_len);
            continue;
        }

        if (s_rx[0] == MESH_MAGIC0 && s_rx[1] == MESH_MAGIC1) {
            uint8_t msg_type = 0;
            const uint8_t* payload = NULL;
            size_t payload_len = 0;
            size_t frame_len = 0;
            if (s_rx_len < 7) {
                return;
            }
            if (!mesh_decode_frame(s_rx, s_rx_len, &msg_type, &payload, &payload_len, &frame_len)) {
                size_t claimed = 5 + ((size_t)s_rx[3] | ((size_t)s_rx[4] << 8)) + 2;
                if (claimed > s_rx_len && claimed < MESH_MAX_FRAME_LEN) {
                    return;
                }
                drop_rx(1);
                continue;
            }
            (void)msg_type;
            on_payload(MESH_PORT_SERIAL_APP, s_rx, frame_len);
            drop_rx(frame_len);
            continue;
        }

        drop_rx(1);
    }
}
