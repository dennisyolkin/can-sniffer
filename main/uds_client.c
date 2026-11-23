#include "uds_client.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "esp_attr.h"
#include "esp_log.h"

// Long enough for an ECU to answer (UDS P2 is 50 ms, plus "response pending") and for every ECU to answer a
// functional OBD-II request.
#define REPLY_WINDOW_US (1000 * 1000)
// A scan step only needs TesterPresent's answer: UDS gives an ECU 50 ms (P2); the slowest so far took 21 ms.
#define SCAN_WINDOW_US (250 * 1000)
// The ESP32 controller only has single-shot or endless retransmission (twai_hal_v1.c), so a request that loses
// arbitration is retried here, a bounded number of times. Car ECUs send in bursts on their 10 ms ticks, and our
// diagnostic IDs lose to all of them: back-to-back retries (tx-logs-2) all landed in the same burst.
#define MAX_ATTEMPTS 5
#define RETRY_DELAY_US (5 * 1000)

#define UDS_NRC 0x7F
#define UDS_NRC_RESPONSE_PENDING 0x78

// ISO-TP (ISO 15765-2) frame types: high nibble of the first byte.
#define ISOTP_SINGLE 0x0
#define ISOTP_FIRST 0x1
#define ISOTP_CONSECUTIVE 0x2
// Our flow control after a first frame: "continue to send", block size 0 (all consecutive frames without another
// flow control), and at least 1 ms between them so a long reply doesn't arrive as one burst.
#define ISOTP_FC_CONTINUE 0x30
#define ISOTP_FC_BLOCK_SIZE 0x00
#define ISOTP_FC_ST_MIN_MS 0x01

static const char *TAG = "uds_client";

// IDs: PSA-RE diag/AEE2010.ev. They never appear in the car's own traffic (checked against trip3 and the test
// captures), so a request cannot collide with an ECU's frame. Padding is 0x00.
// Only ECUs that answered from the OBD port (tx-logs-2, and the ECU scan of 20261120-01) are listed; the BSI, the
// cluster and OBD-II never did (docs/live-view.md, "Transmitting").
// BMS reads: DIDs from PSA-RE TBMU/BMU_CTE1. Values up to 4 bytes fit in a single-frame reply; longer ones come back
// as ISO-TP multi-frame replies. The page decodes them; the label's "ECU:" prefix groups the buttons.
#define REQUEST(req_name, req_label, tx, rx, ...) \
    {.name = req_name, .label = req_label, .tx_id = tx, .rx_id_min = rx, .rx_id_max = rx, .data = {__VA_ARGS__}}
#define BMS(name, label, ...) REQUEST(name, label, 0x6B4, 0x694, __VA_ARGS__)
#define BMS_DID(name, label, did) BMS(name, label, 0x03, 0x22, (uint8_t) ((did) >> 8), (uint8_t) ((did) & 0xFF))
#define BMS_READ(name, label, did) BMS_DID(name, "BMS: " label, did)
#define BMS_EXTRA(name, label, did) BMS_DID(name, "BMS extra: " label, did)
// ReadDTCInformation. Status masks: 0x09 = test failed now or confirmed (what PSA tools list), 0x2D = also pending
// and "failed since the last clear", i.e. the history.
#define BMS_DTC(name, label, ...) BMS(name, "BMS faults: " label, __VA_ARGS__)
// Per fault code: 0x04 snapshot (conditions when it was stored), 0x06 extended data (counters). The page fills in
// the 3-byte DTC at data[3]; record FF = all records. Shown as buttons on the fault's row, not in the button lists.
#define DTC_RECORD(req_name, req_label, tx, rx, subfunction) \
    {.name = req_name, .label = req_label, .tx_id = tx, .rx_id_min = rx, .rx_id_max = rx, \
     .data = {0x06, 0x19, subfunction, 0, 0, 0, 0xFF}, .param_offset = 3, .param_len = 3}
#define BMS_DTC_RECORD(req_name, req_label, subfunction) \
    DTC_RECORD(req_name, "BMS faults: " req_label, 0x6B4, 0x694, subfunction)
// ClearDiagnosticInformation, all groups (at the user's request, 2026-11-23). The page sends the group FFFFFF as the
// parameter, so the request never appears among the plain buttons, only under that ECU's fault list behind a
// confirmation; allowed() accepts nothing but FFFFFF. Clearing deletes the snapshot and extended data with the codes.
#define DTC_CLEAR(req_name, req_label, tx, rx) \
    {.name = req_name, .label = req_label, .tx_id = tx, .rx_id_min = rx, .rx_id_max = rx, \
     .data = {0x04, 0x14, 0, 0, 0}, .param_offset = 2, .param_len = 3, .danger = true}
#define ECU_DTC_CLEAR(prefix, ecu, tx, rx) DTC_CLEAR(prefix "_dtc_clear", "Clear faults: " ecu, tx, rx)
// DiagnosticSessionControl: shown in red on the page.
#define BMS_SESSION(req_name, req_label, session) \
    {.name = req_name, .label = "BMS session: " req_label, .tx_id = 0x6B4, .rx_id_min = 0x694, .rx_id_max = 0x694, \
     .data = {0x02, 0x10, session}, .danger = true}

// The other ECUs that answered the scan (20261120-01), on the scan's address pairs. DIDs from PSA-RE
// diag/AEE2010.ev (OBC_DCDC_ECMP, MCU_ECMP, EPLU), not yet checked against the car. The ECU names in the labels
// are the page's (ECUS in index.html), which decodes the replies by reply ID.
#define DID_READ(name, label, tx, rx, did) \
    REQUEST(name, label, tx, rx, 0x03, 0x22, (uint8_t) ((did) >> 8), (uint8_t) ((did) & 0xFF))
