#include "cloud.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <esp_system.h>
#include <sys/stat.h>

#include "cloud_config.h"
#include "config.h"
#include "google_roots.h"
#include "history.h"
#include "timekeeper.h"

// Pending queue: append-only CSV of readings not yet uploaded ("ts,kg").
static const char *PENDING_FILE = "/pending.csv";

// ── Account (NVS namespace "acct") ──────────────────────────────────────────
// email + uid + refresh token persist; the ID token (1 h) lives in RAM only.
static String s_email, s_uid, s_rtok, s_idTok;
static uint32_t s_idTokUntil = 0;            // millis() when the ID token expires
static bool s_lost = false;                  // refresh token rejected → sign in again

static void saveAccount() {
  Preferences p;
  p.begin("acct", false);
  p.putString("email", s_email);
  p.putString("uid", s_uid);
  p.putString("rtok", s_rtok);
  p.end();
}

void cloudBegin() {
  Preferences p;
  p.begin("acct", false);                  // read-write: read-only logs an error if it's new
  if (p.isKey("email")) s_email = p.getString("email", "");
  if (p.isKey("uid")) s_uid = p.getString("uid", "");
  if (p.isKey("rtok")) s_rtok = p.getString("rtok", "");
  p.end();
  Serial.printf("[cloud] %s\n", !cloudAvailable() ? "accounts not set up in this build"
                               : cloudConfigured() ? ("signed in as " + s_email).c_str()
                               : "not signed in");
}

bool cloudAvailable() { return FIREBASE_API_KEY[0] && FIREBASE_PROJECT_ID[0]; }
bool cloudConfigured() { return cloudAvailable() && s_rtok.length() && !s_lost; }
bool cloudSignInLost() { return s_lost; }
const char *cloudEmail() { return s_email.c_str(); }

// ── HTTPS to Google (certificate-verified) ──────────────────────────────────
static WiFiClientSecure &tls() {
  static WiFiClientSecure c;
  static bool init = false;
  if (!init) {
    c.setCACert(GOOGLE_ROOTS_PEM);
    init = true;
  }
  return c;
}

// One request; returns the HTTP status (<0 = connection failed), body in resp.
static int request(const char *method, const String &url, const String &body, String &resp,
                   const char *contentType = "application/json") {
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(tls(), url)) return -1;
  if (body.length()) http.addHeader("Content-Type", contentType);
  if (s_idTok.length() && url.indexOf("firestore.googleapis.com") >= 0)
    http.addHeader("Authorization", "Bearer " + s_idTok);
  int code = strcmp(method, "GET") == 0 ? http.GET() : http.sendRequest(method, body);
  resp = code > 0 ? http.getString() : String();
  http.end();
  return code;
}

// Minimal JSON helpers for Google's flat responses.
static String jsonStr(const String &j, const char *key) {
  String k = String("\"") + key + "\"";
  int i = j.indexOf(k);
  if (i < 0) return String();
  i = j.indexOf('"', j.indexOf(':', i + k.length()) + 1);
  int e = i < 0 ? -1 : j.indexOf('"', i + 1);
  return e < 0 ? String() : j.substring(i + 1, e);
}
static String jsonEsc(const char *s) {
  String o;
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') o += '\\';
    if ((uint8_t)*s >= 0x20) o += *s;
  }
  return o;
}

static String authUrl(const char *op) {
  return String("https://identitytoolkit.googleapis.com/v1/accounts:") + op +
         "?key=" FIREBASE_API_KEY;
}
static String docsUrl() {
  return String("https://firestore.googleapis.com/v1/projects/" FIREBASE_PROJECT_ID
                "/databases/(default)/documents");
}

// A valid ID token in RAM, refreshing it with the saved refresh token if needed.
static bool ensureToken() {
  if (s_idTok.length() && (int32_t)(s_idTokUntil - millis()) > 60000) return true;
  String r;
  int code = request("POST", "https://securetoken.googleapis.com/v1/token?key=" FIREBASE_API_KEY,
                     "grant_type=refresh_token&refresh_token=" + s_rtok, r,
                     "application/x-www-form-urlencoded");
  if (code != 200) {
    Serial.printf("[cloud] token refresh: HTTP %d %s\n", code, jsonStr(r, "message").c_str());
    if (code == 400 || code == 403) s_lost = true;   // revoked / user deleted / disabled
    return false;
  }
  s_idTok = jsonStr(r, "id_token");
  s_idTokUntil = millis() + jsonStr(r, "expires_in").toInt() * 1000UL;
  String rt = jsonStr(r, "refresh_token");
  if (rt.length() && rt != s_rtok) { s_rtok = rt; saveAccount(); }
  return s_idTok.length() > 0;
}

