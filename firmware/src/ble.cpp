#include "ble.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "sprint_net.h"

#define DEVICE_NAME "Clawdmeter"

// Custom GATT UUIDs for data channel
#define SERVICE_UUID        "4c41555a-4465-7669-6365-000000000001"
#define RX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000002"  // host writes here
#define TX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000003"  // device ack/nack notifies
#define REQ_CHAR_UUID       "4c41555a-4465-7669-6365-000000000004"  // device-initiated refresh request
#define PROV_CHAR_UUID       "4c41555a-4465-7669-6365-000000000005"  // WiFi Sprint provisioning blob

#define BLE_BUF_SIZE 512

// HID keyboard report descriptor (standard 6-KRO boot-protocol-compatible).
// Includes the LED output report (Num/Caps/Scroll Lock indicators) — without
// it macOS's Keyboard Setup Assistant flags the device as "unidentifiable"
// because the descriptor doesn't look like a complete keyboard.
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x06,  // Usage (Keyboard)
    0xA1, 0x01,  // Collection (Application)
    0x85, 0x01,  //   Report ID (1)
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0xE0,  //   Usage Minimum (224) - Left Control
    0x29, 0xE7,  //   Usage Maximum (231) - Right GUI
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x01,  //   Logical Maximum (1)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x08,  //   Report Count (8)
    0x81, 0x02,  //   Input (Data, Variable, Absolute) - Modifier byte
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x08,  //   Report Size (8)
    0x81, 0x01,  //   Input (Constant) - Reserved byte
    // LED output report — required for macOS to treat this as a full keyboard.
    0x95, 0x05,  //   Report Count (5)
    0x75, 0x01,  //   Report Size (1)
    0x05, 0x08,  //   Usage Page (LEDs)
    0x19, 0x01,  //   Usage Minimum (Num Lock)
    0x29, 0x05,  //   Usage Maximum (Kana)
    0x91, 0x02,  //   Output (Data, Variable, Absolute) - LED report
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x03,  //   Report Size (3)
    0x91, 0x01,  //   Output (Constant) - LED report padding
    0x95, 0x06,  //   Report Count (6)
    0x75, 0x08,  //   Report Size (8)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x65,  //   Logical Maximum (101)
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0x00,  //   Usage Minimum (0)
    0x29, 0x65,  //   Usage Maximum (101)
    0x81, 0x00,  //   Input (Data, Array) - Key array (6 keys)
    0xC0,        // End Collection

    // Consumer Control collection — standard media keys (Play/Pause, Next,
    // Previous). A second, independent report on the same HID device; macOS/
    // Windows route these to whichever app currently owns the Now Playing
    // session (Spotify, Music, etc.), same as a physical Bluetooth remote.
    0x05, 0x0C,  // Usage Page (Consumer)
    0x09, 0x01,  // Usage (Consumer Control)
    0xA1, 0x01,  // Collection (Application)
    0x85, 0x02,  //   Report ID (2)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x01,  //   Logical Maximum (1)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x03,  //   Report Count (3)
    0x09, 0xCD,  //   Usage (Play/Pause)
    0x09, 0xB5,  //   Usage (Scan Next Track)
    0x09, 0xB6,  //   Usage (Scan Previous Track)
    0x81, 0x02,  //   Input (Data, Variable, Absolute)
    0x95, 0x05,  //   Report Count (5) - padding to fill the byte
    0x75, 0x01,  //   Report Size (1)
    0x81, 0x03,  //   Input (Constant, Variable, Absolute)
    0xC0,        // End Collection
};

// Consumer Control usage bits (bit position within the single report byte,
// matches the field order declared in HID_REPORT_MAP above).
#define MEDIA_BIT_PLAY_PAUSE 0x01
#define MEDIA_BIT_NEXT_TRACK 0x02
#define MEDIA_BIT_PREV_TRACK 0x04

static NimBLEServer* server = nullptr;
static NimBLEHIDDevice* hid_dev = nullptr;
static NimBLECharacteristic* input_kbd = nullptr;
static NimBLECharacteristic* input_media = nullptr;
static NimBLECharacteristic* tx_char = nullptr;
static NimBLECharacteristic* rx_char = nullptr;
static NimBLECharacteristic* req_char = nullptr;
static NimBLECharacteristic* prov_char = nullptr;