// F080 / F0FE have the same layout on all of them (and on the BMS): part number, supplier; software edition.
#define ECU_IDENT(prefix, ecu, tx, rx) \
    DID_READ(prefix "_f080", "Identification: " ecu " part no.", tx, rx, 0xF080), \
    DID_READ(prefix "_f0fe", "Identification: " ecu " software", tx, rx, 0xF0FE), \
    DID_READ(prefix "_f18c", "Identification: " ecu " serial", tx, rx, 0xF18C)
#define ECU_DTC_COUNT(prefix, ecu, tx, rx) \
    REQUEST(prefix "_dtc_count", "Fault counts: " ecu, tx, rx, 0x03, 0x19, 0x01, 0x09)
// The page names the codes from PSA-RE's list for that ECU (tools/dtc_names.py).
#define ECU_DTC_LIST(prefix, ecu, tx, rx) \
    REQUEST(prefix "_dtc_list", "Fault lists: " ecu, tx, rx, 0x03, 0x19, 0x02, 0x09)
// Not for the park lock: it went silent after the first frame of a multi-frame reply (20261122), and these replies
// are usually multi-frame.
#define ECU_DTC_RECORDS(prefix, ecu, tx, rx) \
    DTC_RECORD(prefix "_dtc_snapshot", "Fault records: " ecu " snapshot", tx, rx, 0x04), \
    DTC_RECORD(prefix "_dtc_extended", "Fault records: " ecu " extended", tx, rx, 0x06)
#define MOTOR(name, label, did) DID_READ(name, "Motor: " label, 0x6A6, 0x686, did)
#define OBC(name, label, did) DID_READ(name, "OBC: " label, 0x590, 0x58F, did)
#define PARK_LOCK(name, label, did) DID_READ(name, "Park lock: " label, 0x5CA, 0x5AA, did)