// ── Sign in / out ───────────────────────────────────────────────────────────
static bool pendingFileExists();

CloudAuth cloudSignIn(const char *email, const char *password, bool create) {
  if (!cloudAvailable()) return AUTH_FAILED;
  if (!wifiConnect(12000)) return AUTH_NO_NETWORK;
  String body = String("{\"email\":\"") + jsonEsc(email) + "\",\"password\":\"" +
                jsonEsc(password) + "\",\"returnSecureToken\":true}";
  String r;
  int code = request("POST", authUrl(create ? "signUp" : "signInWithPassword"), body, r);
  if (code < 0) return AUTH_NO_NETWORK;
  if (code != 200) {
    String m = jsonStr(r, "message");          // e.g. "INVALID_LOGIN_CREDENTIALS"
    Serial.printf("[cloud] sign-in failed: HTTP %d %s\n", code, m.c_str());
    if (m.startsWith("EMAIL_EXISTS")) return AUTH_EMAIL_TAKEN;
    if (m.startsWith("WEAK_PASSWORD")) return AUTH_WEAK_PASSWORD;
    if (m.startsWith("INVALID_EMAIL") || m.startsWith("MISSING_EMAIL")) return AUTH_BAD_EMAIL;
    if (m.startsWith("TOO_MANY_ATTEMPTS")) return AUTH_TOO_MANY;
    if (m.startsWith("INVALID_LOGIN") || m.startsWith("INVALID_PASSWORD") ||
        m.startsWith("EMAIL_NOT_FOUND") || m.startsWith("MISSING_PASSWORD"))
      return AUTH_BAD_LOGIN;
    return AUTH_FAILED;
  }
  String uid = jsonStr(r, "localId");
  if (s_uid.length() && uid != s_uid) {
    // A different person's account: their weights must not mix with the last one's.
    Serial.println("[cloud] different account -> clearing the local log");
    historyClear();
    LittleFS.remove(PENDING_FILE);
  }
  s_email = email;
  s_uid = uid;
  s_rtok = jsonStr(r, "refreshToken");
  s_idTok = jsonStr(r, "idToken");
  s_idTokUntil = millis() + jsonStr(r, "expiresIn").toInt() * 1000UL;
  s_lost = false;
  saveAccount();
  Serial.printf("[cloud] signed in as %s (%s)\n", s_email.c_str(), create ? "new account" : "existing");
  return AUTH_OK;
}

const char *cloudAuthText(CloudAuth r) {
  switch (r) {
    case AUTH_OK: return "Signed in";
    case AUTH_BAD_LOGIN: return "Wrong email or password";
    case AUTH_EMAIL_TAKEN: return "That email already has an account";
    case AUTH_WEAK_PASSWORD: return "Password needs 6+ characters";
    case AUTH_BAD_EMAIL: return "That email address isn't valid";
    case AUTH_TOO_MANY: return "Too many tries, wait a few minutes";
    case AUTH_NO_NETWORK: return "No internet connection";
    default: return "Sign-in failed";
  }
}

bool cloudSendPasswordReset(const char *email) {
  if (!cloudAvailable() || !wifiConnect(12000)) return false;
  String r;
  int code = request("POST", authUrl("sendOobCode"),
                     String("{\"requestType\":\"PASSWORD_RESET\",\"email\":\"") + jsonEsc(email) +
                         "\"}", r);
  Serial.printf("[cloud] password reset email: HTTP %d\n", code);
  return code == 200;
}

static int s_pending = -1;                    // RAM count of the queue (see below)

void cloudSignOut() {
  if (cloudHasPending()) cloudFlush();        // best effort: don't lose the last readings
  s_email = s_uid = s_rtok = s_idTok = "";
  s_lost = false;
  saveAccount();
  historyClear();
  LittleFS.remove(PENDING_FILE);
  s_pending = 0;
  Serial.println("[cloud] signed out; local log cleared");
}