static ble_state_t state = BLE_STATE_INIT;
static bool need_advertise = false;
static char rx_buf[BLE_BUF_SIZE];
static volatile bool data_ready = false;

// --- Album art (media screen) ---------------------------------------------
// The daemon streams a 96x96 RGB565 (little-endian) cover in binary frames on
// the RX characteristic: 'C','A', chunk_idx, flags(bit0 = last), payload.
// Chunks arrive in order (host writes with response); a lost/out-of-order
// chunk aborts the transfer until the host restarts at idx 0. PSRAM-only —
// C6 boards silently ignore the frames.
#ifdef BOARD_HAS_PSRAM
#define ART_W 96
#define ART_H 96
#define ART_BYTES (ART_W * ART_H * 2)
static uint8_t* art_buf = nullptr;       // PSRAM reassembly buffer
static size_t   art_off = 0;
static uint8_t  art_expect_idx = 0;
static volatile bool art_ready = false;  // complete image waiting for the UI
static volatile bool art_clear = false;  // daemon says: no art for this track
#endif
static volatile bool has_received_data = false;
static char mac_str[18];

// --- Last-writer-wins owner lock --------------------------------------------
//
// The board is a BLE peripheral any central in range could connect to. To stop
// the display rotating to a stranger's account, only writes over a bonded +
// encrypted link are accepted. Any bonded machine that writes usage data
// automatically claims ownership (last-writer-wins), persisting the owner's
// identity address in NVS. When you take Cleitin between work (macOS) and home
// (Windows), the new machine becomes the owner on the first write — no gesture
// needed. Bonds are never deleted except by the explicit hold-power gesture
// (ble_clear_bonds), to hand the board over permanently.
static Preferences prefs;
static char owner_addr[18] = {0};   // owner identity address, e.g. "aa:bb:cc:dd:ee:ff"
static bool owner_set = false;
static const char* ZERO_ADDR = "00:00:00:00:00:00";

static void save_owner() {
    prefs.begin("clawd", false);
    prefs.putString("owner", owner_addr);
    prefs.end();
}

static void clear_owner() {
    owner_set = false;
    owner_addr[0] = '\0';
    prefs.begin("clawd", false);
    prefs.remove("owner");
    prefs.end();
}

static void load_owner() {
    prefs.begin("clawd", true);
    String o = prefs.getString("owner", "");
    prefs.end();
    if (o.length() == 17) {  // "aa:bb:cc:dd:ee:ff"
        strncpy(owner_addr, o.c_str(), sizeof(owner_addr) - 1);
        owner_addr[sizeof(owner_addr) - 1] = '\0';
        owner_set = true;
        Serial.printf("BLE: owner loaded = %s\n", owner_addr);
    }
}

// Intentionally dead code — never called. Removing this function shifts the
// binary layout enough that Core 1's ipc_task overflows its stack during
// gpio_isr_register (heap poisoning path). The root cause is a framework-level
// stack budget bug; keeping this function in the binary is the least-invasive
// workaround until the toolchain is updated.
static void __attribute__((used)) prune_foreign_bonds() {
    if (!owner_set) return;
    bool removed;
    do {
        removed = false;
        int n = NimBLEDevice::getNumBonds();
        for (int i = 0; i < n; i++) {
            NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
            if (strcmp(a.toString().c_str(), owner_addr) != 0) {
                Serial.printf("BLE: pruning non-owner bond %s\n", a.toString().c_str());
                NimBLEDevice::deleteBond(a);
                removed = true;
                break;
            }
        }
    } while (removed);
}

static void claim_owner(const std::string& id) {
    strncpy(owner_addr, id.c_str(), sizeof(owner_addr) - 1);
    owner_addr[sizeof(owner_addr) - 1] = '\0';
    owner_set = true;
    save_owner();
    Serial.printf("BLE: owner claimed = %s\n", owner_addr);
    // NOTE: we deliberately do NOT prune other stored bonds here. A boot-time /
    // claim-time prune compared owner_addr against getBondedAddress(i).toString();
    // any address-representation difference pruned the legitimate host's own bond,
    // so the host saw "Peer removed pairing information" (CBError 14) and had to
    // re-pair every session. With last-writer-wins, multiple machines may hold
    // bonds simultaneously — the one actively writing usage data is the owner.
    // The only path that clears bonds is the explicit hold-power gesture
    // (ble_clear_bonds).
}