static const uds_request_t requests[] = {
    REQUEST("vcu_tp", "VCU: TesterPresent", 0x6A2, 0x682, 0x02, 0x3E, 0x00),
    REQUEST("abs_tp", "ABS: TesterPresent", 0x6AD, 0x68D, 0x02, 0x3E, 0x00),
    BMS("bms_tp", "BMS: TesterPresent", 0x02, 0x3E, 0x00),
    BMS_READ("bms_d810", "SoC", 0xD810),
    BMS_READ("bms_d860", "SoH", 0xD860),
    BMS_READ("bms_d413", "Min cell capacity", 0xD413),
    BMS_READ("bms_d865", "Energy", 0xD865),
    BMS_READ("bms_d815", "Pack voltage", 0xD815),
    BMS_READ("bms_d816", "Current", 0xD816),
    BMS_READ("bms_d43d", "Avg cell V", 0xD43D),
    BMS_READ("bms_d86f", "Lowest cell V", 0xD86F),
    BMS_READ("bms_d870", "Highest cell V", 0xD870),
    BMS_READ("bms_d43c", "Lowest cell no.", 0xD43C),
    BMS_READ("bms_d43b", "Highest cell no.", 0xD43B),
    BMS_READ("bms_d877", "Avg temp", 0xD877),
    BMS_READ("bms_d87d", "Min temp", 0xD87D),
    BMS_READ("bms_d817", "Max temp", 0xD817),
    BMS_READ("bms_d878", "Temp spread", 0xD878),
    BMS_READ("bms_d871", "Charge limit 10 s", 0xD871),
    BMS_READ("bms_d873", "Discharge limit 10 s", 0xD873),
    BMS_READ("bms_d811", "Battery status", 0xD811),
    BMS_READ("bms_d86d", "Balancing", 0xD86D),
    BMS_READ("bms_d47a", "Insulation", 0xD47A),
    BMS_READ("bms_d822", "12 V battery", 0xD822),
    BMS_READ("bms_f18b", "Manufacturing date", 0xF18B),
    // Multi-frame replies.
    BMS_READ("bms_d4b5", "Cell SoH (216 B)", 0xD4B5),
    BMS_READ("bms_f080", "ECU identification", 0xF080),
    BMS_READ("bms_f0fe", "Software", 0xF0FE),
    BMS_READ("bms_d901", "Serial number", 0xD901),
    BMS_READ("bms_d465", "D465 (408 B)", 0xD465),

    BMS_EXTRA("bms_d862", "Min charge level", 0xD862),
    BMS_EXTRA("bms_d863", "Max charge level", 0xD863),
    BMS_EXTRA("bms_d410", "SoC sensor calibration", 0xD410),
    BMS_EXTRA("bms_d812", "Why switches opened", 0xD812),
    BMS_EXTRA("bms_d813", "Switch state", 0xD813),
    BMS_EXTRA("bms_d814", "Switch welded?", 0xD814),
    BMS_EXTRA("bms_d86b", "Fuse F1", 0xD86B),
    BMS_EXTRA("bms_d87b", "Insulation +", 0xD87B),
    BMS_EXTRA("bms_d87c", "Insulation -", 0xD87C),
    BMS_EXTRA("bms_d872", "Charge limit 30 s", 0xD872),
    BMS_EXTRA("bms_d874", "Discharge limit 30 s", 0xD874),
    BMS_EXTRA("bms_d876", "Current limit 10 s", 0xD876),
    BMS_EXTRA("bms_d445", "Hottest module", 0xD445),
    BMS_EXTRA("bms_d446", "Coldest module", 0xD446),
    BMS_EXTRA("bms_d49c", "Mileage (BMS)", 0xD49C),
    BMS_EXTRA("bms_d4ca", "Time counter (BMS)", 0xD4CA),
    BMS_EXTRA("bms_d89e", "CAN fault record", 0xD89E),
    BMS_EXTRA("bms_f18c", "F18C", 0xF18C),
    BMS_EXTRA("bms_d900", "D900", 0xD900),
    BMS_EXTRA("bms_e082", "E082", 0xE082),
    BMS_EXTRA("bms_d416", "D416", 0xD416),
    BMS_EXTRA("bms_d417", "D417", 0xD417),
    BMS_EXTRA("bms_d418", "D418", 0xD418),
    BMS_EXTRA("bms_d419", "D419", 0xD419),
    BMS_EXTRA("bms_d41e", "D41E", 0xD41E),
    BMS_EXTRA("bms_d41f", "D41F", 0xD41F),
    BMS_EXTRA("bms_d420", "D420", 0xD420),
    BMS_EXTRA("bms_d421", "D421", 0xD421),
    BMS_EXTRA("bms_d422", "D422", 0xD422),
    BMS_EXTRA("bms_d423", "D423", 0xD423),

    BMS_DTC("bms_dtc_count", "Count", 0x03, 0x19, 0x01, 0x09),
    BMS_DTC("bms_dtc_active", "Active / confirmed", 0x03, 0x19, 0x02, 0x09),
    BMS_DTC("bms_dtc_history", "History", 0x03, 0x19, 0x02, 0x2D),
    BMS_DTC_RECORD("bms_dtc_snapshot", "Snapshot", 0x04),
    BMS_DTC_RECORD("bms_dtc_extended", "Extended data", 0x06),
    ECU_DTC_CLEAR("bms", "BMS", 0x6B4, 0x694),

    // The ECU falls back to the default session ~5 s after the last request (S3 timer) unless TesterPresent keeps
    // the extended one open. Never the programming session (0x02).
    BMS_SESSION("bms_session_default", "Default (10 01)", 0x01),
    BMS_SESSION("bms_session_extended", "Extended (10 03)", 0x03),

    ECU_IDENT("obc", "OBC", 0x590, 0x58F),
    ECU_IDENT("cmel", "Motor", 0x6A6, 0x686),
    ECU_IDENT("vcu", "VCU", 0x6A2, 0x682),
    ECU_IDENT("abs", "ABS", 0x6AD, 0x68D),
    ECU_IDENT("dae", "Steering", 0x6B5, 0x695),
    ECU_IDENT("eplu", "Park lock", 0x5CA, 0x5AA),
    ECU_IDENT("cvm", "Camera", 0x74A, 0x64A),

    ECU_DTC_COUNT("obc", "OBC", 0x590, 0x58F),
    ECU_DTC_COUNT("cmel", "Motor", 0x6A6, 0x686),
    ECU_DTC_COUNT("vcu", "VCU", 0x6A2, 0x682),
    ECU_DTC_COUNT("abs", "ABS", 0x6AD, 0x68D),
    ECU_DTC_COUNT("dae", "Steering", 0x6B5, 0x695),
    ECU_DTC_COUNT("eplu", "Park lock", 0x5CA, 0x5AA),
    ECU_DTC_COUNT("cvm", "Camera", 0x74A, 0x64A),

    ECU_DTC_LIST("obc", "OBC", 0x590, 0x58F),
    ECU_DTC_LIST("cmel", "Motor", 0x6A6, 0x686),
    ECU_DTC_LIST("vcu", "VCU", 0x6A2, 0x682),
    ECU_DTC_LIST("abs", "ABS", 0x6AD, 0x68D),
    ECU_DTC_LIST("dae", "Steering", 0x6B5, 0x695),
    ECU_DTC_LIST("eplu", "Park lock", 0x5CA, 0x5AA),
    ECU_DTC_LIST("cvm", "Camera", 0x74A, 0x64A),

    ECU_DTC_RECORDS("obc", "OBC", 0x590, 0x58F),
    ECU_DTC_RECORDS("cmel", "Motor", 0x6A6, 0x686),
    ECU_DTC_RECORDS("vcu", "VCU", 0x6A2, 0x682),
    ECU_DTC_RECORDS("abs", "ABS", 0x6AD, 0x68D),
    ECU_DTC_RECORDS("dae", "Steering", 0x6B5, 0x695),
    ECU_DTC_RECORDS("cvm", "Camera", 0x74A, 0x64A),

    ECU_DTC_CLEAR("obc", "OBC", 0x590, 0x58F),
    ECU_DTC_CLEAR("cmel", "Motor", 0x6A6, 0x686),
    ECU_DTC_CLEAR("vcu", "VCU", 0x6A2, 0x682),
    ECU_DTC_CLEAR("abs", "ABS", 0x6AD, 0x68D),
    ECU_DTC_CLEAR("dae", "Steering", 0x6B5, 0x695),
    ECU_DTC_CLEAR("eplu", "Park lock", 0x5CA, 0x5AA),
    ECU_DTC_CLEAR("cvm", "Camera", 0x74A, 0x64A),

    // DC05 / DC06 next to the BMS's D816 / D815 tell the traction load from the auxiliaries (the 3A8 energy
    // counter came out ~18 % below 563 x 5C3 on 2026-11-20); DC5B pins down the scale of 208 b5 (torque).
    MOTOR("cmel_dc00", "Speed", 0xDC00),
    MOTOR("cmel_dc5b", "Torque", 0xDC5B),
    MOTOR("cmel_dcb6", "Torque (DCB6)", 0xDCB6),
    MOTOR("cmel_dc69", "Reduced torque", 0xDC69),
    MOTOR("cmel_dc05", "Battery current", 0xDC05),
    MOTOR("cmel_dc06", "Battery voltage", 0xDC06),
    MOTOR("cmel_dc02", "Coolant temp", 0xDC02),
    MOTOR("cmel_dc04", "Inverter temp", 0xDC04),
    MOTOR("cmel_dc10", "Inverter temp (DC10)", 0xDC10),
    MOTOR("cmel_dc70", "Stator temp", 0xDC70),
    MOTOR("cmel_dc71", "Rotor temp", 0xDC71),

    OBC("obc_d824", "DC/DC input voltage", 0xD824),
    OBC("obc_d825", "DC/DC input current", 0xD825),
    OBC("obc_d828", "DC/DC output voltage", 0xD828),
    OBC("obc_d829", "DC/DC output current", 0xD829),
    OBC("obc_d826", "DC/DC temp", 0xD826),
    OBC("obc_d827", "DC/DC status", 0xD827),
    OBC("obc_d822", "12 V battery", 0xD822),
    OBC("obc_d82b", "Mains voltage", 0xD82B),
    OBC("obc_d82c", "Mains current", 0xD82C),
    OBC("obc_d82d", "Charging cable", 0xD82D),
    OBC("obc_d8aa", "Recharge status", 0xD8AA),
    OBC("obc_d8eb", "Charge level", 0xD8EB),

    PARK_LOCK("eplu_d400", "Status", 0xD400),
    PARK_LOCK("eplu_d40c", "Activations", 0xD40C),
    PARK_LOCK("eplu_d40e", "Activation errors", 0xD40E),
    PARK_LOCK("eplu_d404", "Supply voltage", 0xD404),
};
#define REQUEST_COUNT (sizeof(requests) / sizeof(requests[0]))
// Log entries store the request index in a byte, with UDS_CLIENT_SCAN_REQUEST reserved.
_Static_assert(REQUEST_COUNT < UDS_CLIENT_SCAN_REQUEST, "request index no longer fits in uds_log_entry_t");