// ── Pending queue ───────────────────────────────────────────────────────────
// Pending count is cached in RAM: the UI and main loop ask for it every few ms,
// and on this core even LittleFS.exists() opens the file and logs "[E] does not
// exist" when the queue is empty. The file is read once; after that the cache is
// kept in step by cloudQueue() / cloudFlush().
static bool pendingFileExists() {
  struct stat st;                          // stat() checks silently, unlike exists()
  return stat("/littlefs/pending.csv", &st) == 0;
}

// A reading taken before the clock was set (ts 0) is queued as "0,kg,ms,boot":
// millis() when it was taken + this boot's id. Once NTP sets the clock in the
// same boot it gets its real time. From an earlier boot it can't be dated, and
// it is dropped from the upload (it stays in the local log).
static uint32_t bootId() {
  static uint32_t id = 0;
  while (!id) id = esp_random();
  return id;
}

void cloudQueue(uint32_t ts, float kg) {
  int before = cloudPendingCount();
  File f = LittleFS.open(PENDING_FILE, "a");
  if (!f) {
    Serial.println("[cloud] queue open failed");
    return;
  }
  if (ts) f.printf("%u,%.2f\n", (unsigned)ts, kg);
  else f.printf("0,%.2f,%lu,%lu\n", kg, (unsigned long)millis(), (unsigned long)bootId());
  f.close();
  s_pending = before + 1;
  Serial.printf("[cloud] queued %.2f kg (pending=%d)\n", kg, s_pending);
}

int cloudPendingCount() {
  if (s_pending >= 0) return s_pending;
  s_pending = 0;
  if (!pendingFileExists()) return 0;
  File f = LittleFS.open(PENDING_FILE, "r");
  if (!f) return 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length()) s_pending++;
  }
  f.close();
  return s_pending;
}

// Nothing uploads while signed out (readings wait in the queue for an account).
bool cloudHasPending() { return cloudConfigured() && cloudPendingCount() > 0; }

struct Pending { uint32_t ts, ms, boot; float kg; };
static const int MAXQ = 64;

static int readQueue(Pending *q) {
  int n = 0;
  if (!pendingFileExists()) return 0;
  File f = LittleFS.open(PENDING_FILE, "r");
  if (!f) return 0;
  while (f.available() && n < MAXQ) {
    String line = f.readStringUntil('\n');
    line.trim();
    int c = line.indexOf(',');
    if (c < 0) continue;
    Pending &p = q[n++];
    p.ts = (uint32_t)line.substring(0, c).toInt();
    p.kg = line.substring(c + 1).toFloat();
    int c2 = line.indexOf(',', c + 1), c3 = c2 < 0 ? -1 : line.indexOf(',', c2 + 1);
    p.ms = c3 < 0 ? 0 : (uint32_t)strtoul(line.substring(c2 + 1, c3).c_str(), nullptr, 10);
    p.boot = c3 < 0 ? 0 : (uint32_t)strtoul(line.substring(c3 + 1).c_str(), nullptr, 10);
  }
  f.close();
  return n;
}

static void writeQueue(const Pending *q, int n) {
  if (n == 0) {
    LittleFS.remove(PENDING_FILE);
  } else {
    File f = LittleFS.open(PENDING_FILE, "w");
    if (f) {
      for (int i = 0; i < n; i++) {
        if (q[i].ts || !q[i].boot) f.printf("%u,%.2f\n", (unsigned)q[i].ts, q[i].kg);
        else f.printf("0,%.2f,%lu,%lu\n", q[i].kg, (unsigned long)q[i].ms,
                      (unsigned long)q[i].boot);
      }
      f.close();
    }
  }
  s_pending = n;
}

