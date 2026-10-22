#include "cloud.h"

#include <Arduino.h>
#include <esp_system.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <sys/stat.h>
#include <WiFiClientSecure.h>

#include "config.h"
#include "history.h"
#include "timekeeper.h"

// Pending queue: append-only CSV of readings not yet synced ("ts,kg").
static const char *PENDING_FILE = "/pending.csv";

// Per-device sheet config (NVS namespace "cloud").
static String s_url, s_token;
static bool s_loaded = false;

static void loadConfig() {
  if (s_loaded) return;
  Preferences p;
  p.begin("cloud", true);
  s_url = p.getString("url", "");
  s_token = p.getString("tok", "");
  p.end();
#if defined(CLOUD_FROM_SECRETS)
  // Personal builds only: default to the owner's sheet from secrets.h when the
  // device has none saved. Product builds never compile the sheet in.
  if (!s_url.length()) {
    s_url = CLOUD_URL;
    s_token = CLOUD_TOKEN;
  }
#endif
  s_loaded = true;
}

void cloudBegin() {
  // LittleFS is mounted by historyBegin(); here we only load the sheet config.
  loadConfig();
  Serial.printf("[cloud] %s\n", s_url.length() ? "sheet configured" : "off (no sheet set)");
}

void cloudSetConfig(const char *url, const char *token) {
  String u = url ? url : "", t = token ? token : "";
  u.trim();
  t.trim();
  if (!u.startsWith("http")) u = "";          // anything else = cloud off
  Preferences p;
  p.begin("cloud", false);
  p.putString("url", u);
  p.putString("tok", t);
  p.end();
  s_url = u;
  s_token = t;
  s_loaded = true;
  Serial.printf("[cloud] config saved: %s\n", u.length() ? "sheet set" : "offline");
}

bool cloudConfigured() { loadConfig(); return s_url.length() > 0; }
const char *cloudUrl() { loadConfig(); return s_url.c_str(); }
const char *cloudToken() { loadConfig(); return s_token.c_str(); }

// Pending count is cached in RAM: the UI and main loop ask for it every few ms,
// and on this core even LittleFS.exists() opens the file and logs "[E] does not
// exist" when the queue is empty. The file is read once; after that the cache is
// kept in step by cloudQueue() / cloudFlush().
static int s_pending = -1;                 // -1 = not counted yet

static bool pendingFileExists() {
  struct stat st;                          // stat() checks silently, unlike exists()
  return stat("/littlefs/pending.csv", &st) == 0;
}

// A reading taken before the clock was set (ts 0) is queued as "0,kg,ms,boot":
// millis() when it was taken + this boot's id. Once NTP sets the clock in the
// same boot it gets its real time; the sheet would otherwise stamp the upload
// time (a burst of rows seconds apart). From an earlier boot it can't be dated.
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

// Nothing to send while no sheet is configured (readings stay queued locally).
bool cloudHasPending() { return cloudConfigured() && cloudPendingCount() > 0; }

// POST one reading. Returns true on HTTP 200.
static bool postReading(uint32_t ts, float kg) {
  WiFiClientSecure client;
  client.setInsecure();  // skip cert chain (Apps Script host); fine for this
  HTTPClient http;
  // Do NOT follow the redirect: Apps Script runs doPost() (appends the row) at
  // /exec and returns 302. Following it re-POSTs to Google's echo host and 400s,
  // which we'd misread as failure and retry → duplicate rows. The 302 IS success.
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.setTimeout(12000);
  if (!http.begin(client, s_url)) return false;
  http.addHeader("Content-Type", "application/json");

  char body[128];
  snprintf(body, sizeof(body), "{\"token\":\"%s\",\"ts\":%u,\"kg\":%.2f}",
           s_token.c_str(), (unsigned)ts, kg);
  int code = http.POST((uint8_t *)body, strlen(body));
  http.end();
  Serial.printf("[cloud] POST %.2f -> HTTP %d\n", kg, code);
  return code == 200 || code == 302;  // 302 = appended + redirecting = done
}