static void start_advertising() {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->reset();
    // Primary advertising packet (≤31 bytes):
    //   flags (3) + appearance (4) + HID service 0x1812 (4) + name "Clawdmeter" (12)
    //   = 23 bytes. macOS Bluetooth Settings only surfaces BLE-only devices
    //   that explicitly advertise the standard HID service UUID (0x1812) —
    //   without it the device is recognized internally but hidden from the
    //   GUI nearby-devices list.
    adv->setAppearance(HID_KEYBOARD);
    adv->addServiceUUID(NimBLEUUID((uint16_t)0x1812));  // BLE HID Service
    adv->setName(DEVICE_NAME);
    // Scan response carries the 128-bit custom data-service UUID for active
    // scanners (the host daemon scans actively).
    NimBLEAdvertisementData scanResp;
    scanResp.setCompleteServices(NimBLEUUID(SERVICE_UUID));
    adv->setScanResponseData(scanResp);
    adv->enableScanResponse(true);
    bool ok = adv->start();
    // Only reflect ADVERTISING in the UI state when no client is connected.
    // With MAX_CONNECTIONS=2, onConnect re-advertises to fill the second slot;
    // without this guard the UI would flip CONNECTED → ADVERTISING on every
    // first connect and never come back until a second client arrived.
    if (!server || server->getConnectedCount() == 0) {
        state = BLE_STATE_ADVERTISING;
    }
    Serial.printf("BLE: advertising start=%s (connected=%u)\n",
        ok ? "OK" : "FAILED",
        server ? (unsigned)server->getConnectedCount() : 0);
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
        state = BLE_STATE_CONNECTED;
        Serial.printf("BLE: connected from %s (active=%u)\n",
            info.getAddress().toString().c_str(),
            (unsigned)s->getConnectedCount());
        // Keep advertising while a connection slot is still free so a second
        // central (e.g. the host daemon alongside an OS-held HID link) can
        // discover and connect. NimBLE auto-stops advertising on each accept.
        if (s->getConnectedCount() < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
            need_advertise = true;
        }
    }

    void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
        // Only flip the UI state to DISCONNECTED when the last client leaves.
        if (s->getConnectedCount() == 0) state = BLE_STATE_DISCONNECTED;
        need_advertise = true;
        Serial.printf("BLE: disconnected (reason=%d, remaining=%u)\n",
            reason, (unsigned)s->getConnectedCount());
    }

    // Last-writer-wins: any machine that completes bonding and authentication is
    // welcome. If no owner is set yet, the first bonder claims it. A different
    // machine bonding does NOT delete or reject — it will claim ownership when
    // it actually writes usage data (see RxCallbacks below).
    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        std::string id = info.getIdAddress().toString();
        Serial.printf("BLE: auth complete peer=%s bonded=%d enc=%d\n",
            id.c_str(), info.isBonded() ? 1 : 0, info.isEncrypted() ? 1 : 0);
        if (id == ZERO_ADDR) return;
        if (!owner_set) {
            claim_owner(id);
        }
    }

};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        // Only accept usage data over a bonded+encrypted link. The first
        // encrypted writer claims ownership; any subsequent encrypted writer
        // from a different machine takes over (last-writer-wins). This lets
        // Cleitin travel between work and home without a gesture.
        std::string id = info.getIdAddress().toString();
        if (!info.isEncrypted()) {
            Serial.println("BLE: dropping RX write from unencrypted link");
            return;
        }
        if (id == ZERO_ADDR) return;
        if (!owner_set || strcmp(id.c_str(), owner_addr) != 0) {
            claim_owner(id);
        }
        std::string val = chr->getValue();
