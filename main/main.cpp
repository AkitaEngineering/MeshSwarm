#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "../provisioning_pubkey.h"
#include "mavlink_min.h"
#include "mesh_codec.h"

static const char *TAG = "MESHSWARM";

#define CONSOLE_UART_NUM      UART_NUM_0
#define MESHTASTIC_UART_NUM   UART_NUM_1
#define MAVLINK_UART_NUM      UART_NUM_2

#define UART_RX_BUF_SIZE      1024
#define UART_TX_BUF_SIZE      1024

#define MESH_TXD_PIN          (4)
#define MESH_RXD_PIN          (5)
#define MAV_TXD_PIN           (17)
#define MAV_RXD_PIN           (16)

#ifndef DRONE_ID
#define DRONE_ID              1
#endif
#define SWARM_BROADCAST_ID    255
#define GCS_SYSTEM_ID         255
#define GCS_COMPONENT_ID      1
#define FC_TARGET_SYSTEM_ID   1
#define FC_TARGET_COMPONENT_ID 1

#define COMMAND_RTL               1
#define COMMAND_LAND              2
#define COMMAND_EMERGENCY_LAND    3
#define COMMAND_SYNC_REQUEST      4
#define COMMAND_HEARTBEAT         5

#define ACK_STATUS_ACCEPTED       1
#define ACK_STATUS_SYNC           2
#define ACK_STATUS_REJECTED       3

#define CONTROL_PAYLOAD_LEN 9
#define CONTROL_ACK_PAYLOAD_LEN 10
#define TELEMETRY_PAYLOAD_LEN 33
#define NONCE_LEN 12
#define TAG_LEN 16
#define KEY_HEX_LEN 32
#define MAX_SIGNATURE_LEN 128

#ifndef LOST_LINK_TIMEOUT_MS
#define LOST_LINK_TIMEOUT_MS 30000
#endif
#ifndef LOW_BATTERY_VOLTAGE
#define LOW_BATTERY_VOLTAGE 11.0f
#endif
#ifndef TELEM_SEQ_SAVE_EVERY
#define TELEM_SEQ_SAVE_EVERY 12
#endif

static uint8_t runtime_key[16];
static const size_t kKeyLen = sizeof(runtime_key);
static bool runtime_key_configured = false;
static SemaphoreHandle_t key_mutex;

#ifdef ALLOW_INSECURE_DEFAULT_KEY
static const uint8_t kDefaultKey[] = {
    0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
    0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C,
};
#endif

static uint32_t last_control_seq = 0;
static uint32_t telemetry_seq = 0;
static uint8_t drone_id = DRONE_ID;

struct DroneTelemetry {
    int32_t lat_e7;
    int32_t lon_e7;
    int32_t alt_mm;
    float batteryVoltage;
    float roll;
    float pitch;
    float yaw;
    uint8_t droneID;
};

static DroneTelemetry current_telemetry = {0, 0, 0, 0, 0, 0, 0, DRONE_ID};
static portMUX_TYPE telemetry_spinlock = portMUX_INITIALIZER_UNLOCKED;

static float geofence_lat = 0;
static float geofence_lon = 0;
static float geofence_radius_m = 0;
static bool geofence_rtl_sent = false;
static bool low_batt_rtl_sent = false;
static bool lost_link_rtl_sent = false;
static bool gcs_ever_seen = false;
static int64_t last_gcs_rx_us = 0;
static uint32_t mesh_hb_nonce = 1;

static void fill_random_bytes(uint8_t* buf, size_t len) {
    esp_fill_random(buf, len);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool hex_to_bytes(const char* hex, uint8_t* out, size_t out_len) {
    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2) {
        return false;
    }
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool verify_provisioning_signature(const uint8_t* key, const uint8_t* sig, size_t sig_len) {
    uint8_t hash[32];
    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL || mbedtls_md(md_info, key, kKeyLen, hash) != 0) {
        ESP_LOGE(TAG, "Failed to hash provisioning key.");
        return false;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = mbedtls_pk_parse_public_key(
        &pk,
        (const unsigned char*)PROVISIONING_PUBKEY_PEM,
        strlen(PROVISIONING_PUBKEY_PEM) + 1
    );
    if (rc != 0) {
        ESP_LOGE(TAG, "Provisioning public key parse failed: -0x%04x", (unsigned int)-rc);
        mbedtls_pk_free(&pk);
        return false;
    }

    rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sig_len);
    mbedtls_pk_free(&pk);
    return rc == 0;
}