int cloudFlush() {
  if (!cloudHasPending()) return 0;

  // Read all pending readings into memory.
  static const int MAXQ = 64;
  uint32_t ts[MAXQ], ms[MAXQ], boot[MAXQ];
  float kg[MAXQ];
  int nq = 0;
  {
    File f = LittleFS.open(PENDING_FILE, "r");
    if (!f) return 0;
    while (f.available() && nq < MAXQ) {
      String line = f.readStringUntil('\n');
      line.trim();
      int c = line.indexOf(',');
      if (c < 0) continue;
      ts[nq] = (uint32_t)line.substring(0, c).toInt();
      kg[nq] = line.substring(c + 1).toFloat();
      int c2 = line.indexOf(',', c + 1), c3 = c2 < 0 ? -1 : line.indexOf(',', c2 + 1);
      ms[nq] = c3 < 0 ? 0 : (uint32_t)strtoul(line.substring(c2 + 1, c3).c_str(), nullptr, 10);
      boot[nq] = c3 < 0 ? 0 : (uint32_t)strtoul(line.substring(c3 + 1).c_str(), nullptr, 10);
      nq++;
    }
    f.close();
  }
  if (nq == 0) return 0;

  Serial.printf("[cloud] flushing %d reading(s)...\n", nq);
  if (!wifiConnect(12000)) {
    Serial.println("[cloud] WiFi unavailable, will retry later");
    wifiOff();
    return 0;
  }
  ntpEnsure(4000);   // backfill the clock if boot had no network

  // Date readings taken unclocked earlier in this boot (see cloudQueue).
  if (timeIsSet()) {
    uint32_t now = (uint32_t)nowEpoch();
    for (int i = 0; i < nq; i++) {
      if (ts[i] || !boot[i]) continue;
      if (boot[i] == bootId()) {
        ts[i] = now - (millis() - ms[i]) / 1000;
        Serial.printf("[cloud] dated unclocked %.2f kg @ %u\n", kg[i], (unsigned)ts[i]);
      } else {
        Serial.printf("[cloud] %.2f kg was taken before a restart without a clock: "
                      "the sheet will stamp the upload time\n", kg[i]);
      }
    }
  }

  int sent = 0;
  for (int i = 0; i < nq; i++) {
    if (postReading(ts[i], kg[i]))
      sent++;
    else
      break;  // stop on first failure; keep the rest queued
    delay(200);
  }

  wifiOff();

  // Rewrite the queue with whatever didn't send.
  if (sent >= nq) {
    LittleFS.remove(PENDING_FILE);
  } else if (sent > 0) {
    File f = LittleFS.open(PENDING_FILE, "w");
    if (f) {
      for (int i = sent; i < nq; i++) {
        if (ts[i] || !boot[i]) f.printf("%u,%.2f\n", (unsigned)ts[i], kg[i]);
        else f.printf("0,%.2f,%lu,%lu\n", kg[i], (unsigned long)ms[i], (unsigned long)boot[i]);
      }
      f.close();
    }
  }
  s_pending = nq - sent;                   // keep the RAM count in step with the file
  Serial.printf("[cloud] synced %d/%d\n", sent, nq);
  return sent;
}

int cloudPull() {
  if (!cloudConfigured()) {
    Serial.println("[cloud] pull: no sheet configured");
    return -1;
  }
  if (!wifiConnect(12000)) {
    Serial.println("[cloud] pull: WiFi unavailable");
    wifiOff();
    return -1;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // GET → 302 → googleusercontent
  http.setTimeout(15000);
  String url = s_url + "?token=" + s_token;
  if (!http.begin(client, url)) {
    wifiOff();
    return -1;
  }
  int code = http.GET();
  String body = http.getString();
  http.end();
  wifiOff();

  if (code != 200) {
    Serial.printf("[cloud] pull HTTP %d\n", code);
    return -1;
  }
  if (body.indexOf(',') < 0) {
    Serial.println("[cloud] pull: no data / bad token");
    return -1;
  }

  // Overwrite the local log with the sheet's data, then reload into RAM.
  File f = LittleFS.open("/history.csv", "w");
  if (!f) return -1;
  f.print(body);
  if (!body.endsWith("\n")) f.print("\n");
  f.close();

  historyReload();
  int n = historyCount();
  Serial.printf("[cloud] pulled %d readings from sheet\n", n);
  return n;
}
