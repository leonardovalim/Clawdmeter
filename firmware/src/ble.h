#pragma once
#include <stdint.h>

enum ble_state_t {
    BLE_STATE_INIT,
    BLE_STATE_ADVERTISING,
    BLE_STATE_CONNECTED,
    BLE_STATE_DISCONNECTED,
};

void ble_init(void);
void ble_tick(void);
ble_state_t ble_get_state(void);
const char* ble_get_device_name(void);
const char* ble_get_mac_address(void);
void ble_clear_bonds(void);
bool ble_has_bonds(void);
bool ble_has_data(void);
const char* ble_get_data(void);
// Album art: true when the art state changed since the last call. *buf == nullptr
// means "cleared" (no art for the current track). Always false on PSRAM-less boards.
bool ble_take_album_art(const uint8_t** buf, int* w, int* h);
void ble_send_ack(void);
void ble_send_nack(void);
void ble_request_refresh(void);

// Transport commands pushed to the host over REQ_CHAR (same channel as the
// 0x01 refresh request). The daemon applies them to the active media session.
#define MEDIA_CMD_PLAYPAUSE 0x10
#define MEDIA_CMD_NEXT      0x11
#define MEDIA_CMD_PREV      0x12
void ble_send_media_cmd(uint8_t cmd);

// BLE HID keyboard
void ble_keyboard_press(uint8_t key, uint8_t modifier);
void ble_keyboard_release(void);