static void load_state_from_nvs() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
#ifdef ALLOW_INSECURE_DEFAULT_KEY
        memcpy(runtime_key, kDefaultKey, kKeyLen);
        runtime_key_configured = true;
#endif
        return;
    }

    size_t required_size = kKeyLen;
    err = nvs_get_blob(my_handle, "aes_key", runtime_key, &required_size);
    if (err != ESP_OK || required_size != kKeyLen) {
        ESP_LOGE(TAG, "AES Key not found in NVS. Provision with SETKEYSIG before use.");
#ifdef ALLOW_INSECURE_DEFAULT_KEY
        ESP_LOGW(TAG, "Using insecure compiled default key because ALLOW_INSECURE_DEFAULT_KEY is set.");
        memcpy(runtime_key, kDefaultKey, kKeyLen);
        runtime_key_configured = true;
#endif
    } else {
        ESP_LOGI(TAG, "AES Key loaded from NVS.");
        runtime_key_configured = true;
    }

    nvs_get_u32(my_handle, "ctrl_seq", &last_control_seq);
    nvs_get_u32(my_handle, "telem_seq", &telemetry_seq);
    uint8_t stored_id = 0;
    if (nvs_get_u8(my_handle, "drone_id", &stored_id) == ESP_OK && stored_id != 0) {
        drone_id = stored_id;
        current_telemetry.droneID = stored_id;
    }
    nvs_get_u32(my_handle, "fence_lat", (uint32_t*)&geofence_lat);
    nvs_get_u32(my_handle, "fence_lon", (uint32_t*)&geofence_lon);
    nvs_get_u32(my_handle, "fence_rad", (uint32_t*)&geofence_radius_m);

    nvs_close(my_handle);
}

static void save_key_to_nvs(const uint8_t* key, size_t len) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_blob(my_handle, "aes_key", key, len);
        nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "AES Key saved to NVS.");
    }
}

static void save_u32_nvs(const char* name, uint32_t value) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_u32(my_handle, name, value);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

static void save_ctrl_seq_to_nvs(uint32_t seq) {
    save_u32_nvs("ctrl_seq", seq);
}

static void save_telem_seq_to_nvs(uint32_t seq) {
    save_u32_nvs("telem_seq", seq);
}

static bool copy_runtime_key(uint8_t* out) {
    if (!runtime_key_configured || key_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(key_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    memcpy(out, runtime_key, kKeyLen);
    bool ok = runtime_key_configured;
    xSemaphoreGive(key_mutex);
    return ok;
}

static bool aes_gcm_decrypt(const uint8_t* nonce, const uint8_t* ciphertext, size_t clen,
                            const uint8_t* tag, uint8_t* plaintext) {
    uint8_t key[16];
    if (!copy_runtime_key(key)) {
        return false;
    }
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, kKeyLen * 8) != 0) {
        mbedtls_gcm_free(&gcm);
        return false;
    }
    int rc = mbedtls_gcm_auth_decrypt(&gcm, clen, nonce, NONCE_LEN, NULL, 0, tag, TAG_LEN,
                                      ciphertext, plaintext);
    mbedtls_gcm_free(&gcm);
    mbedtls_platform_zeroize(key, sizeof(key));
    return rc == 0;
}

static bool aes_gcm_encrypt(const uint8_t* plaintext, size_t plen, const uint8_t* nonce,
                            uint8_t* ciphertext, uint8_t* tag) {
    uint8_t key[16];
    if (!copy_runtime_key(key)) {
        return false;
    }
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, kKeyLen * 8) != 0) {
        mbedtls_gcm_free(&gcm);
        return false;
    }
    int rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plen, nonce, NONCE_LEN,
                                       NULL, 0, plaintext, ciphertext, TAG_LEN, tag);
    mbedtls_gcm_free(&gcm);
    mbedtls_platform_zeroize(key, sizeof(key));
    return rc == 0;
}

