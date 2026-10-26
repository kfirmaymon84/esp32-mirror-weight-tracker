#include "ota_update.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include "config.h"
#include "ota_pubkey.h"
#include "timekeeper.h"

#ifndef OTA_BASE_URL
#define OTA_BASE_URL \
  "https://github.com/kfirmaymon84/esp32-mirror-weight-tracker/releases/latest/download"
#endif

// Keep a freshly installed image "pending" until otaConfirmHealthy(): by default
// the Arduino core marks it valid as soon as it boots, defeating the rollback.
extern "C" bool verifyRollbackLater() { return true; }

bool otaAutoEnabled() {
#if defined(OTA_BOARD)
  return true;
#else
  return false;
#endif
}

// NVS "ota": trying = build just installed (cleared once it proves healthy),
// bad = a build that failed to start and was rolled back (never retried).
void otaBootBegin() {
  Preferences p;
  p.begin("ota", false);
  int trying = p.getInt("trying", 0);
  if (trying && trying != FW_BUILD) {
    Serial.printf("[ota] build %d failed to start -> rolled back to %d, won't retry it\n",
                  trying, FW_BUILD);
    p.putInt("bad", trying);
    p.putInt("trying", 0);
  } else if (trying) {
    Serial.printf("[ota] running new build %d (pending until healthy)\n", trying);
  }
  p.end();
}

void otaConfirmHealthy() {
  static bool done = false;
  if (done) return;
  done = true;
  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("[ota] new firmware confirmed");
  }
  Preferences p;
  p.begin("ota", false);
  if (p.getInt("trying", 0)) p.putInt("trying", 0);
  p.end();
}

// TLS without certificate checks: the firmware's integrity comes from the
// signature, not from the transport. Plain http is allowed for a local test server.
static bool httpBegin(HTTPClient &http, const char *url) {
  static WiFiClientSecure tls;
  static WiFiClient plain;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);   // GitHub → asset host
  http.setTimeout(20000);
  if (strncmp(url, "https://", 8) == 0) {
    tls.setInsecure();
    return http.begin(tls, url);
  }
  return http.begin(plain, url);
}

// Minimal JSON field readers for the flat manifest ota/release.py writes.
static int valueStart(const String &j, const char *key) {
  String k = String("\"") + key + "\"";
  int i = j.indexOf(k);
  if (i < 0) return -1;
  i = j.indexOf(':', i + k.length());
  if (i < 0) return -1;
  while (++i < (int)j.length() && (j[i] == ' ' || j[i] == '\n' || j[i] == '\r')) {}
  return i;
}
static bool jsonStr(const String &j, const char *key, char *out, size_t n) {
  int i = valueStart(j, key);
  if (i < 0 || j[i] != '"') return false;
  int e = j.indexOf('"', i + 1);
  if (e < 0 || (size_t)(e - i - 1) >= n) return false;
  strlcpy(out, j.c_str() + i + 1, e - i);
  return true;
}
static long jsonNum(const String &j, const char *key) {
  int i = valueStart(j, key);
  return i < 0 ? -1 : j.substring(i).toInt();
}

static bool signatureOk(const char *msg, const char *sigB64) {
  uint8_t sig[160], hash[32];
  size_t sigLen = 0;
  if (mbedtls_base64_decode(sig, sizeof(sig), &sigLen, (const uint8_t *)sigB64,
                            strlen(sigB64)) != 0)
    return false;
  mbedtls_sha256_ret((const uint8_t *)msg, strlen(msg), hash, 0);
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  int r = mbedtls_pk_parse_public_key(&pk, (const uint8_t *)OTA_PUBKEY_PEM,
                                      sizeof(OTA_PUBKEY_PEM));   // PEM length incl. NUL
  if (r == 0) r = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sigLen);
  mbedtls_pk_free(&pk);
  return r == 0;
}

