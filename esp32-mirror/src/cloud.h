#pragma once
#include <stdint.h>

// Cloud: every user's weigh-ins live in their own account (Firebase Auth +
// Firestore, project in cloud_config.h). The user signs in (or creates the
// account) on the Wi-Fi setup page on their phone; the device keeps only a
// refresh token, never the password. All TLS to Google is certificate-verified.
//
// Readings are queued to LittleFS first, so nothing is lost while offline or
// signed out: the queue uploads once WiFi and an account are available. Each
// reading is stored as users/<uid>/readings/<epoch>, so a retry overwrites
// rather than duplicates.

enum CloudAuth {
  AUTH_OK,
  AUTH_BAD_LOGIN,        // wrong email or password
  AUTH_EMAIL_TAKEN,      // "new account" for an email that already has one
  AUTH_WEAK_PASSWORD,    // fewer than 6 characters
  AUTH_BAD_EMAIL,
  AUTH_TOO_MANY,         // Firebase throttled the attempts
  AUTH_NO_NETWORK,
  AUTH_FAILED,           // anything else (or accounts not set up in this build)
};

void cloudBegin();                  // load the saved account (NVS)
bool cloudAvailable();              // this build has a Firebase project
bool cloudConfigured();             // signed in on this device
bool cloudSignInLost();             // the account rejected the saved token: sign in again
const char *cloudEmail();           // signed-in (or last) email, "" if none

// Needs WiFi up (connects with the saved network if not). On success the
// account is saved; switching to a different account clears the local log.
CloudAuth cloudSignIn(const char *email, const char *password, bool create);
const char *cloudAuthText(CloudAuth r);
bool cloudSendPasswordReset(const char *email);
// Upload what's pending (best effort), then forget the account AND clear the
// local log + queue, so the next person doesn't see this user's weights.
void cloudSignOut();

void cloudQueue(uint32_t ts, float kg);   // add a reading to the pending queue
bool cloudHasPending();                    // anything to upload (and signed in)?
int  cloudPendingCount();
int  cloudFlush();                         // WiFi up, upload the queue, WiFi off; #sent
// Download the account's readings (cloud = source of truth) + keep anything
// still pending, replace the local log, reload it. #readings, or -1 on failure.
int  cloudPull();

// The user's settings, shared by all their devices (users/<uid>/settings/profile).
// Device-only things (touch calibration, paired scale) are not part of it.
struct UserSettings {
  bool lb = false;          // display unit
  int sound = 2;            // 0..3
  float goalKg = 0;         // 0 = no goal
  float heightCm = 0;       // 0 = not set
  uint32_t updated = 0;     // epoch of the change (newer side wins)
};
// Both connect WiFi if needed and leave it on (call wifiOff()).
bool cloudPushSettings(const UserSettings &s);
// false = unreachable; true with found = false = the account has none yet.
bool cloudPullSettings(UserSettings &s, bool &found);