static void send_mavlink_buffer(const uint8_t* buf, size_t len) {
    if (len == 0) {
        return;
    }
    uart_write_bytes(MAVLINK_UART_NUM, (const char*)buf, len);
}

static void send_mavlink_command(uint16_t command, uint8_t confirmation) {
    uint8_t buf[64];
    size_t len = mavlink_min_pack_command_long(
        buf, sizeof(buf), FC_TARGET_SYSTEM_ID, FC_TARGET_COMPONENT_ID, command, confirmation);
    send_mavlink_buffer(buf, len);
}

static bool seq_is_newer(uint32_t seq, uint32_t last) {
    if (seq > last) {
        return true;
    }
    return last > 0xF0000000u && seq < 0x10000000u;
}

static void mark_gcs_seen(void) {
    gcs_ever_seen = true;
    last_gcs_rx_us = esp_timer_get_time();
    lost_link_rtl_sent = false;
}

static bool encrypt_and_send(uint8_t msg_type, const uint8_t* plaintext, size_t plen,
                             bool want_ack, uint8_t priority) {
    uint8_t nonce[NONCE_LEN];
    uint8_t ciphertext[64];
    uint8_t tag[TAG_LEN];
    if (plen > sizeof(ciphertext)) {
        return false;
    }
    fill_random_bytes(nonce, NONCE_LEN);
    if (!aes_gcm_encrypt(plaintext, plen, nonce, ciphertext, tag)) {
        return false;
    }

    uint8_t blob[NONCE_LEN + 64 + TAG_LEN];
    memcpy(blob, nonce, NONCE_LEN);
    memcpy(blob + NONCE_LEN, ciphertext, plen);
    memcpy(blob + NONCE_LEN + plen, tag, TAG_LEN);
    size_t blob_len = NONCE_LEN + plen + TAG_LEN;

    uint8_t frame[MESH_MAX_FRAME_LEN];
    size_t frame_len = mesh_encode_frame(msg_type, blob, blob_len, frame, sizeof(frame));
    if (frame_len == 0) {
        return false;
    }
    return mesh_link_send(MESH_PORT_PRIVATE_APP, frame, frame_len, want_ack, priority);
}

static void send_control_ack(uint32_t request_seq, uint8_t status) {
    uint8_t plaintext[CONTROL_ACK_PAYLOAD_LEN];
    memcpy(plaintext, &request_seq, sizeof(request_seq));
    plaintext[4] = drone_id;
    plaintext[5] = status;
    memcpy(plaintext + 6, &last_control_seq, sizeof(last_control_seq));
    encrypt_and_send(MESH_MSG_ACK, plaintext, CONTROL_ACK_PAYLOAD_LEN, false, 70);
}