// Every diagnostic address pair PSA-RE lists for AEE2010.ev (generated from diag/AEE2010.ev/*/*/ecu.yml), what the
// scan tries. Left out: the power tailgate (76B -> 66B), because 66B is in the car's own traffic. Some are for other
// variants (urea, gearbox, engine) and simply stay silent. None of the others appear in the captures so far.
static const uds_ecu_t ecus[] = {
    {"LVNSD", "Low voltage network separation device", 0x49B, 0x49A},
    {"ESM", "Electronic shifter module", 0x58B, 0x58A},
    {"OBC", "On-board charger / DC-DC", 0x590, 0x58F},
    {"LED_DRIVER", "LED headlamp driver", 0x597, 0x557},
    {"BVN", "Night vision", 0x5AC, 0x58C},
    {"AVAS", "Pedestrian warning sound generator", 0x5B0, 0x5AF},
    {"LED_DRIVER", "LED headlamp driver", 0x5B7, 0x577},
    {"EPLU", "Electric park lock", 0x5CA, 0x5AA},
    {"EBB", "Electric brake booster", 0x5D0, 0x5CF},
    {"EDP_C", "Driver door module", 0x620, 0x504},
    {"SCR", "Urea injection (SCR) control unit", 0x6A1, 0x681},
    {"VCU", "Vehicle control unit", 0x6A2, 0x682},
    {"CMEL", "Electric motor control unit", 0x6A6, 0x686},
    {"RMCU", "Rear electric motor control unit", 0x6A7, 0x687},
    {"CMM", "Engine control unit", 0x6A8, 0x688},
    {"BVA", "Gearbox control unit", 0x6A9, 0x689},
    {"SBW", "Shift-by-wire gear selector", 0x6AA, 0x68A},
    {"ABS", "ABS/ESP brake control unit", 0x6AD, 0x68D},
    {"CSS", "Suspension control unit", 0x6B0, 0x690},
    {"TBMU", "Traction battery management", 0x6B4, 0x694},
    {"DAE", "Electric power steering", 0x6B5, 0x695},
    {"ARTIV", "Front radar (distance alert / ACC / AEB)", 0x6B6, 0x696},
    {"AFS/OLVI", "Directional / adaptive headlamps", 0x6B7, 0x697},
    {"LVNSD", "Low voltage network separation device", 0x6BA, 0x69A},
    {"DMTR", "Network voltage stabiliser", 0x6BC, 0x69C},
    {"ASR", "Grip Control traction module (ASR+)", 0x6BD, 0x69D},
    {"BECB", "Battery state sensor", 0x6C4, 0x604},
    {"HDC", "Steering column switch module", 0x6C8, 0x628},
    {"SIR", "Alarm siren", 0x6E5, 0x625},
    {"MUS", "Ultrasonic intrusion module", 0x6E6, 0x626},
    {"CDPL/ESV/TNB", "Rain and light sensor", 0x730, 0x710},
    {"ADDGO/BECH/CTPE", "Diesel additive (FAP) pump", 0x731, 0x711},
    {"BAAS/CCE/FEU_AR", "Hands-free tailgate kick sensor", 0x732, 0x712},
    {"AVP/BDM_C/BPL/B", "Surround view camera", 0x733, 0x713},
    {"CLIM_AV/CRT/EMF", "Climate control", 0x736, 0x716},
    {"CCE", "Electric A/C compressor", 0x737, 0x717},
    {"VTH", "Head-up display", 0x738, 0x718},
    {"BML", "Keyless entry and start module", 0x741, 0x641},
    {"HDC", "Steering column switch module", 0x742, 0x642},
    {"EMF", "Multifunction display", 0x743, 0x643},
    {"RBG", "Airbag control unit", 0x744, 0x644},
    {"BSG", "Generic body control module", 0x748, 0x648},
    {"CVM", "Multifunction windscreen camera", 0x74A, 0x64A},
    {"EDP_C", "Driver door module", 0x74B, 0x64B},
    {"CRT", "Telematics remote control", 0x74C, 0x64C},
    {"AVE/AVP", "Electric steering lock / Surround view camera", 0x74F, 0x64F},
    {"BSI", "Body control module / main gateway", 0x752, 0x652},
    {"MDPLC", "Sliding side door module", 0x755, 0x655},
    {"MDPLC_D", "Right sliding side door module", 0x756, 0x656},
    {"AAS", "Parking sensors", 0x75D, 0x65D},
    {"CMB", "Instrument cluster", 0x75F, 0x65F},
    {"RAD", "Radio", 0x760, 0x660},
    {"IVC_D", "Driver monitoring camera", 0x761, 0x661},
    {"BTA/BTEL", "Standalone telematics unit", 0x764, 0x664},
    {"BSR", "Trailer control module", 0x76C, 0x66C},
    {"CLIM_AV", "Climate control", 0x76D, 0x66D},
    {"VTH", "Head-up display", 0x770, 0x670},
    {"CDC", "CD changer", 0x771, 0x671},
    {"AVP", "Surround view camera", 0x773, 0x673},
    {"CLIM_AR", "Rear climate control", 0x778, 0x678},
    {"FMUX", "Centre console multiplexed controls", 0x77B, 0x67B},
    {"BTA/MDS", "Standalone telematics unit", 0x77C, 0x67C},
    {"AMPLI", "Audio amplifier", 0x77D, 0x67D},
};
#define ECU_COUNT (sizeof(ecus) / sizeof(ecus[0]))
_Static_assert(ECU_COUNT <= UDS_CLIENT_ECU_MAX, "raise UDS_CLIENT_ECU_MAX");

