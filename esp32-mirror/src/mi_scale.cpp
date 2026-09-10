#include "mi_scale.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <math.h>
#include <string.h>

#include "config.h"
#include "mi_crypto.h"

namespace MiScale {

// ── Characteristic UUIDs (16-bit under the BT base) ─────────────────────────
static const uint16_t UUID_SERVICE = 0xFE95;
static const uint16_t UUID_UPNP = 0x0010;      // write CMD_LOGIN
static const uint16_t UUID_AVDTP = 0x0019;     // auth channel (write + notify)
static const uint16_t UUID_DATA_RX = 0x001a;   // send encrypted commands
static const uint16_t UUID_DATA_RX2 = 0x001b;  // receive encrypted data
static const uint16_t UUID_DEV_INFO = 0x001c;  // pre-auth device info
static const uint16_t UUID_FW = 0x0004;        // firmware version (read)

// ── Protocol constants ──────────────────────────────────────────────────────
static const uint8_t CMD_LOGIN[] = {0x24, 0x00, 0x00, 0x00};
static const uint8_t CMD_SEND_KEY[] = {0x00, 0x00, 0x00, 0x0b, 0x01, 0x00};
static const uint8_t CMD_SEND_INFO[] = {0x00, 0x00, 0x00, 0x0a, 0x01, 0x00};
static const uint8_t RCV_RDY[] = {0x00, 0x00, 0x01, 0x01};
static const uint8_t RCV_OK[] = {0x00, 0x00, 0x01, 0x00};
static const uint8_t RCV_ACK[] = {0x00, 0x00, 0x03, 0x00};
static const uint8_t HEADER6[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
static const uint8_t PING[] = {0x00, 0x00, 0x01, 0x05, 0x01, 0x00};
static const uint8_t PONG[] = {0x00, 0x00, 0x05, 0x04, 0x01, 0x00};

static const uint8_t PAYLOAD_CAP[] = {0x05, 0x20, 0x02, 0x00, 0xf0};
static const uint8_t PAYLOAD_PROP[] = {0x0c, 0x20, 0x04, 0x00, 0x00, 0x01,
                                       0x05, 0x01, 0x00, 0x01, 0x10, 0x05};

// ── Notification plumbing ───────────────────────────────────────────────────
struct Notif {
  uint16_t len;
  uint8_t buf[256];
};

static QueueHandle_t qAvdtp, qDataRx, qDataRx2, qDevInfo, qUpnp;

static NimBLEClient *client = nullptr;
static NimBLERemoteCharacteristic *chUpnp, *chAvdtp, *chDataRx, *chDataRx2,
    *chDevInfo, *chFw;

static WeightCallback weightCb;
static StatusCallback statusCb;
static volatile bool scaleSeen = false;
static volatile uint32_t s_heartbeat = 0;   // BLE task liveness (for the watchdog)

static void status(const char *s) {
  Serial.printf("[ble] %s\n", s);
  if (statusCb) statusCb(s);
}

static QueueHandle_t queueForUuid(const NimBLEUUID &u) {
  if (u == NimBLEUUID(UUID_AVDTP)) return qAvdtp;
  if (u == NimBLEUUID(UUID_DATA_RX)) return qDataRx;
  if (u == NimBLEUUID(UUID_DATA_RX2)) return qDataRx2;
  if (u == NimBLEUUID(UUID_DEV_INFO)) return qDevInfo;
  if (u == NimBLEUUID(UUID_UPNP)) return qUpnp;
  return nullptr;
}

static void onNotify(NimBLERemoteCharacteristic *c, uint8_t *data, size_t len,
                     bool isNotify) {
  QueueHandle_t q = queueForUuid(c->getUUID());
  if (!q) return;
  Notif n;
  n.len = len > sizeof(n.buf) ? sizeof(n.buf) : (uint16_t)len;
  memcpy(n.buf, data, n.len);
  xQueueSend(q, &n, 0);
}

static void drainQueue(QueueHandle_t q) {
  Notif n;
  while (xQueueReceive(q, &n, 0) == pdTRUE) {
  }
}

// Receive next notification (any) with timeout.
static bool qRecv(QueueHandle_t q, Notif &out, uint32_t timeoutMs) {
  return xQueueReceive(q, &out, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

// Wait until a notification starts with prefix; non-matching ones are discarded
// (matches the lock-step semantics of the reference implementation).
static bool waitPrefix(QueueHandle_t q, const uint8_t *pfx, size_t pfxLen,
                       uint32_t timeoutMs, Notif *out = nullptr) {
  uint32_t deadline = millis() + timeoutMs;
  Notif n;
  for (;;) {
    int32_t remaining = (int32_t)(deadline - millis());
    if (remaining <= 0) return false;
    if (!qRecv(q, n, (uint32_t)remaining)) return false;
    if (n.len >= pfxLen && memcmp(n.buf, pfx, pfxLen) == 0) {
      if (out) *out = n;
      return true;
    }
  }
}

// ── Small write helpers (all Mi chars are write-without-response) ────────────
static bool writeC(NimBLERemoteCharacteristic *c, const uint8_t *d, size_t len) {
  if (!c) return false;
  return c->writeValue(d, len, false);
}

// Send data as numbered parcel frames: [i+1, 0x00] + chunk (<=200 B chunks).
static void sendParcel(NimBLERemoteCharacteristic *c, const uint8_t *data,
                       size_t len) {
  const size_t CHUNK = 200;
  size_t i = 0;
  for (size_t off = 0; off < len; off += CHUNK, i++) {
    size_t n = (len - off) < CHUNK ? (len - off) : CHUNK;
    uint8_t frame[202];
    frame[0] = (uint8_t)(i + 1);
    frame[1] = 0x00;
    memcpy(frame + 2, data + off, n);
    writeC(c, frame, n + 2);
    delay(10);
  }
}

// Receive a parcel on AVDTP for the given cmd byte (single- or multi-frame).
// Returns payload length, or -1 on timeout.
static int receiveParcel(uint8_t cmdByte, uint8_t *out, size_t outCap,
                         uint32_t timeoutMs) {
  uint8_t singlePfx[4] = {0x00, 0x00, 0x02, cmdByte};
  uint8_t multiPfx[4] = {0x00, 0x00, 0x00, cmdByte};

  uint32_t deadline = millis() + timeoutMs;
  Notif n;
  bool haveHeader = false;
  bool isSingle = false;
  for (;;) {
    int32_t remaining = (int32_t)(deadline - millis());
    if (remaining <= 0) return -1;
    if (!qRecv(qAvdtp, n, (uint32_t)remaining)) return -1;
    if (n.len >= 4 && memcmp(n.buf, singlePfx, 4) == 0) {
      isSingle = true;
      haveHeader = true;
      break;
    }
    if (n.len >= 4 && memcmp(n.buf, multiPfx, 4) == 0) {
      isSingle = false;
      haveHeader = true;
      break;
    }
    // else discard
  }
  if (!haveHeader) return -1;

  if (isSingle) {
    writeC(chAvdtp, RCV_ACK, sizeof(RCV_ACK));
    size_t plen = n.len - 4;
    if (plen > outCap) plen = outCap;
    memcpy(out, n.buf + 4, plen);
    return (int)plen;
  }

  // multi-frame: header [00 00 00 CMD n_lo n_hi]
  uint16_t nFrames = n.buf[4] | (n.buf[5] << 8);
  writeC(chAvdtp, RCV_RDY, sizeof(RCV_RDY));
  size_t off = 0;
  for (uint16_t i = 0; i < nFrames; i++) {
    if (!qRecv(qAvdtp, n, 5000)) return -1;
    size_t part = n.len >= 2 ? n.len - 2 : 0;  // frame = [frm_lo frm_hi data...]
    if (off + part > outCap) part = outCap - off;
    memcpy(out + off, n.buf + 2, part);
    off += part;
  }
  writeC(chAvdtp, RCV_OK, sizeof(RCV_OK));
  return (int)off;
}

// Receive one logical DATA_RX2 message (single- or multi-frame).
// Returns ciphertext length (incl 4-byte tag) or -1; sets *devSeq.
static int receiveDataRx2(uint32_t *devSeq, uint8_t *out, size_t outCap,
                          uint32_t timeoutMs) {
  Notif n;
  if (!qRecv(qDataRx2, n, timeoutMs)) return -1;

  if (n.len >= 4 && n.buf[2] == 0x02) {
    // single-frame: [00 00 02 00 seq_lo seq_hi ct...]
    *devSeq = n.buf[4] | (n.buf[5] << 8);
    writeC(chDataRx2, RCV_ACK, sizeof(RCV_ACK));
    size_t ctlen = n.len >= 6 ? n.len - 6 : 0;
    if (ctlen > outCap) ctlen = outCap;
    memcpy(out, n.buf + 6, ctlen);
    return (int)ctlen;
  }

  // multi-frame header: [00 00 00 00 n_lo n_hi]
  uint16_t nFrames = (n.len >= 6) ? (n.buf[4] | (n.buf[5] << 8)) : 0;
  writeC(chDataRx2, RCV_RDY, sizeof(RCV_RDY));
  size_t off = 0;
  *devSeq = 0;
  for (uint16_t i = 0; i < nFrames; i++) {
    if (!qRecv(qDataRx2, n, 5000)) return -1;
    if (i == 0) {
      *devSeq = (n.len >= 4) ? (n.buf[2] | (n.buf[3] << 8)) : 0;
      size_t part = n.len >= 4 ? n.len - 4 : 0;
      if (off + part > outCap) part = outCap - off;
      memcpy(out + off, n.buf + 4, part);
      off += part;
    } else {
      size_t part = n.len >= 2 ? n.len - 2 : 0;
      if (off + part > outCap) part = outCap - off;
      memcpy(out + off, n.buf + 2, part);
      off += part;
    }
  }
  writeC(chDataRx2, RCV_OK, sizeof(RCV_OK));
  return (int)off;
}

// ── Session keys ────────────────────────────────────────────────────────────
struct SessionKeys {
  uint8_t dev_key[16], app_key[16], dev_iv[4], app_iv[4];
  uint8_t salt[32], salt_inv[32];
};

static bool deriveKeys(const uint8_t *randomKey, const uint8_t *remoteKey,
                       SessionKeys &k) {
  memcpy(k.salt, randomKey, 16);
  memcpy(k.salt + 16, remoteKey, 16);
  memcpy(k.salt_inv, remoteKey, 16);
  memcpy(k.salt_inv + 16, randomKey, 16);
  uint8_t derived[64];
  const uint8_t info[] = {'m', 'i', 'b', 'l', 'e', '-', 'l', 'o',
                          'g', 'i', 'n', '-', 'i', 'n', 'f', 'o'};
  if (!hkdf_sha256(MI_TOKEN, sizeof(MI_TOKEN), k.salt, 32, info, sizeof(info),
                   derived, 64))
    return false;
  memcpy(k.dev_key, derived + 0, 16);
  memcpy(k.app_key, derived + 16, 16);
  memcpy(k.dev_iv, derived + 32, 4);
  memcpy(k.app_iv, derived + 36, 4);
  return true;
}

// ── Weight frame parsing ────────────────────────────────────────────────────
// This scale streams live weight in 0x19 frames (LE uint16 @ off 23, /100) and
// does NOT send a 0x22 stable frame, so we detect "stable" ourselves: the value
// holding steady (within tol) for N consecutive frames above a min load.
static const float STABLE_TOL_KG = 0.3f;
static const int STABLE_FRAMES = 5;
static const float MIN_LOAD_KG = 3.0f;

// Per-stand state: we keep a persistent connection, so we record exactly ONE
// reading per physical stand and re-arm when the person steps off.
static float stLast = -999;
static int stCount = 0;
static bool stLocked = false;
static bool recordedThisLoad = false;
static volatile uint32_t lastLoadMs = 0;    // last time someone was on the scale

static void resetStability() {
  stLast = -999;
  stCount = 0;
  stLocked = false;
  recordedThisLoad = false;
}

// Emit one stable reading per stand.
static void tryRecord(float kg, const char *csv, int imp) {
  if (recordedThisLoad) return;
  recordedThisLoad = true;
  if (weightCb) weightCb(kg, true, csv, imp);
}

static void handleWeightPlain(const uint8_t *pt, size_t len) {
  if (len < 1) return;
  uint8_t type = pt[0];

  if (type == 0x19 && len >= 25) {
    uint16_t raw = pt[23] | (pt[24] << 8);
    float kg = raw / 100.0f;
    if (kg < MIN_LOAD_KG) {  // nobody on the scale → arm for the next weigh-in
      resetStability();
      return;
    }
    lastLoadMs = millis();
    if (weightCb) weightCb(kg, false, nullptr, 0);  // live

    if (!stLocked) {
      if (stLast > 0 && fabsf(kg - stLast) <= STABLE_TOL_KG)
        stCount++;
      else
        stCount = 0;
      stLast = kg;
      if (stCount >= STABLE_FRAMES) {
        stLocked = true;
        tryRecord(kg, nullptr, 0);  // heuristic lock (fallback if no 0x23)
      }
    }
    return;
  }

  if ((type == 0x22 || type == 0x2b || type == 0x23) && len >= 14) {
    // Final measurement. CSV after the 0xa0 marker:
    //   id,user,weight_10g,impedance,ts   (weight_10g/100 = kg)
    int a0 = -1;
    for (size_t i = 0; i < len; i++) {
      if (pt[i] == 0xa0) {
        a0 = (int)i;
        break;
      }
    }
    if (a0 < 0) return;
    char csv[160];
    size_t clen = len - (a0 + 1);
    if (clen >= sizeof(csv)) clen = sizeof(csv) - 1;
    memcpy(csv, pt + a0 + 1, clen);
    csv[clen] = 0;

    char work[160];
    strncpy(work, csv, sizeof(work));
    work[sizeof(work) - 1] = 0;
    char *save = nullptr;
    char *tok = strtok_r(work, ",", &save);
    int idx = 0;
    long weight10g = -1;
    int impedance = 0;
    while (tok) {
      if (idx == 2) weight10g = atol(tok);
      if (idx == 3) impedance = atoi(tok);
      tok = strtok_r(nullptr, ",", &save);
      idx++;
    }
    if (weight10g >= 0) {
      lastLoadMs = millis();
      tryRecord(weight10g / 100.0f, csv, impedance);  // authoritative final
    }
  }
}

// ── One full session: connect → auth → exchanges → stream ───────────────────
static bool runAuth(SessionKeys &keys) {
  // Pre-auth: subscribe DATA_RX2, DATA_RX, DEV_INFO (order per capture).
  drainQueue(qAvdtp);
  drainQueue(qDataRx);
  drainQueue(qDataRx2);
  drainQueue(qDevInfo);
  drainQueue(qUpnp);

  if (!chDataRx2->subscribe(true, onNotify)) return false;
  if (!chDataRx->subscribe(true, onNotify)) return false;
  if (chDevInfo) chDevInfo->subscribe(true, onNotify);

  // Device-info query (Mi Home does this before auth). Drain replies.
  const uint8_t di0[] = {0x00};
  const uint8_t di1[] = {0x01};
  const uint8_t di2[] = {0x08, 0x01, 0x00};
  const uint8_t di3[] = {0x03};
  const uint8_t *dis[] = {di0, di1, di2, di3};
  const size_t disl[] = {1, 1, 3, 1};
  for (int i = 0; i < 4; i++) {
    if (chDevInfo) {
      writeC(chDevInfo, dis[i], disl[i]);
      Notif tmp;
      qRecv(qDevInfo, tmp, 300);  // best-effort drain (replies come fast)
    }
  }

  if (!chAvdtp->subscribe(true, onNotify)) return false;
  delay(100);

  // Step 1: init + (best-effort) MTU exchange on AVDTP.
  writeC(chUpnp, (const uint8_t[]){0xa4}, 1);
  {
    const uint8_t p400[] = {0x00, 0x00, 0x04, 0x00};
    Notif mtu;
    if (waitPrefix(qAvdtp, p400, 4, 1200, &mtu)) {
      Serial.printf("[auth] app-MTU frame len=%d\n", mtu.len);
      uint8_t ack[256];
      ack[0] = 0x00; ack[1] = 0x00; ack[2] = 0x05; ack[3] = 0x00;
      memcpy(ack + 4, mtu.buf + 4, mtu.len - 4);
      writeC(chAvdtp, ack, mtu.len);
      const uint8_t p401[] = {0x00, 0x00, 0x04, 0x01};
      Notif big;
      if (waitPrefix(qAvdtp, p401, 4, 1200, &big)) {
        Serial.printf("[auth] app-MTU big frame len=%d\n", big.len);
        uint8_t mirror[256];
        mirror[0] = 0x00; mirror[1] = 0x00; mirror[2] = 0x05; mirror[3] = 0x01;
        memcpy(mirror + 4, big.buf + 4, big.len - 4);
        size_t mlen = big.len;
        for (size_t o = 0; o < mlen; o += 20) {
          size_t n = (mlen - o) < 20 ? (mlen - o) : 20;
          writeC(chAvdtp, mirror + o, n);
          delay(8);
        }
      }
    }
  }

  if (chUpnp) chUpnp->subscribe(true, onNotify);
  delay(100);

  // Step 2: CMD_LOGIN
  writeC(chUpnp, CMD_LOGIN, sizeof(CMD_LOGIN));

  // Step 3: CMD_SEND_KEY, wait RCV_RDY
  writeC(chAvdtp, CMD_SEND_KEY, sizeof(CMD_SEND_KEY));
  if (!waitPrefix(qAvdtp, RCV_RDY, sizeof(RCV_RDY), 5000)) {
    Serial.println("[auth] no RCV_RDY for key");
    return false;
  }

  // Step 4: send our 16-byte random key
  uint8_t randomKey[16];
  for (int i = 0; i < 16; i += 4) {
    uint32_t r = esp_random();
    memcpy(randomKey + i, &r, 4);
  }
  sendParcel(chAvdtp, randomKey, 16);
  if (!waitPrefix(qAvdtp, RCV_OK, sizeof(RCV_OK), 5000)) {
    Serial.println("[auth] no RCV_OK after key");
    return false;
  }

  // Step 5: receive remote_key [00 00 02 0d ...]
  const uint8_t p020d[] = {0x00, 0x00, 0x02, 0x0d};
  Notif rk;
  if (!waitPrefix(qAvdtp, p020d, 4, 10000, &rk)) {
    Serial.println("[auth] no remote_key");
    return false;
  }
  uint8_t remoteKey[16];
  memcpy(remoteKey, rk.buf + 4, 16);
  writeC(chAvdtp, RCV_ACK, sizeof(RCV_ACK));

  // Step 6: receive server proof (cmd 0x0c)
  uint8_t serverProof[64];
  int spLen = receiveParcel(0x0c, serverProof, sizeof(serverProof), 10000);
  if (spLen < 32) {
    Serial.printf("[auth] bad server proof len=%d\n", spLen);
    return false;
  }

  // Step 7: derive + verify
  if (!deriveKeys(randomKey, remoteKey, keys)) return false;
  uint8_t expect[32];
  hmac_sha256(keys.dev_key, 16, keys.salt_inv, 32, expect);
  if (memcmp(expect, serverProof, 32) == 0) {
    Serial.println("[auth] server proof verified");
  } else {
    Serial.println("[auth] WARNING server proof mismatch (continuing)");
  }

  // Step 8: send our proof
  uint8_t clientInfo[32];
  hmac_sha256(keys.app_key, 16, keys.salt, 32, clientInfo);
  writeC(chAvdtp, CMD_SEND_INFO, sizeof(CMD_SEND_INFO));
  if (!waitPrefix(qAvdtp, RCV_RDY, sizeof(RCV_RDY), 5000)) {
    Serial.println("[auth] no RCV_RDY for info");
    return false;
  }
  sendParcel(chAvdtp, clientInfo, 32);
  if (!waitPrefix(qAvdtp, RCV_OK, sizeof(RCV_OK), 5000)) {
    Serial.println("[auth] no RCV_OK after info");
    return false;
  }
  Serial.println("[auth] AUTH COMPLETE");
  return true;
}

static bool propExchange(SessionKeys &keys, uint32_t seq, const uint8_t *payload,
                         size_t plen) {
  uint8_t ct[256];
  uint8_t nonce[12];
  mi_data_nonce(keys.app_iv, seq, nonce);
  if (!aes_ccm_encrypt(keys.app_key, nonce, payload, plen, ct)) return false;
  size_t ctlen = plen + 4;

  writeC(chDataRx, HEADER6, sizeof(HEADER6));
  if (!waitPrefix(qDataRx, RCV_RDY, sizeof(RCV_RDY), 5000)) {
    Serial.printf("[data] seq%u: no RCV_RDY\n", seq);
    return false;
  }

  uint8_t frame[264];
  frame[0] = 0x01;
  frame[1] = 0x00;
  frame[2] = (uint8_t)(seq & 0xff);
  frame[3] = (uint8_t)((seq >> 8) & 0xff);
  memcpy(frame + 4, ct, ctlen);
  writeC(chDataRx, frame, 4 + ctlen);
  if (!waitPrefix(qDataRx, RCV_OK, sizeof(RCV_OK), 5000)) {
    Serial.printf("[data] seq%u: no RCV_OK\n", seq);
    return false;
  }

  // Read + decrypt the reply (we don't need to parse it for weight).
  uint32_t devSeq = 0;
  uint8_t rct[256];
  int rlen = receiveDataRx2(&devSeq, rct, sizeof(rct), 10000);
  if (rlen > 4) {
    uint8_t pt[256];
    mi_data_nonce(keys.dev_iv, devSeq, nonce);
    bool ok = aes_ccm_decrypt(keys.dev_key, nonce, rct, rlen, pt);
    if (ok) {
      int n = rlen - 4;
      Serial.printf("[data] seq%u reply type=0x%02x len=%d\n", seq, pt[0], n);
      // NOTE: do NOT record weight from exchange replies — the subscribe reply
      // echoes the scale's cached last measurement. Real weigh-ins come via the
      // live stream in holdConnection().
    } else {
      Serial.printf("[data] seq%u decrypt FAIL len=%d\n", seq, rlen);
    }
  } else {
    Serial.printf("[data] seq%u: reply empty/none (rlen=%d)\n", seq, rlen);
  }
  return true;
}

static void buildUserProfile(uint8_t *out, size_t *outLen) {
  char js[220];
  int n = snprintf(
      js, sizeof(js),
      "{\"mid\":\"0\",\"duid\":1,\"uc\":1,\"ow\":1,\"unit\":1,\"time\":%lu,"
      "\"ud\":[{\"duid\":1,\"ut\":1,\"age\":%d,\"sex\":%d,\"hi\":%d,\"wt\":600}]}",
      (unsigned long)(millis() / 1000), USER_AGE, USER_SEX, USER_HEIGHT_CM);
  const uint8_t header[] = {0x8f, 0x20, 0x03, 0x00, 0x05,
                            0x07, 0x01, 0x01, 0x01, 0x00};
  size_t off = 0;
  memcpy(out + off, header, sizeof(header));
  off += sizeof(header);
  out[off++] = (uint8_t)n;
  out[off++] = 0xa0;
  memcpy(out + off, js, n);
  off += n;
  *outLen = off;
}

// Hold the connection and process the live weight stream. We stay subscribed
// so every weigh-in is caught in real time (no connect/auth race). Releases
// after IDLE_RELEASE_MS with nobody on the scale, so the scale can sleep.
static const uint32_t IDLE_RELEASE_MS = 180000;  // 3 min

static void holdConnection(SessionKeys &keys) {
  status("Ready");
  resetStability();
  lastLoadMs = millis();
  bool wasLoaded = false;
  while (client && client->isConnected()) {
    s_heartbeat = millis();        // liveness during a held session
    // service keepalive pings on AVDTP
    Notif n;
    while (xQueueReceive(qAvdtp, &n, 0) == pdTRUE) {
      if (n.len == 6 && memcmp(n.buf, PING, 6) == 0)
        writeC(chAvdtp, PONG, sizeof(PONG));
    }
    // read a data frame
    uint32_t devSeq = 0;
    uint8_t ct[256];
    int clen = receiveDataRx2(&devSeq, ct, sizeof(ct), 1000);
    if (clen > 4) {
      uint8_t pt[256];
      uint8_t nonce[12];
      mi_data_nonce(keys.dev_iv, devSeq, nonce);
      if (aes_ccm_decrypt(keys.dev_key, nonce, ct, clen, pt)) {
        handleWeightPlain(pt, clen - 4);
      }
    }
    // status + re-arm when the person has stepped off
    bool loaded = (millis() - lastLoadMs) < 1500;
    if (loaded != wasLoaded) {
      status(loaded ? "Reading" : "Ready");
      wasLoaded = loaded;
    }
    if (!loaded && recordedThisLoad && (millis() - lastLoadMs) > 3000)
      resetStability();  // re-arm for the next stand
    // release the scale after a long idle so it can sleep (saves its battery)
    if (millis() - lastLoadMs > IDLE_RELEASE_MS) {
      Serial.println("[ble] idle release (scale can sleep)");
      break;
    }
  }
}

// Returns true if a stable reading was captured this session.
static bool runSession() {
  client = NimBLEDevice::createClient();
  client->setConnectTimeout(8);  // seconds
  status("Connecting");
  bool connected = false;
  for (int attempt = 0; attempt < 3 && !connected; attempt++) {
    connected = client->connect(NimBLEAddress(SCALE_MAC, BLE_ADDR_PUBLIC));
    if (!connected) delay(300);
  }
  if (!connected) {
    status("Connect failed");
    NimBLEDevice::deleteClient(client);
    client = nullptr;
    return false;
  }
  status("Connected");
  Serial.printf("[ble] negotiated MTU=%d\n", client->getMTU());

  NimBLERemoteService *svc = client->getService(NimBLEUUID(UUID_SERVICE));
  if (!svc) {
    Serial.println("[ble] no fe95 service");
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    client = nullptr;
    return false;
  }
  chUpnp = svc->getCharacteristic(NimBLEUUID(UUID_UPNP));
  chAvdtp = svc->getCharacteristic(NimBLEUUID(UUID_AVDTP));
  chDataRx = svc->getCharacteristic(NimBLEUUID(UUID_DATA_RX));
  chDataRx2 = svc->getCharacteristic(NimBLEUUID(UUID_DATA_RX2));
  chDevInfo = svc->getCharacteristic(NimBLEUUID(UUID_DEV_INFO));
  chFw = svc->getCharacteristic(NimBLEUUID(UUID_FW));

  if (!chUpnp || !chAvdtp || !chDataRx || !chDataRx2) {
    Serial.println("[ble] missing characteristics");
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    client = nullptr;
    return false;
  }

  SessionKeys keys;
  if (runAuth(keys)) {
    status("Authenticated");
    delay(80);                    // brief settle before exchanges

    // Skip the seq1 user-profile exchange (it was the flaky one; we only want
    // weight). seq0 = capability, seq1 = subscribe (contiguous nonce counter).
    propExchange(keys, 0, PAYLOAD_CAP, sizeof(PAYLOAD_CAP));
    delay(120);
    propExchange(keys, 1, PAYLOAD_PROP, sizeof(PAYLOAD_PROP));

    holdConnection(keys);         // persistent: catch every weigh-in live
  }

  if (client) {
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    client = nullptr;
  }
  return false;
}

// ── Scan callback ───────────────────────────────────────────────────────────
class ScanCB : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice *dev) override {
    if (dev->getAddress() != NimBLEAddress(SCALE_MAC, BLE_ADDR_PUBLIC)) return;
    // Only wake for an ACTIVE beacon (someone is on the scale). The fe95
    // service-data frame-control flips from idle 0x5830 to active 0x5b10 when
    // stepped on. Gating on this avoids reconnecting to an idle scale (which
    // just keeps it awake) and means a live measurement is in progress.
    std::string sd = dev->getServiceData(NimBLEUUID((uint16_t)UUID_SERVICE));
    bool active = sd.size() >= 2 && (uint8_t)sd[1] == 0x5b;
    if (active) {
      scaleSeen = true;
      NimBLEDevice::getScan()->stop();
    }
  }
};

void setWeightCallback(WeightCallback cb) { weightCb = cb; }
void setStatusCallback(StatusCallback cb) { statusCb = cb; }

void begin() {
  qAvdtp = xQueueCreate(6, sizeof(Notif));
  qDataRx = xQueueCreate(6, sizeof(Notif));
  qDataRx2 = xQueueCreate(8, sizeof(Notif));
  qDevInfo = xQueueCreate(4, sizeof(Notif));
  qUpnp = xQueueCreate(4, sizeof(Notif));

  NimBLEDevice::init("");
  NimBLEDevice::setMTU(247);
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCB(), false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(80);
}

uint32_t heartbeat() { return s_heartbeat; }

void loop() {
  s_heartbeat = millis();          // liveness tick for the watchdog
  scaleSeen = false;
  status("Scanning");
  NimBLEDevice::getScan()->start(5, false);
  NimBLEDevice::getScan()->clearResults();
  if (scaleSeen) {
    runSession();   // connects, then holds until idle-release or drop
    delay(500);
  } else {
    delay(300);
  }
}

static void bleTask(void *) {
  begin();
  for (;;) loop();
}

void startTask() {
  xTaskCreatePinnedToCore(bleTask, "ble", 8192, nullptr, 4, nullptr, 0);
}

}  // namespace MiScale