static float haversine_m(float lat1, float lon1, float lat2, float lon2) {
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

static bool gps_valid(const DroneTelemetry* telem) {
    return telem->lat_e7 != 0 || telem->lon_e7 != 0;
}

static void maybe_failsafe(void) {
    DroneTelemetry snap;
    portENTER_CRITICAL(&telemetry_spinlock);
    snap = current_telemetry;
    portEXIT_CRITICAL(&telemetry_spinlock);

    if (geofence_radius_m > 0.0f && gps_valid(&snap)) {
        float lat = snap.lat_e7 / 10000000.0f;
        float lon = snap.lon_e7 / 10000000.0f;
        float dist = haversine_m(geofence_lat, geofence_lon, lat, lon);
        if (dist > geofence_radius_m) {
            if (!geofence_rtl_sent) {
                ESP_LOGW(TAG, "Geofence breach (%.1fm); RTL", dist);
                send_mavlink_command(MAV_CMD_NAV_RETURN_TO_LAUNCH, 0);
                geofence_rtl_sent = true;
            }
        } else {
            geofence_rtl_sent = false;
        }
    }

    if (snap.batteryVoltage > 0.5f && snap.batteryVoltage < LOW_BATTERY_VOLTAGE) {
        if (!low_batt_rtl_sent) {
            ESP_LOGW(TAG, "Low battery %.2fV; RTL", snap.batteryVoltage);
            send_mavlink_command(MAV_CMD_NAV_RETURN_TO_LAUNCH, 0);
            low_batt_rtl_sent = true;
        }
    } else if (snap.batteryVoltage >= LOW_BATTERY_VOLTAGE) {
        low_batt_rtl_sent = false;
    }

    if (gcs_ever_seen && !lost_link_rtl_sent) {
        int64_t now = esp_timer_get_time();
        if ((now - last_gcs_rx_us) > ((int64_t)LOST_LINK_TIMEOUT_MS * 1000)) {
            ESP_LOGW(TAG, "Lost GCS link; RTL");
            send_mavlink_command(MAV_CMD_NAV_RETURN_TO_LAUNCH, 0);
            lost_link_rtl_sent = true;
        }
    }
}

static void handle_control_plaintext(const uint8_t* plaintext) {
    uint32_t seq = 0;
    memcpy(&seq, plaintext, sizeof(seq));
    uint8_t dest = plaintext[4];
    int32_t command = 0;
    memcpy(&command, plaintext + 5, sizeof(command));

    if (dest != drone_id && dest != SWARM_BROADCAST_ID) {
        ESP_LOGI(TAG, "Command is for another drone; ignored.");
        return;
    }

    mark_gcs_seen();

    if (command == COMMAND_SYNC_REQUEST) {
        if (seq_is_newer(seq, last_control_seq)) {
            last_control_seq = seq;
            save_ctrl_seq_to_nvs(seq);
        }
        send_control_ack(seq, ACK_STATUS_SYNC);
        return;
    }

    if (!seq_is_newer(seq, last_control_seq)) {
        ESP_LOGW(TAG, "Stale/Replay control command rejected. Seq: %lu", (unsigned long)seq);
        send_control_ack(seq, ACK_STATUS_REJECTED);
        return;
    }

    uint8_t ack_status = ACK_STATUS_ACCEPTED;
    switch (command) {
        case COMMAND_RTL:
            ESP_LOGI(TAG, "Executing RTL");
            send_mavlink_command(MAV_CMD_NAV_RETURN_TO_LAUNCH, 0);
            break;
        case COMMAND_LAND:
            ESP_LOGI(TAG, "Executing Land");
            send_mavlink_command(MAV_CMD_NAV_LAND, 0);
            break;
        case COMMAND_EMERGENCY_LAND:
            ESP_LOGW(TAG, "Executing Emergency Land");
            send_mavlink_command(MAV_CMD_NAV_LAND, 1);
            break;
        case COMMAND_HEARTBEAT:
            break;
        default:
            ESP_LOGW(TAG, "Unsupported command rejected: %ld", (long)command);
            ack_status = ACK_STATUS_REJECTED;
            break;
    }

    last_control_seq = seq;
    save_ctrl_seq_to_nvs(seq);
    send_control_ack(seq, ack_status);
}

static bool decrypt_blob(const uint8_t* blob, size_t blob_len, size_t plaintext_len,
                         uint8_t* plaintext) {
    if (blob_len != NONCE_LEN + plaintext_len + TAG_LEN) {
        return false;
    }
    return aes_gcm_decrypt(blob, blob + NONCE_LEN, plaintext_len,
                           blob + NONCE_LEN + plaintext_len, plaintext);
}

static void on_mesh_payload(uint32_t portnum, const uint8_t* payload, size_t len) {
    uint8_t msg_type = 0;
    const uint8_t* inner = NULL;
    size_t inner_len = 0;
    size_t frame_len = 0;
    if (!mesh_decode_frame(payload, len, &msg_type, &inner, &inner_len, &frame_len)) {
        if (portnum != MESH_PORT_PRIVATE_APP && portnum != MESH_PORT_SERIAL_APP) {
            return;
        }
        msg_type = MESH_MSG_CONTROL;
        inner = payload;
        inner_len = len;
    }
    (void)frame_len;

    if (msg_type != MESH_MSG_CONTROL || inner == NULL) {
        return;
    }

    if (!runtime_key_configured) {
        ESP_LOGW(TAG, "Ignoring control: AES key not provisioned");
        return;
    }

    uint8_t plaintext[CONTROL_PAYLOAD_LEN];
    if (!decrypt_blob(inner, inner_len, CONTROL_PAYLOAD_LEN, plaintext)) {
        ESP_LOGE(TAG, "Failed to decrypt incoming control packet.");
        return;
    }
    handle_control_plaintext(plaintext);
}

static void apply_mavlink_message(const mavlink_min_message_t* msg) {
    portENTER_CRITICAL(&telemetry_spinlock);
    if (msg->msgid == MAVLINK_MSG_ID_GLOBAL_POSITION_INT && msg->payload_len >= 28) {
        int32_t lat, lon, alt;
        memcpy(&lat, msg->payload + 4, 4);
        memcpy(&lon, msg->payload + 8, 4);
        memcpy(&alt, msg->payload + 12, 4);
        current_telemetry.lat_e7 = lat;
        current_telemetry.lon_e7 = lon;
        current_telemetry.alt_mm = alt;
    } else if (msg->msgid == MAVLINK_MSG_ID_SYS_STATUS && msg->payload_len >= 16) {
        uint16_t mv;
        memcpy(&mv, msg->payload + 14, 2);
        current_telemetry.batteryVoltage = mv / 1000.0f;
    } else if (msg->msgid == MAVLINK_MSG_ID_ATTITUDE && msg->payload_len >= 16) {
        float roll, pitch, yaw;
        memcpy(&roll, msg->payload + 4, 4);
        memcpy(&pitch, msg->payload + 8, 4);
        memcpy(&yaw, msg->payload + 12, 4);
        current_telemetry.roll = roll;
        current_telemetry.pitch = pitch;
        current_telemetry.yaw = yaw;
    }
    portEXIT_CRITICAL(&telemetry_spinlock);
}

static void task_mavlink_rx(void *arg) {
    (void)arg;
    uint8_t* data = (uint8_t*)malloc(UART_RX_BUF_SIZE);
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate MAVLink RX buffer.");
        vTaskDelete(NULL);
        return;
    }
    mavlink_min_message_t msg = {};
    int64_t last_hb_us = 0;
    while (1) {
        int len = uart_read_bytes(MAVLINK_UART_NUM, data, UART_RX_BUF_SIZE, 50 / portTICK_PERIOD_MS);
        for (int i = 0; i < len; i++) {
            if (mavlink_min_parse_byte(data[i], &msg)) {
                apply_mavlink_message(&msg);
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - last_hb_us >= 1000000) {
            uint8_t hb[32];
            size_t n = mavlink_min_pack_heartbeat(hb, sizeof(hb));
            send_mavlink_buffer(hb, n);
            last_hb_us = now;
        }
    }
}

static void task_meshtastic_rx(void *arg) {
    (void)arg;
    while (1) {
        mesh_link_poll(on_mesh_payload);
        maybe_failsafe();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void task_telemetry_tx(void *arg) {
    (void)arg;
    bool logged_unprovisioned = false;
    while (1) {
        if (!runtime_key_configured) {
            if (!logged_unprovisioned) {
                ESP_LOGW(TAG, "Telemetry paused until SETKEYSIG provisions an AES key");
                logged_unprovisioned = true;
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        logged_unprovisioned = false;

        uint8_t plaintext[TELEMETRY_PAYLOAD_LEN];
        size_t pos = 0;

        telemetry_seq++;
        if ((telemetry_seq % TELEM_SEQ_SAVE_EVERY) == 0) {
            save_telem_seq_to_nvs(telemetry_seq);
        }

        portENTER_CRITICAL(&telemetry_spinlock);
        memcpy(plaintext + pos, &telemetry_seq, sizeof(uint32_t)); pos += 4;
        memcpy(plaintext + pos, &current_telemetry.lat_e7, 4); pos += 4;
        memcpy(plaintext + pos, &current_telemetry.lon_e7, 4); pos += 4;
        memcpy(plaintext + pos, &current_telemetry.alt_mm, 4); pos += 4;
        memcpy(plaintext + pos, &current_telemetry.batteryVoltage, 4); pos += 4;
        memcpy(plaintext + pos, &current_telemetry.roll, 4); pos += 4;
        memcpy(plaintext + pos, &current_telemetry.pitch, 4); pos += 4;
        memcpy(plaintext + pos, &current_telemetry.yaw, 4); pos += 4;
        plaintext[pos] = current_telemetry.droneID;
        portEXIT_CRITICAL(&telemetry_spinlock);

        if (!encrypt_and_send(MESH_MSG_TELEMETRY, plaintext, TELEMETRY_PAYLOAD_LEN, false, 10)) {
            ESP_LOGE(TAG, "Telemetry send failed.");
        } else {
            ESP_LOGD(TAG, "Telemetry sent seq=%lu", (unsigned long)telemetry_seq);
        }

#ifndef MESH_SERIAL_SIMPLE
        uint8_t hb[32];
        size_t n = 0;
        if (mesh_encode_heartbeat(mesh_hb_nonce++, hb, sizeof(hb), &n)) {
            mesh_link_write_raw(hb, n);
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void apply_key(const uint8_t* key) {
    if (key_mutex && xSemaphoreTake(key_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(runtime_key, key, 16);
        runtime_key_configured = true;
        xSemaphoreGive(key_mutex);
    } else {
        memcpy(runtime_key, key, 16);
        runtime_key_configured = true;
    }
    save_key_to_nvs(runtime_key, 16);
}

static void task_provisioning(void *arg) {
    (void)arg;
    uint8_t* data = (uint8_t*)malloc(UART_RX_BUF_SIZE + 1);
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate provisioning buffer.");
        vTaskDelete(NULL);
        return;
    }
    while (1) {
        int len = uart_read_bytes(CONSOLE_UART_NUM, data, UART_RX_BUF_SIZE, 100 / portTICK_PERIOD_MS);
        if (len > 0) {
            data[len] = 0;
            char* signed_ptr = strstr((char*)data, "SETKEYSIG:");
            if (signed_ptr != NULL) {
                char key_hex[KEY_HEX_LEN + 1];
                char sig_hex[MAX_SIGNATURE_LEN * 2 + 1];
                int matched = sscanf(
                    signed_ptr,
                    "SETKEYSIG:%32[0-9a-fA-F]:%256[0-9a-fA-F]",
                    key_hex,
                    sig_hex
                );
                if (matched == 2) {
                    uint8_t newkey[16];
                    uint8_t sig[MAX_SIGNATURE_LEN];
                    size_t sig_len = strlen(sig_hex) / 2;
                    if ((strlen(sig_hex) % 2) != 0 || sig_len > MAX_SIGNATURE_LEN ||
                        !hex_to_bytes(key_hex, newkey, sizeof(newkey)) ||
                        !hex_to_bytes(sig_hex, sig, sig_len)) {
                        ESP_LOGE(TAG, "Invalid SETKEYSIG hex payload.");
                    } else if (verify_provisioning_signature(newkey, sig, sig_len)) {
                        apply_key(newkey);
                        ESP_LOGI(TAG, "Signed AES key provisioned.");
                    } else {
                        ESP_LOGE(TAG, "SETKEYSIG signature verification failed.");
                    }
                } else {
                    ESP_LOGE(TAG, "Invalid SETKEYSIG format. Expected SETKEYSIG:<32hex>:<der_sig_hex>.");
                }
            }

            char* id_ptr = strstr((char*)data, "SETID:");
            if (id_ptr != NULL) {
                unsigned int parsed = 0;
                if (sscanf(id_ptr, "SETID:%u", &parsed) == 1 && parsed >= 1 && parsed <= 254) {
                    drone_id = (uint8_t)parsed;
                    current_telemetry.droneID = drone_id;
                    nvs_handle_t handle;
                    if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK) {
                        nvs_set_u8(handle, "drone_id", drone_id);
                        nvs_commit(handle);
                        nvs_close(handle);
                    }
                    ESP_LOGI(TAG, "Drone ID set to %u", parsed);
                }
            }

            char* fence_ptr = strstr((char*)data, "SETFENCE:");
            if (fence_ptr != NULL) {
                float lat = 0, lon = 0, radius = 0;
                if (sscanf(fence_ptr, "SETFENCE:%f:%f:%f", &lat, &lon, &radius) == 3) {
                    geofence_lat = lat;
                    geofence_lon = lon;
                    geofence_radius_m = radius;
                    nvs_handle_t handle;
                    if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK) {
                        uint32_t bits;
                        memcpy(&bits, &geofence_lat, 4);
                        nvs_set_u32(handle, "fence_lat", bits);
                        memcpy(&bits, &geofence_lon, 4);
                        nvs_set_u32(handle, "fence_lon", bits);
                        memcpy(&bits, &geofence_radius_m, 4);
                        nvs_set_u32(handle, "fence_rad", bits);
                        nvs_commit(handle);
                        nvs_close(handle);
                    }
                    ESP_LOGI(TAG, "Geofence set lat=%.6f lon=%.6f r=%.1f", lat, lon, radius);
                }
            }

#ifdef ALLOW_INSECURE_SETKEY
            char* ptr = strstr((char*)data, "SETKEY:");
            if (ptr != NULL) {
                char hex[33];
                if (sscanf(ptr, "SETKEY:%32s", hex) == 1 && strlen(hex) == 32) {
                    uint8_t newkey[16];
                    if (hex_to_bytes(hex, newkey, sizeof(newkey))) {
                        apply_key(newkey);
                    } else {
                        ESP_LOGE(TAG, "Invalid SETKEY hex.");
                    }
                } else {
                    ESP_LOGE(TAG, "Invalid SETKEY format. Expected 32 hex chars.");
                }
            }
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    key_mutex = xSemaphoreCreateMutex();
    load_state_from_nvs();
    mavlink_min_init(GCS_SYSTEM_ID, GCS_COMPONENT_ID);

    uart_config_t uart_config_console = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(CONSOLE_UART_NUM, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE, 0, NULL, 0);
    uart_param_config(CONSOLE_UART_NUM, &uart_config_console);

    uart_config_t uart_config_mesh = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(MESHTASTIC_UART_NUM, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE, 0, NULL, 0);
    uart_param_config(MESHTASTIC_UART_NUM, &uart_config_mesh);
    uart_set_pin(MESHTASTIC_UART_NUM, MESH_TXD_PIN, MESH_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    uart_config_t uart_config_mav = {
        .baud_rate = 57600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(MAVLINK_UART_NUM, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE, 0, NULL, 0);
    uart_param_config(MAVLINK_UART_NUM, &uart_config_mav);
    uart_set_pin(MAVLINK_UART_NUM, MAV_TXD_PIN, MAV_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    mesh_link_init();
#ifdef ALLOW_INSECURE_DEFAULT_KEY
    ESP_LOGW(TAG, "ALLOW_INSECURE_DEFAULT_KEY is enabled; not for production");
#endif
#ifdef ALLOW_INSECURE_SETKEY
    ESP_LOGW(TAG, "ALLOW_INSECURE_SETKEY is enabled; not for production");
#endif
    ESP_LOGI(
        TAG,
        "MeshSwarm boot id=%u key=%s fence=%.1fm lost_link=%ums",
        (unsigned)drone_id,
        runtime_key_configured ? "nvs" : "UNPROVISIONED",
        geofence_radius_m,
        (unsigned)LOST_LINK_TIMEOUT_MS
    );
    ESP_LOGI(TAG, "UART drivers installed. Starting FreeRTOS Tasks...");

    xTaskCreate(task_mavlink_rx, "mavlink_rx", 4096, NULL, 5, NULL);
    xTaskCreate(task_meshtastic_rx, "mesh_rx", 6144, NULL, 5, NULL);
    xTaskCreate(task_telemetry_tx, "telem_tx", 6144, NULL, 4, NULL);
    xTaskCreate(task_provisioning, "provisioning", 8192, NULL, 3, NULL);
}