typedef enum {
    STATE_IDLE,
    STATE_SENDING,  // Waiting for on_tx_done.
    STATE_BACKOFF,  // An attempt failed; waiting before the next one.
    STATE_WAITING,  // Sent; collecting replies until the window closes.
} state_t;

// What STATE_SENDING / STATE_BACKOFF are sending.
typedef enum {
    TX_KIND_REQUEST,
    TX_KIND_FLOW_CONTROL,
} tx_kind_t;

typedef enum {
    TX_RESULT_NONE,
    TX_RESULT_OK,
    TX_RESULT_FAILED,
} tx_result_t;

static twai_node_handle_t node;

// Web server -> logger loop, under request_lock: the index and its parameter travel together.
static portMUX_TYPE request_lock = portMUX_INITIALIZER_UNLOCKED;
static int pending = -1;
static uint8_t pending_param[UDS_CLIENT_PARAM_MAX];
static atomic_bool busy;
// ISR -> logger loop.
static atomic_int tx_result;

// Logger loop only.
static state_t state;
static const uds_request_t *current;
static unsigned attempts;
static unsigned replies;
static uint8_t last_reply_service;  // Service byte of the latest single-frame reply (7E, 7F, …).
static bool tx_failed;
static bool final_reply;
static int64_t window_end_us;
static int64_t reply_window_us = REPLY_WINDOW_US;

// ECU scan. Requests from the web server go through atomics; the rest is logger loop only, except the results,
// which the web server copies under the log lock.
static atomic_bool scan_start_requested;
static atomic_bool scan_stop_requested;
static bool scanning;
static size_t scan_next;
static char scan_results[ECU_COUNT + 1];
static uds_request_t scan_request;
static int64_t retry_at_us;
static tx_kind_t tx_kind;
static uint16_t tx_id;
// The current request's frame, with the page's parameter filled in.
static uint8_t request_data[8];
// The driver keeps a pointer to the frame until it is sent.
static twai_frame_t tx_frame;
static uint8_t tx_data[8];

// Multi-frame reply being reassembled (logger loop only).
static bool isotp_active;
static bool flow_control_due;
static uint16_t isotp_len;
static uint16_t isotp_received;
static uint8_t isotp_next_sn;
static uint16_t isotp_id;
static uint8_t isotp_data[UDS_CLIENT_MSG_MAX];
// "<name> <hex>" for events.csv; static, it is too big for the logger loop's stack.
static char message_text[32 + 2 * UDS_CLIENT_MSG_MAX];

// Shared with the web server.
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static uds_log_entry_t log_entries[UDS_CLIENT_LOG_LEN];
static unsigned log_head;
static unsigned log_count;
static uint32_t log_seq;
static uds_message_t message;

// Nothing that writes, resets, unlocks or programs the ECU, except clearing all fault codes (0x14, asked for by the
// user); single frames only. Checked in C on the exact frame
// that goes out (after the page's parameter is filled in), so neither the table nor the page can slip past it.
static bool allowed(uint16_t id, const uint8_t *data) {
    uint8_t len = data[0];
    if (len < 2 || len > 7) {
        return false;
    }
    switch (data[1]) {
    case 0x3E:  // TesterPresent, with or without "suppress positive response".
        return len == 2 && (data[2] == 0x00 || data[2] == 0x80);
    case 0x22:  // ReadDataByIdentifier: one or more 2-byte DIDs.
        return len >= 3 && (len - 1) % 2 == 0;
    case 0x19:  // ReadDTCInformation: count / list by status mask, snapshot / extended data of one DTC.
        if (data[2] == 0x01 || data[2] == 0x02) {
            return len == 3;
        }
        return (data[2] == 0x04 || data[2] == 0x06) && len == 6;
    case 0x14:  // ClearDiagnosticInformation: all groups only (FFFFFF), never one group or one DTC.
        return len == 4 && data[2] == 0xFF && data[3] == 0xFF && data[4] == 0xFF;
    case 0x10:  // DiagnosticSessionControl: default or extended session; never programming (0x02).
        return len == 2 && (data[2] == 0x01 || data[2] == 0x03);
    case 0x01:  // OBD-II current data, functional or physical request.
        return id == 0x7DF || (id >= 0x7E0 && id <= 0x7E7);
    default:
        return false;
    }
}

static void add_log(uds_log_kind_t kind, uint32_t t_ms, uint16_t id, uint16_t len, const uint8_t *data) {
    uds_log_entry_t entry = {
        .t_ms = t_ms,
        .kind = kind,
        .request = current == &scan_request ? UDS_CLIENT_SCAN_REQUEST : (uint8_t) (current - requests),
        .id = id,
        .len = len,
    };
    if (data != NULL) {
        memcpy(entry.data, data, len < sizeof(entry.data) ? len : sizeof(entry.data));
    }
    taskENTER_CRITICAL(&lock);
    log_entries[(log_head + log_count) % UDS_CLIENT_LOG_LEN] = entry;
    if (log_count < UDS_CLIENT_LOG_LEN) {
        log_count++;
    } else {
        log_head = (log_head + 1) % UDS_CLIENT_LOG_LEN;
    }
    log_seq++;
    taskEXIT_CRITICAL(&lock);
}

