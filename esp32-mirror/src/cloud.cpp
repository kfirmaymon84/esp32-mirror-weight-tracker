#include "cloud.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>

#include "config.h"
#include "history.h"
#include "timekeeper.h"

// Pending queue: append-only CSV of readings not yet synced ("ts,kg").
static const char *PENDING_FILE = "/pending.csv";

void cloudBegin() {
  // LittleFS is already mounted by historyBegin(); nothing else needed.
}

void cloudQueue(uint32_t ts, float kg) {
  File f = LittleFS.open(PENDING_FILE, "a");
  if (!f) {
    Serial.println("[cloud] queue open failed");
    return;
  }
  f.printf("%u,%.2f\n", (unsigned)ts, kg);
  f.close();
  Serial.printf("[cloud] queued %.2f kg (pending=%d)\n", kg, cloudPendingCount());
}

int cloudPendingCount() {
  // Guard the open: when the queue is empty the file doesn't exist, and opening
  // a missing file for read spams LittleFS "[E] does not exist" every loop.
  if (!LittleFS.exists(PENDING_FILE)) return 0;
  File f = LittleFS.open(PENDING_FILE, "r");
  if (!f) return 0;
  int n = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length()) n++;
  }
  f.close();
  return n;
}

bool cloudHasPending() { return cloudPendingCount() > 0; }

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
  if (!http.begin(client, CLOUD_URL)) return false;
  http.addHeader("Content-Type", "application/json");

  char body[96];
  snprintf(body, sizeof(body), "{\"token\":\"%s\",\"ts\":%u,\"kg\":%.2f}",
           CLOUD_TOKEN, (unsigned)ts, kg);
  int code = http.POST((uint8_t *)body, strlen(body));
  http.end();
  Serial.printf("[cloud] POST %.2f -> HTTP %d\n", kg, code);
  return code == 200 || code == 302;  // 302 = appended + redirecting = done
}

int cloudFlush() {
  if (!cloudHasPending()) return 0;

  // Read all pending readings into memory.
  static const int MAXQ = 64;
  uint32_t ts[MAXQ];
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
      for (int i = sent; i < nq; i++) f.printf("%u,%.2f\n", (unsigned)ts[i], kg[i]);
      f.close();
    }
  }
  Serial.printf("[cloud] synced %d/%d\n", sent, nq);
  return sent;
}

int cloudPull() {
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
  String url = String(CLOUD_URL) + "?token=" + CLOUD_TOKEN;
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
