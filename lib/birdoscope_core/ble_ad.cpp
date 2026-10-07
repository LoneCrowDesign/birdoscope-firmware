#include "ble_ad.h"

#include <string.h>

const char *bleAddrTypeName(uint8_t hciType) {
  switch (hciType) {
    case BLE_ADDR_PUBLIC:
    case BLE_ADDR_PUBLIC_ID:
      return "public";
    case BLE_ADDR_RANDOM:
    case BLE_ADDR_RANDOM_ID:
      return "random";
    default:
      return "";
  }
}

bool bleNextAdStructure(const uint8_t *payload, size_t payloadLen, size_t *cursor,
                        BleAdStructure *out) {
  if (!payload || !cursor || !out) return false;

  const size_t i = *cursor;
  if (i >= payloadLen) return false;

  const uint8_t len = payload[i];
  // A zero length ends the significant data.
  if (len == 0) return false;
  if (i + 1 + len > payloadLen) return false;

  out->type = payload[i + 1];
  out->len = len - 1;
  out->data = out->len ? &payload[i + 2] : nullptr;
  *cursor = i + 1 + len;
  return true;
}

const uint8_t *bleFindAdType(const uint8_t *payload, size_t payloadLen,
                             uint8_t adType, uint8_t *outLen) {
  size_t cursor = 0;
  BleAdStructure ad;
  while (bleNextAdStructure(payload, payloadLen, &cursor, &ad)) {
    if (ad.type == adType) {
      if (outLen) *outLen = ad.len;
      return ad.data;
    }
  }
  if (outLen) *outLen = 0;
  return nullptr;
}

const uint8_t *bleLocalName(const uint8_t *payload, size_t payloadLen,
                            uint8_t *outLen) {
  const uint8_t *name =
      bleFindAdType(payload, payloadLen, BLE_AD_NAME_COMPLETE, outLen);
  if (name) return name;
  return bleFindAdType(payload, payloadLen, BLE_AD_NAME_SHORT, outLen);
}

int bleMfrCompanyId(const uint8_t *mfrData, size_t len) {
  if (!mfrData || len < 2) return -1;
  return (int)((uint16_t)mfrData[0] | ((uint16_t)mfrData[1] << 8));
}

size_t bleMfrAsciiId(const uint8_t *mfrData, size_t len,
                     uint8_t idOffset, uint8_t idLen, char *out, size_t cap) {
  if (!out || cap <= (size_t)idLen) return 0;
  out[0] = '\0';
  if (!mfrData || idLen == 0 || len < (size_t)idOffset + idLen) return 0;

  for (uint8_t i = 0; i < idLen; i++) {
    const uint8_t c = mfrData[idOffset + i];
    if (c < 0x20 || c > 0x7E) { out[0] = '\0'; return 0; }
    out[i] = (char)c;
  }
  out[idLen] = '\0';
  return idLen;
}

size_t bleAxonSerial(const uint8_t *mfrData, size_t len, char *out, size_t cap) {
  if (!out || cap < 10) return 0;
  out[0] = '\0';
  if (bleMfrCompanyId(mfrData, len) != BLE_COMPANY_AXON) return 0;
  if (len < 3 || mfrData[2] != BLE_AXON_TYPE_BYTE) return 0;
  return bleMfrAsciiId(mfrData, len, 3, 9, out, cap);
}