OtaResult otaCheck(OtaRelease &rel) {
#if !defined(OTA_BOARD)
  return OTA_FAILED;
#else
  if (!wifiConnect(12000)) {
    Serial.println("[ota] check: no WiFi");
    return OTA_FAILED;
  }
  String url = String(OTA_BASE_URL) + "/" + OTA_BOARD + ".json";
  HTTPClient http;
  if (!httpBegin(http, url.c_str())) return OTA_FAILED;
  int code = http.GET();
  String j = code == 200 ? http.getString() : String();
  http.end();
  if (code == 404) {                        // nothing published for this board yet
    Serial.println("[ota] check: no release for this board");
    return OTA_UP_TO_DATE;
  }
  if (code != 200) {
    Serial.printf("[ota] check: HTTP %d\n", code);
    return OTA_FAILED;
  }

  char board[24], sig[200];
  rel.build = (int)jsonNum(j, "build");
  rel.size = (uint32_t)jsonNum(j, "size");
  if (!jsonStr(j, "board", board, sizeof(board)) || !jsonStr(j, "sig", sig, sizeof(sig)) ||
      !jsonStr(j, "sha256", rel.sha256, sizeof(rel.sha256)) ||
      !jsonStr(j, "url", rel.url, sizeof(rel.url)) || rel.build <= 0 || rel.size == 0) {
    Serial.println("[ota] check: bad manifest");
    return OTA_FAILED;
  }
  if (!jsonStr(j, "version", rel.version, sizeof(rel.version)))
    snprintf(rel.version, sizeof(rel.version), "%d", rel.build);
  if (strcmp(board, OTA_BOARD) != 0) {
    Serial.printf("[ota] check: manifest is for '%s', not this board\n", board);
    return OTA_FAILED;
  }

  Preferences p;
  p.begin("ota", true);
  int bad = p.getInt("bad", 0);
  p.end();
  if (rel.build <= FW_BUILD || rel.build == bad) {
    Serial.printf("[ota] up to date (running %d, latest %d%s)\n", FW_BUILD, rel.build,
                  rel.build == bad ? " = rolled back before, skipped" : "");
    return OTA_UP_TO_DATE;
  }
  char msg[120];
  snprintf(msg, sizeof(msg), "%s:%d:%s", board, rel.build, rel.sha256);
  if (!signatureOk(msg, sig)) {
    Serial.printf("[ota] build %d: SIGNATURE INVALID - ignored\n", rel.build);
    return OTA_FAILED;
  }
  Serial.printf("[ota] build %d (%s) available, %u bytes, signature ok\n", rel.build,
                rel.version, (unsigned)rel.size);
  return OTA_AVAILABLE;
#endif
}

bool otaInstall(const OtaRelease &rel, std::function<void(int pct)> progress) {
  if (!wifiConnect(12000)) return false;
  HTTPClient http;
  if (!httpBegin(http, rel.url)) return false;
  int code = http.GET();
  int len = http.getSize();
  if (code != 200 || (len > 0 && (uint32_t)len != rel.size)) {
    Serial.printf("[ota] download: HTTP %d, %d bytes (want %u)\n", code, len, (unsigned)rel.size);
    http.end();
    return false;
  }
  if (!Update.begin(rel.size)) {
    Serial.printf("[ota] no room for the image: %s\n", Update.errorString());
    http.end();
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  WiFiClient *s = http.getStreamPtr();
  static uint8_t buf[2048];
  uint32_t got = 0, lastData = millis();
  int lastPct = -1;
  bool writeOk = true;
  while (got < rel.size && millis() - lastData < 20000) {
    size_t av = s->available();
    if (!av) {
      if (!http.connected()) break;
      delay(2);
      continue;
    }
    size_t want = av < sizeof(buf) ? av : sizeof(buf);
    if (want > rel.size - got) want = rel.size - got;
    int n = s->readBytes(buf, want);
    if (n <= 0) continue;
    if (Update.write(buf, n) != (size_t)n) {
      Serial.printf("[ota] flash write failed: %s\n", Update.errorString());
      writeOk = false;
      break;
    }
    mbedtls_sha256_update_ret(&sha, buf, n);
    got += n;
    lastData = millis();
    int pct = (int)((uint64_t)got * 100 / rel.size);
    if (pct != lastPct && progress) progress(lastPct = pct);
  }
  http.end();
  uint8_t h[32];
  mbedtls_sha256_finish_ret(&sha, h);
  mbedtls_sha256_free(&sha);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", h[i]);

  if (!writeOk || got != rel.size || strcmp(hex, rel.sha256) != 0) {
    Serial.printf("[ota] image rejected: %u/%u bytes, sha %s\n", (unsigned)got,
                  (unsigned)rel.size, got == rel.size ? "MISMATCH" : "-");
    Update.abort();
    return false;
  }
  if (!Update.end()) {
    Serial.printf("[ota] finish failed: %s\n", Update.errorString());
    return false;
  }
  Preferences p;
  p.begin("ota", false);
  p.putInt("trying", rel.build);
  p.end();
  Serial.printf("[ota] build %d installed, verified - restarting into it\n", rel.build);
  return true;
}