int cloudFlush() {
  if (!cloudHasPending()) return 0;
  static Pending q[MAXQ];
  int nq = readQueue(q);
  if (nq == 0) { writeQueue(q, 0); return 0; }

  Serial.printf("[cloud] uploading %d reading(s)...\n", nq);
  if (!wifiConnect(12000)) {
    Serial.println("[cloud] WiFi unavailable, will retry later");
    wifiOff();
    return 0;
  }
  ntpEnsure(4000);   // backfill the clock if boot had no network

  // Date readings taken unclocked earlier in this boot (see cloudQueue); one
  // from an earlier boot can't be dated and isn't uploaded.
  uint32_t now = timeIsSet() ? (uint32_t)nowEpoch() : 0;
  for (int i = 0; i < nq; i++) {
    if (q[i].ts) continue;
    if (now && q[i].boot == bootId()) q[i].ts = now - (millis() - q[i].ms) / 1000;
    else if (q[i].boot != bootId()) {
      Serial.printf("[cloud] %.2f kg has no date (taken before a restart): not uploaded\n", q[i].kg);
      q[i].kg = 0;                          // dropped below
    }
  }

  int sent = 0;
  if (ensureToken()) {
    String body = "{\"writes\":[";
    int nw = 0;
    for (int i = 0; i < nq; i++) {
      if (!q[i].ts || q[i].kg <= 0) continue;
      char at[24], w[360];
      time_t t = q[i].ts;
      struct tm tm;
      gmtime_r(&t, &tm);
      strftime(at, sizeof(at), "%Y-%m-%dT%H:%M:%SZ", &tm);   // "at": readable timestamp
      snprintf(w, sizeof(w),
               "%s{\"update\":{\"name\":\"projects/" FIREBASE_PROJECT_ID
               "/databases/(default)/documents/users/%s/readings/%u\",\"fields\":{"
               "\"ts\":{\"integerValue\":\"%u\"},\"at\":{\"timestampValue\":\"%s\"},"
               "\"kg\":{\"doubleValue\":%.2f},\"src\":{\"stringValue\":\"device\"}}}}",
               nw ? "," : "", s_uid.c_str(), (unsigned)q[i].ts, (unsigned)q[i].ts, at, q[i].kg);
      body += w;
      nw++;
    }
    body += "]}";
    String r;
    int code = nw ? request("POST", docsUrl() + ":commit", body, r) : 200;
    if (code == 200) sent = nw;
    else Serial.printf("[cloud] upload: HTTP %d %s\n", code, jsonStr(r, "message").c_str());
  }
  wifiOff();

  if (sent || nq) {
    // Keep only what still needs uploading: everything if the commit failed
    // (it's all-or-nothing), else just same-boot readings still awaiting a date.
    int keep = 0;
    for (int i = 0; i < nq; i++) {
      bool waiting = !q[i].ts && q[i].kg > 0;
      if ((!sent && q[i].kg > 0) || waiting) q[keep++] = q[i];
    }
    writeQueue(q, keep);
  }
  Serial.printf("[cloud] uploaded %d, %d still pending\n", sent, s_pending);
  return sent;
}

int cloudPull() {
  if (!cloudConfigured()) {
    Serial.println("[cloud] pull: not signed in");
    return -1;
  }
  if (!wifiConnect(12000)) {
    Serial.println("[cloud] pull: WiFi unavailable");
    wifiOff();
    return -1;
  }
  if (!ensureToken()) { wifiOff(); return -1; }

  File out = LittleFS.open("/history.tmp", "w");
  if (!out) { wifiOff(); return -1; }
  int n = 0, pages = 0;
  String pageToken;
  bool ok = true;
  do {
    // Ordered by document id = epoch, oldest first; only the kg field. Small
    // pages: a big response doesn't fit in RAM next to BLE + TLS and arrives cut
    // short (losing the nextPageToken at its end = a silently partial history).
    String url = docsUrl() + "/users/" + s_uid + "/readings?pageSize=20&mask.fieldPaths=kg";
    if (pageToken.length()) url += "&pageToken=" + pageToken;
    String r;
    int code = request("GET", url, "", r);
    r.trim();
    if (code != 200 || !r.endsWith("}")) {
      Serial.printf("[cloud] pull: HTTP %d, %u bytes%s %s\n", code, r.length(),
                    r.endsWith("}") ? "" : " (cut short)", jsonStr(r, "message").c_str());
      ok = false;
      break;
    }
    for (int p = r.indexOf("/readings/"); p >= 0; p = r.indexOf("/readings/", p + 10)) {
      uint32_t ts = (uint32_t)strtoul(r.c_str() + p + 10, nullptr, 10);
      int k = r.indexOf("\"kg\"", p), v = k < 0 ? -1 : r.indexOf("Value\"", k);
      if (v < 0) continue;
      v = r.indexOf(':', v) + 1;
      while (v < (int)r.length() && (r[v] == ' ' || r[v] == '"')) v++;
      float kg = r.substring(v, v + 12).toFloat();
      if (ts && kg > 0) { out.printf("%u,%.2f\n", (unsigned)ts, kg); n++; }
    }
    pageToken = jsonStr(r, "nextPageToken");
  } while (pageToken.length() && ++pages < 100);   // up to 2000 readings
  if (pageToken.length()) Serial.println("[cloud] pull: more than 2000 readings, kept the oldest");
  wifiOff();

  if (ok) {   // keep readings not uploaded yet, so they don't vanish from the log
    static Pending q[MAXQ];
    int nq = readQueue(q);
    for (int i = 0; i < nq; i++) out.printf("%u,%.2f\n", (unsigned)q[i].ts, q[i].kg);
  }
  out.close();
  if (!ok) { LittleFS.remove("/history.tmp"); return -1; }
  LittleFS.remove("/history.csv");
  LittleFS.rename("/history.tmp", "/history.csv");
  historyReload();
  Serial.printf("[cloud] pulled %d readings from the account\n", n);
  return historyCount();
}

