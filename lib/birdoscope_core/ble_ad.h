// ble_ad.h - BLE advertisement decoding.
//
// Uses no NimBLE types, so the host tests can build it. Callers pass the raw
// AD payload bytes from whichever stack they use. Every `mfrData` argument is
// the manufacturer-specific data field without its AD length and type header.
//
// See decision_record.md D12.

#pragma once

#include <stddef.h>
#include <stdint.h>

// HCI address type. The enum omits the static, RPA and NRPA split, which the
// analysis pipeline derives from the address bytes.
enum BleAddrType : uint8_t {
  BLE_ADDR_PUBLIC = 0,
  BLE_ADDR_RANDOM = 1,
  BLE_ADDR_PUBLIC_ID = 2,
  BLE_ADDR_RANDOM_ID = 3,
};

// Returns "public", "random", or "" when unrecognised, matching the roost
// ble_addr_type vocabulary for roostRowSetEnumByName. Identity-resolved types
// map to the same two names, because the column records the advertiser's
// address space.
const char *bleAddrTypeName(uint8_t hciType);

#define BLE_AD_MANUFACTURER  0xFF   // AD type for manufacturer-specific data

#define BLE_COMPANY_AXON     0x034D   // 9-char serial follows a type byte
#define BLE_AXON_TYPE_BYTE   0x02
#define BLE_COMPANY_XUNTONG  0x09C8   // Flock Penguin battery pack
#define BLE_COMPANY_AXIS     0x0D5B   // Axis Communications AB
#define BLE_COMPANY_MOTOROLA 0x04EC   // Motorola Solutions

// Bluetooth SIG company identifier, the first two bytes of manufacturer data,
// little-endian. Returns -1 when the field is too short to hold one.
int bleMfrCompanyId(const uint8_t* mfrData, size_t len);

// Copies `idLen` bytes at `idOffset` into `out` as a NUL-terminated string,
// requiring every byte to be printable ASCII. Returns the length copied, or 0
// on a short field or a non-printable byte. `cap` must exceed `idLen`.
//
// A company id plus a type byte is weak evidence alone, and the printability
// check cuts false positives.
size_t bleMfrAsciiId(const uint8_t* mfrData, size_t len,
                     uint8_t idOffset, uint8_t idLen, char* out, size_t cap);

// Extracts the 9-character ASCII serial that Axon BLE devices send in
// manufacturer data, which begins 4D 03 02 with the serial in bytes 3-11. The
// serial survives MAC randomisation. Keep in step with the Axon row of
// ble_mfr_rules in core.cpp, which matches the same payload.
//
// Returns the serial length (9) on a match, 0 otherwise. `out` must hold at
// least 10 bytes.
size_t bleAxonSerial(const uint8_t *mfrData, size_t len, char *out, size_t cap);

// One AD structure in a raw advertisement payload, laid out as
// [len][type][data...] on air, where len covers the type byte plus data.
struct BleAdStructure {
  uint8_t type;
  const uint8_t *data;
  uint8_t len;  // data length, excluding the type byte
};

// Reads the structure at *cursor and advances past it. Pass *cursor = 0 to
// start. Returns false at the end or on a malformed length, and never reads
// past `payloadLen`.
bool bleNextAdStructure(const uint8_t *payload, size_t payloadLen, size_t *cursor,
                        BleAdStructure *out);

// Finds the first structure of one AD type and returns a pointer to its data,
// or nullptr.
const uint8_t *bleFindAdType(const uint8_t *payload, size_t payloadLen,
                             uint8_t adType, uint8_t *outLen);

#define BLE_AD_NAME_SHORT    0x08
#define BLE_AD_NAME_COMPLETE 0x09

// Returns the advertised local name, preferring the complete name over the
// shortened one, or nullptr. The name has no terminator and may hold 0x00, so
// callers write `*outLen` bytes of it.
const uint8_t *bleLocalName(const uint8_t *payload, size_t payloadLen,
                            uint8_t *outLen);