#ifdef BOARD_HAS_PSRAM
        // Binary album-art frame? JSON payloads always start with '{', so the
        // "CA" magic cannot collide.
        if (val.length() >= 4 && val[0] == 'C' && val[1] == 'A') {
            uint8_t idx   = (uint8_t)val[2];
            uint8_t flags = (uint8_t)val[3];
            if (idx == 0xFF) {                       // art-clear frame
                art_clear = true;
                return;
            }
            if (!art_buf) {
                art_buf = (uint8_t*)heap_caps_malloc(ART_BYTES, MALLOC_CAP_SPIRAM);
                if (!art_buf) return;
            }
            if (idx == 0) { art_off = 0; art_expect_idx = 0; }
            if (idx != art_expect_idx) {             // lost a chunk — abort quietly
                art_expect_idx = 0;
                art_off = 0;
                return;
            }
            size_t n = val.length() - 4;
            if (art_off + n > ART_BYTES) {           // oversized — abort
                art_expect_idx = 0;
                art_off = 0;
                return;
            }
            memcpy(art_buf + art_off, val.data() + 4, n);
            art_off += n;
            art_expect_idx++;
            if (flags & 0x01) {
                if (art_off == ART_BYTES) art_ready = true;
                art_expect_idx = 0;
            }
            return;
        }
#endif
        size_t len = std::min(val.length(), (size_t)(BLE_BUF_SIZE - 1));
        memcpy(rx_buf, val.c_str(), len);
        rx_buf[len] = '\0';
        data_ready = true;
        has_received_data = true;
    }
};

// WiFi Sprint provisioning: the daemon writes a {"ssid","pw","tok"} JSON blob
// once per BLE connection. Same encrypted-link gate as RxCallbacks — no
// owner check needed since sprint_net_provision only persists to this
// device's own NVS, it doesn't affect who owns the data channel.
class ProvCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        if (!info.isEncrypted()) return;                 // same gate as RxCallbacks
        std::string v = chr->getValue();
        JsonDocument doc;
        if (deserializeJson(doc, v)) return;
        const char* ssid = doc["ssid"] | "";
        const char* pw   = doc["pw"]   | "";
        const char* tok  = doc["tok"]  | "";
        if (ssid[0] && tok[0]) {
            sprint_net_provision(ssid, pw, tok);
            Serial.println("BLE: WiFi provisioning received");
        }
    }
};
static ProvCallbacks provCb;

// When the daemon enables notifications on the refresh char, ask for data
// if we have none yet. Firing on subscribe (not on connect) ensures the
// notification isn't dropped before the daemon's CCCD write completes.
class ReqCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* chr, NimBLEConnInfo& info, uint16_t subValue) override {
        Serial.printf("BLE: req_char onSubscribe subValue=%u has_data=%d\n", subValue, has_received_data ? 1 : 0);
        if (subValue != 0 && !has_received_data) {
            ble_request_refresh();
        }
    }
};