// ── Settings (users/<uid>/settings/profile) ─────────────────────────────────
static String settingsUrl() { return docsUrl() + "/users/" + s_uid + "/settings/profile"; }

bool cloudPushSettings(const UserSettings &s) {
  if (!cloudConfigured() || !wifiConnect(12000) || !ensureToken()) return false;
  char at[24], body[420];
  time_t t = s.updated;
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(at, sizeof(at), "%Y-%m-%dT%H:%M:%SZ", &tm);
  snprintf(body, sizeof(body),
           "{\"fields\":{\"unit\":{\"stringValue\":\"%s\"},\"sound\":{\"integerValue\":\"%d\"},"
           "\"goalKg\":{\"doubleValue\":%.2f},"
           "\"heightCm\":{\"doubleValue\":%.1f},\"updatedTs\":{\"integerValue\":\"%u\"},"
           "\"updatedAt\":{\"timestampValue\":\"%s\"}}}",
           s.lb ? "lb" : "kg", s.sound, s.goalKg, s.heightCm,
           (unsigned)s.updated, at);
  String r;
  int code = request("PATCH", settingsUrl(), body, r);   // creates or replaces
  Serial.printf("[cloud] settings upload: HTTP %d %s\n", code, jsonStr(r, "message").c_str());
  return code == 200;
}

// Value of a Firestore field: {"<key>": {"<type>Value": <v> or "<v>"}}.
static String fieldValue(const String &j, const char *key) {
  int k = j.indexOf(String("\"") + key + "\"");
  int v = k < 0 ? -1 : j.indexOf("Value\"", k);
  if (v < 0) return String();
  v = j.indexOf(':', v) + 1;
  while (v < (int)j.length() && j[v] == ' ') v++;
  if (j[v] == '"') return j.substring(v + 1, j.indexOf('"', v + 1));
  int e = v;
  while (e < (int)j.length() && j[e] != ',' && j[e] != '}' && j[e] != '\n') e++;
  return j.substring(v, e);
}

bool cloudPullSettings(UserSettings &s, bool &found) {
  found = false;
  if (!cloudConfigured() || !wifiConnect(12000) || !ensureToken()) return false;
  String r;
  int code = request("GET", settingsUrl(), "", r);
  if (code == 404) return true;                         // none saved yet
  if (code != 200) {
    Serial.printf("[cloud] settings download: HTTP %d %s\n", code, jsonStr(r, "message").c_str());
    return false;
  }
  found = true;
  s.lb = fieldValue(r, "unit") == "lb";
  s.sound = constrain(fieldValue(r, "sound").toInt(), 0, 3);
  s.goalKg = fieldValue(r, "goalKg").toFloat();
  s.heightCm = fieldValue(r, "heightCm").toFloat();
  s.updated = (uint32_t)strtoul(fieldValue(r, "updatedTs").c_str(), nullptr, 10);
  return true;
}