// events.csv only has room for text; the frame goes in candump notation. Only `len` bytes: the receive buffer holds
// stale bytes from earlier frames past the DLC, and ECUs often send short, unpadded replies.
static void log_frame_event(uint64_t timestamp_us, const char *event, uint16_t id, uint8_t len, const uint8_t *data) {
    char text[24];
    int pos = snprintf(text, sizeof(text), "%03X#", id);
    for (uint8_t i = 0; i < len && i < 8; i++) {
        pos += snprintf(text + pos, sizeof(text) - pos, "%02X", data[i]);
    }
    // Fails when not recording; the page log still has it.
    (void) sd_logger_log_event(timestamp_us, event, text);
}

static void log_text_event(uint64_t timestamp_us, const char *event, const char *what, unsigned n) {
    char text[48];
    snprintf(text, sizeof(text), "%s %u", what, n);
    (void) sd_logger_log_event(timestamp_us, event, text);
}

static void scan_step_done(uint64_t now_us) {
    char result = UDS_SCAN_SILENT;
    if (tx_failed) {
        result = UDS_SCAN_NOT_SENT;
    } else if (replies > 0) {
        result = last_reply_service == UDS_NRC ? UDS_SCAN_NEGATIVE : UDS_SCAN_ANSWERED;
    }
    static const char *const names[] = {
        [UDS_SCAN_ANSWERED] = "answered", [UDS_SCAN_NEGATIVE] = "negative", [UDS_SCAN_SILENT] = "silent",
        [UDS_SCAN_NOT_SENT] = "not_sent",
    };
    char text[48];
    snprintf(text, sizeof(text), "%s %03X %s", ecus[scan_next].node, ecus[scan_next].tx_id, names[(int) result]);
    (void) sd_logger_log_event(now_us, "scan_result", text);
    taskENTER_CRITICAL(&lock);
    scan_results[scan_next] = result;
    log_seq++;
    taskEXIT_CRITICAL(&lock);
    scan_next++;
}

static void scan_end(uint64_t now_us, const char *why) {
    unsigned answered = 0;
    for (size_t i = 0; i < ECU_COUNT; i++) {
        answered += scan_results[i] == UDS_SCAN_ANSWERED || scan_results[i] == UDS_SCAN_NEGATIVE;
    }
    char text[48];
    snprintf(text, sizeof(text), "%s %u/%u answered", why, answered, (unsigned) scan_next);
    (void) sd_logger_log_event(now_us, "scan_done", text);
    taskENTER_CRITICAL(&lock);
    scanning = false;
    log_seq++;
    taskEXIT_CRITICAL(&lock);
}

static void finish(uint64_t now_us) {
    state = STATE_IDLE;
    if (current == &scan_request) {
        scan_step_done(now_us);
        if (scan_next >= ECU_COUNT) {
            scan_end(now_us, "complete");
        }
    }
    if (!scanning) {
        atomic_store(&busy, false);
    }
}

static void fail(int64_t now_us) {
    tx_failed = true;
    add_log(UDS_LOG_TX_FAILED, now_us / 1000, current->tx_id, attempts, NULL);
    log_text_event(now_us, "uds_tx_failed", current->name, attempts);
    finish(now_us);
}

// Sends tx_data (already filled) on tx_id; retries go through STATE_BACKOFF.
static void transmit(int64_t now_us) {
    attempts++;
    atomic_store(&tx_result, TX_RESULT_NONE);
    tx_frame = (twai_frame_t) {
        .header.id = tx_id,
        .buffer = tx_data,
        .buffer_len = sizeof(tx_data),
    };
    window_end_us = now_us + reply_window_us;
    if (twai_node_transmit(node, &tx_frame, 0) != ESP_OK) {
        atomic_store(&tx_result, TX_RESULT_FAILED);
    }
    state = STATE_SENDING;
    add_log(UDS_LOG_TX, now_us / 1000, tx_id, sizeof(tx_data), tx_data);
    log_frame_event(now_us, tx_kind == TX_KIND_REQUEST ? "uds_tx" : "uds_fc", tx_id, sizeof(tx_data), tx_data);
}

static void start_request(int64_t now_us) {
    tx_kind = TX_KIND_REQUEST;
    tx_id = current->tx_id;
    memcpy(tx_data, request_data, sizeof(tx_data));
    attempts = 0;
    transmit(now_us);
}

// Only ever in answer to a first frame from the ECU we just asked, on that request's ID.
static void start_flow_control(int64_t now_us) {
    tx_kind = TX_KIND_FLOW_CONTROL;
    tx_id = current->tx_id;
    memset(tx_data, 0, sizeof(tx_data));
    tx_data[0] = ISOTP_FC_CONTINUE;
    tx_data[1] = ISOTP_FC_BLOCK_SIZE;
    tx_data[2] = ISOTP_FC_ST_MIN_MS;
    attempts = 0;
    transmit(now_us);
}

static void rx_error(uint64_t timestamp_us, uds_rx_error_t error) {
    static const char *const names[] = {
        [UDS_RX_ERROR_SEQUENCE] = "sequence",
        [UDS_RX_ERROR_TOO_LONG] = "too_long",
        [UDS_RX_ERROR_INCOMPLETE] = "incomplete",
    };
    isotp_active = false;
    flow_control_due = false;
    add_log(UDS_LOG_RX_ERROR, timestamp_us / 1000, isotp_id, error, NULL);
    char text[48];
    snprintf(text, sizeof(text), "%s %s %u/%u", current->name, names[error], isotp_received, isotp_len);
    (void) sd_logger_log_event(timestamp_us, "uds_rx_error", text);
}