void ble_init(void) {
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, no MITM, SC

    // Restore the locked owner (if any). We do NOT prune bonds here: a boot-time
    // prune deleted the host's own bond whenever its stored address
    // representation differed from the live one, causing "Peer removed pairing
    // information" (CBError 14) and a forced re-pair every session. Bonds are
    // only cleared by the explicit hold-power gesture (ble_clear_bonds).
    load_owner();

    // Format MAC address
    NimBLEAddress addr = NimBLEDevice::getAddress();
    snprintf(mac_str, sizeof(mac_str), "%s", addr.toString().c_str());
    for (int i = 0; mac_str[i]; i++) {
        if (mac_str[i] >= 'a' && mac_str[i] <= 'f') mac_str[i] -= 32;
    }

    server = NimBLEDevice::createServer();
    static ServerCallbacks serverCb;
    server->setCallbacks(&serverCb);

    // --- HID keyboard service ---
    hid_dev = new NimBLEHIDDevice(server);
    hid_dev->setReportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
    hid_dev->setManufacturer("Anthropic");
    // PnP ID: (vendorIdSource, vendorId, productId, version).
    // Source 1 = Bluetooth SIG, vendor 0x02E5 = Espressif. Originally claimed
    // Apple's USB vendor 0x05AC + Magic Keyboard product 0x820A — macOS
    // validates Apple-claimed HIDs against known device IDs and silently
    // refuses to surface a Connect button for spoofers.
    hid_dev->setPnp(0x01, 0x02E5, 0x0001, 0x0100);
    // country=33 (US ANSI). Setting this to 0 ("not supported") causes macOS
    // to launch the Keyboard Setup Assistant on first pair asking the user
    // to identify the layout — we only ever send a handful of fixed keys
    // (Space, Shift+Tab, media keys) so the physical layout is irrelevant;
    // advertise a known one to skip the wizard.
    hid_dev->setHidInfo(33, 0x02);
    hid_dev->setBatteryLevel(100);
    input_kbd   = hid_dev->getInputReport(1);  // report ID 1 — keyboard
    input_media = hid_dev->getInputReport(2);  // report ID 2 — consumer control

    // --- Custom data service ---
    NimBLEService* svc = server->createService(SERVICE_UUID);

    rx_char = svc->createCharacteristic(
        RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    static RxCallbacks rxCb;
    rx_char->setCallbacks(&rxCb);

    tx_char = svc->createCharacteristic(
        TX_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    req_char = svc->createCharacteristic(
        REQ_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );
    static ReqCallbacks reqCb;
    req_char->setCallbacks(&reqCb);

    prov_char = svc->createCharacteristic(
        PROV_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    prov_char->setCallbacks(&provCb);

    svc->start();
    server->start();
    start_advertising();

    Serial.printf("BLE: init complete, MAC=%s\n", mac_str);
}

void ble_tick(void) {
    if (need_advertise) {
        need_advertise = false;
        start_advertising();
    }
}

ble_state_t ble_get_state(void) {
    return state;
}

const char* ble_get_device_name(void) {
    return DEVICE_NAME;
}

const char* ble_get_mac_address(void) {
    return mac_str;
}

void ble_clear_bonds(void) {
    NimBLEDevice::deleteAllBonds();
    clear_owner();  // release ownership so the board can be handed to another machine
    Serial.println("BLE: bonds cleared");
    if (state == BLE_STATE_CONNECTED) {
        server->disconnect(server->getPeerInfo(0).getConnHandle());
    }
    need_advertise = true;
}

bool ble_has_bonds(void) {
    return NimBLEDevice::getNumBonds() > 0;
}

bool ble_has_data(void) {
    return data_ready;
}

const char* ble_get_data(void) {
    data_ready = false;
    return rx_buf;
}

bool ble_take_album_art(const uint8_t** buf, int* w, int* h) {
#ifdef BOARD_HAS_PSRAM
    if (art_clear) {
        art_clear = false;
        art_ready = false;
        *buf = nullptr;
        *w = *h = 0;
        return true;
    }
    if (!art_ready) return false;
    art_ready = false;
    *buf = art_buf;
    *w = ART_W;
    *h = ART_H;
    return true;
#else
    (void)buf; (void)w; (void)h;
    return false;
#endif
}

void ble_send_ack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"ack\":true}");
        tx_char->notify();
    }
}

void ble_send_nack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"err\":true}");
        tx_char->notify();
    }
}

void ble_request_refresh(void) {
    if (state == BLE_STATE_CONNECTED && req_char) {
        uint8_t v = 0x01;
        req_char->setValue(&v, 1);
        req_char->notify();
        Serial.println("BLE: refresh requested");
    }
}

void ble_send_media_cmd(uint8_t cmd) {
    if (state == BLE_STATE_CONNECTED && req_char) {
        req_char->setValue(&cmd, 1);
        req_char->notify();
        Serial.printf("BLE: media cmd 0x%02x\n", cmd);
    }
}

void ble_keyboard_press(uint8_t key, uint8_t modifier) {
    if (state != BLE_STATE_CONNECTED || !input_kbd) return;
    // HID report: [modifier, reserved, key1, key2, key3, key4, key5, key6]
    uint8_t report[8] = {modifier, 0, key, 0, 0, 0, 0, 0};
    input_kbd->setValue(report, sizeof(report));
    input_kbd->notify();
}

void ble_keyboard_release(void) {
    if (state != BLE_STATE_CONNECTED || !input_kbd) return;
    uint8_t report[8] = {0};
    input_kbd->setValue(report, sizeof(report));
    input_kbd->notify();
}

static void ble_media_key_tap(uint8_t bit) {
    if (state != BLE_STATE_CONNECTED || !input_media) return;
    uint8_t report = bit;
    input_media->setValue(&report, 1);
    input_media->notify();
    report = 0;
    input_media->setValue(&report, 1);
    input_media->notify();
}

void ble_media_play_pause(void)  { ble_media_key_tap(MEDIA_BIT_PLAY_PAUSE); }
void ble_media_next_track(void)  { ble_media_key_tap(MEDIA_BIT_NEXT_TRACK); }
void ble_media_prev_track(void)  { ble_media_key_tap(MEDIA_BIT_PREV_TRACK); }
