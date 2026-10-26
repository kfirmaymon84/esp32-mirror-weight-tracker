#pragma once
#include <functional>
#include <stdint.h>

// Automatic online updates (product builds, -D OTA_BOARD="e32r35t" etc.).
//
// The device fetches <OTA_BASE_URL>/<OTA_BOARD>.json — by default the latest
// GitHub release of this project — and installs the firmware it points to when:
//   * its "board" is this board and its "build" is higher than FW_BUILD,
//   * its ECDSA P-256 signature over "board:build:sha256" verifies against the
//     public key in ota_pubkey.h (only ota/release.py, with the private key,
//     can produce one), and
//   * the downloaded image hashes to that signed sha256.
// Anything else is discarded before the device switches to it.
//
// Rollback: a new image boots "pending". otaConfirmHealthy() (called after the
// firmware has run normally for a while) marks it valid; if it crashes or
// restarts first, the bootloader returns to the previous firmware and that
// build is remembered as bad so it isn't downloaded again.

struct OtaRelease {
  int build = 0;
  char version[16] = "";
  uint32_t size = 0;
  char sha256[65] = "";
  char url[256] = "";
};

enum OtaResult { OTA_UP_TO_DATE, OTA_AVAILABLE, OTA_FAILED };

bool otaAutoEnabled();                 // false on builds without OTA_BOARD
void otaBootBegin();                   // first thing in setup(): rollback bookkeeping
void otaConfirmHealthy();              // mark the running image good (idempotent)

// Channel: stable (default) = <base>/latest/download, the releases everyone
// gets; beta = also the "beta" pre-release, which ota/release.py publishes first
// and later promotes to stable. A beta device takes whichever build is newer.
bool otaBeta();
void otaSetBeta(bool on);
void otaForgetBad();                   // retry a build that was rolled back (dev, USB)

// May an automatic check run now? Overnight (02:00-05:00 local), or any time
// once 3 days have passed without a successful check (a device that's off at
// night). Without a clock: yes (the caller's idle rules still apply).
bool otaWindowOpen();

// Both connect WiFi themselves (saved network) and leave it on; call wifiOff().
OtaResult otaCheck(OtaRelease &rel);
// Download + verify + switch the boot slot. On success the caller restarts.
// progress(pct) is called while downloading (also a chance to feed watchdogs).
bool otaInstall(const OtaRelease &rel, std::function<void(int pct)> progress);