static void message_complete(uint64_t timestamp_us) {
    isotp_active = false;
    taskENTER_CRITICAL(&lock);
    message.seq++;
    message.t_ms = timestamp_us / 1000;
    message.request = current == &scan_request ? UDS_CLIENT_SCAN_REQUEST : (uint8_t) (current - requests);
    message.id = isotp_id;
    message.len = isotp_len;
    memcpy(message.data, isotp_data, isotp_len);
    taskEXIT_CRITICAL(&lock);
    add_log(UDS_LOG_MESSAGE, timestamp_us / 1000, isotp_id, isotp_len, NULL);

    int pos = snprintf(message_text, sizeof(message_text), "%s ", current->name);
    for (uint16_t i = 0; i < isotp_len; i++) {
        pos += snprintf(message_text + pos, sizeof(message_text) - pos, "%02X", isotp_data[i]);
    }
    (void) sd_logger_log_event(timestamp_us, "uds_msg", message_text);
}

// First frame: 1L LL + 6 payload bytes, where LLL is the total payload length.
static void on_first_frame(const can_log_frame_t *frame) {
    isotp_id = frame->id;
    isotp_len = ((frame->data[0] & 0x0F) << 8) | frame->data[1];
    isotp_received = 0;
    if (frame->dlc < 8 || isotp_len <= 7) {
        return;  // Malformed; a reply this short would have been a single frame.
    }
    if (isotp_len > UDS_CLIENT_MSG_MAX) {
        // Without flow control the ECU gives up by itself (N_Bs timeout).
        rx_error(frame->timestamp_us, UDS_RX_ERROR_TOO_LONG);
        final_reply = true;
        return;
    }
    memcpy(isotp_data, &frame->data[2], 6);
    isotp_received = 6;
    isotp_next_sn = 1;
    isotp_active = true;
    flow_control_due = true;
}

// Consecutive frame: 2N + up to 7 payload bytes, N counting 1..15, 0, 1...
static void on_consecutive_frame(const can_log_frame_t *frame) {
    if (!isotp_active || frame->id != isotp_id) {
        return;
    }
    if ((frame->data[0] & 0x0F) != isotp_next_sn) {
        rx_error(frame->timestamp_us, UDS_RX_ERROR_SEQUENCE);
        final_reply = true;
        return;
    }
    uint16_t n = isotp_len - isotp_received;
    if (n > 7) {
        n = 7;
    }
    if (frame->dlc < n + 1) {
        rx_error(frame->timestamp_us, UDS_RX_ERROR_SEQUENCE);
        final_reply = true;
        return;
    }
    memcpy(&isotp_data[isotp_received], &frame->data[1], n);
    isotp_received += n;
    isotp_next_sn = (isotp_next_sn + 1) & 0x0F;
    // N_Cr: the ECU has up to a second between consecutive frames.
    window_end_us = frame->timestamp_us + reply_window_us;
    if (isotp_received == isotp_len) {
        message_complete(frame->timestamp_us);
        final_reply = true;
    }
}

esp_err_t uds_client_init(twai_node_handle_t twai_node) {
    for (size_t i = 0; i < REQUEST_COUNT; i++) {
        const uds_request_t *r = &requests[i];
        if (!allowed(r->tx_id, r->data) || r->param_len > UDS_CLIENT_PARAM_MAX ||
            r->param_offset + r->param_len > 1 + r->data[0]) {
            ESP_LOGE(TAG, "request %s is not on the allowlist", requests[i].name);
            return ESP_ERR_INVALID_ARG;
        }
    }
    node = twai_node;
    return ESP_OK;
}

void uds_client_on_frame(const can_log_frame_t *frame) {
    if (state == STATE_IDLE || frame->extended || frame->remote || frame->dlc == 0) {
        return;
    }
    if (frame->id < current->rx_id_min || frame->id > current->rx_id_max) {
        return;
    }
    // Consecutive frames are not logged one by one (a 216-byte reply has 31); the complete message is.
    uint8_t type = frame->data[0] >> 4;
    if (type == ISOTP_CONSECUTIVE) {
        on_consecutive_frame(frame);
        return;
    }
    if (type != ISOTP_SINGLE && type != ISOTP_FIRST) {
        return;
    }
    replies++;
    last_reply_service = type == ISOTP_SINGLE ? frame->data[1] : frame->data[2];
    add_log(UDS_LOG_RX, frame->timestamp_us / 1000, frame->id, frame->dlc, frame->data);
    log_frame_event(frame->timestamp_us, "uds_rx", frame->id, frame->dlc, frame->data);
    if (type == ISOTP_FIRST) {
        on_first_frame(frame);
        return;
    }
    // A physical request has one answer, unless the ECU says it needs more time. Functional ones wait for everybody.
    bool pending_reply = frame->dlc >= 4 && frame->data[1] == UDS_NRC && frame->data[3] == UDS_NRC_RESPONSE_PENDING;
    if (current->rx_id_min == current->rx_id_max && !pending_reply) {
        final_reply = true;
    }
}

void uds_client_poll(int64_t now_us, bool bus_ok) {
    switch (state) {
    case STATE_IDLE: {
        if (atomic_exchange(&scan_stop_requested, false) && scanning) {
            scan_end(now_us, "stopped");
            atomic_store(&busy, false);
        }
        if (atomic_exchange(&scan_start_requested, false) && !scanning) {
            taskENTER_CRITICAL(&lock);
            memset(scan_results, UDS_SCAN_NOT_TRIED, ECU_COUNT);
            scan_results[ECU_COUNT] = '\0';
            scanning = true;
            log_seq++;
            taskEXIT_CRITICAL(&lock);
            scan_next = 0;
            (void) sd_logger_log_event(now_us, "scan_start", "TesterPresent to every PSA-RE address");
        }
        if (scanning) {
            const uds_ecu_t *ecu = &ecus[scan_next];
            scan_request = (uds_request_t) {
                .name = ecu->node,
                .label = ecu->label,
                .tx_id = ecu->tx_id,
                .rx_id_min = ecu->rx_id,
                .rx_id_max = ecu->rx_id,
                .data = {0x02, 0x3E, 0x00},
            };
            current = &scan_request;
            memcpy(request_data, current->data, sizeof(request_data));
            replies = 0;
            tx_failed = false;
            final_reply = false;
            isotp_active = false;
            flow_control_due = false;
            reply_window_us = SCAN_WINDOW_US;
            if (!bus_ok || !allowed(current->tx_id, request_data)) {
                add_log(UDS_LOG_REFUSED, now_us / 1000, current->tx_id, 0, NULL);
                log_text_event(now_us, "uds_refused", current->name, 0);
                scan_end(now_us, "refused");
                atomic_store(&busy, false);
                return;
            }
            start_request(now_us);
            return;
        }
        uint8_t param[UDS_CLIENT_PARAM_MAX];
        taskENTER_CRITICAL(&request_lock);
        int index = pending;
        pending = -1;
        memcpy(param, pending_param, sizeof(param));
        if (index >= 0) {
            atomic_store(&busy, true);
        }
        taskEXIT_CRITICAL(&request_lock);
        if (index < 0) {
            return;
        }
        current = &requests[index];
        memcpy(request_data, current->data, sizeof(request_data));
        memcpy(&request_data[current->param_offset], param, current->param_len);
        replies = 0;
        tx_failed = false;
        final_reply = false;
        isotp_active = false;
        flow_control_due = false;
        reply_window_us = REPLY_WINDOW_US;
        // An error-passive or bus-off node has been disturbing the bus already; don't add to it.
        if (!bus_ok || !allowed(current->tx_id, request_data)) {
            add_log(UDS_LOG_REFUSED, now_us / 1000, current->tx_id, 0, NULL);
            log_text_event(now_us, "uds_refused", current->name, 0);
            finish(now_us);
            return;
        }
        start_request(now_us);
        return;
    }
    case STATE_SENDING: {
        int result = atomic_load(&tx_result);
        if (result == TX_RESULT_OK) {
            state = STATE_WAITING;
        } else if (result == TX_RESULT_FAILED || now_us >= window_end_us) {
            if (attempts < MAX_ATTEMPTS) {
                state = STATE_BACKOFF;
                retry_at_us = now_us + RETRY_DELAY_US;
            } else {
                fail(now_us);
            }
        }
        return;
    }
    case STATE_BACKOFF:
        if (now_us < retry_at_us) {
            return;
        }
        if (bus_ok) {
            transmit(now_us);
        } else {
            fail(now_us);
        }
        return;
    case STATE_WAITING:
        if (flow_control_due && !final_reply) {
            flow_control_due = false;
            if (bus_ok) {
                start_flow_control(now_us);
            } else {
                fail(now_us);
            }
            return;
        }
        if (isotp_active && now_us >= window_end_us) {
            rx_error(now_us, UDS_RX_ERROR_INCOMPLETE);
        }
        if (final_reply || now_us >= window_end_us) {
            add_log(UDS_LOG_DONE, now_us / 1000, current->tx_id, replies, NULL);
            log_text_event(now_us, "uds_done", current->name, replies);
            finish(now_us);
        }
        return;
    }
}

void IRAM_ATTR uds_client_on_tx_done_from_isr(bool success) {
    atomic_store(&tx_result, success ? TX_RESULT_OK : TX_RESULT_FAILED);
}

esp_err_t uds_client_request(const char *name, const uint8_t *param, size_t param_len) {
    for (size_t i = 0; i < REQUEST_COUNT; i++) {
        if (strcmp(requests[i].name, name) != 0) {
            continue;
        }
        if (param_len != requests[i].param_len) {
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t err = ESP_OK;
        taskENTER_CRITICAL(&request_lock);
        if (atomic_load(&busy) || pending >= 0) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            pending = (int) i;
            if (param_len > 0) {
                memcpy(pending_param, param, param_len);
            }
        }
        taskEXIT_CRITICAL(&request_lock);
        return err;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t uds_client_scan_start(void) {
    if (atomic_load(&busy)) {
        return ESP_ERR_INVALID_STATE;
    }
    // busy at once, so a request from the page cannot slip in before the logger loop picks the scan up.
    atomic_store(&busy, true);
    atomic_store(&scan_start_requested, true);
    return ESP_OK;
}

void uds_client_scan_stop(void) {
    atomic_store(&scan_stop_requested, true);
}

size_t uds_client_ecu_count(void) {
    return ECU_COUNT;
}

const uds_ecu_t *uds_client_get_ecu(size_t index) {
    return index < ECU_COUNT ? &ecus[index] : NULL;
}

bool uds_client_get_scan(char *out, size_t size, size_t *next) {
    taskENTER_CRITICAL(&lock);
    bool active = scanning;
    *next = scan_next;
    if (scan_results[0] == '\0') {
        memset(scan_results, UDS_SCAN_NOT_TRIED, ECU_COUNT);
    }
    strlcpy(out, scan_results, size);
    taskEXIT_CRITICAL(&lock);
    return active;
}

const char *uds_client_request_name(uint8_t request) {
    if (request == UDS_CLIENT_SCAN_REQUEST) {
        return "scan";
    }
    return request < REQUEST_COUNT ? requests[request].name : "?";
}

size_t uds_client_request_count(void) {
    return REQUEST_COUNT;
}

const uds_request_t *uds_client_get_request(size_t index) {
    return index < REQUEST_COUNT ? &requests[index] : NULL;
}

void uds_client_get_message(uds_message_t *out) {
    taskENTER_CRITICAL(&lock);
    *out = message;
    taskEXIT_CRITICAL(&lock);
}

size_t uds_client_get_log(uds_log_entry_t *out, size_t max, uint32_t *seq) {
    size_t n = 0;
    taskENTER_CRITICAL(&lock);
    for (; n < log_count && n < max; n++) {
        out[n] = log_entries[(log_head + n) % UDS_CLIENT_LOG_LEN];
    }
    *seq = log_seq;
    taskEXIT_CRITICAL(&lock);
    return n;
}
