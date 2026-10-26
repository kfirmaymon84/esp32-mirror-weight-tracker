#pragma once

// Public half of the OTA signing key (ota/release.py genkey). Firmware
// downloaded by the automatic updater is installed only if its signature
// verifies against this key. Safe to publish; the private key is not here.
static const char OTA_PUBKEY_PEM[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEZox/KSNJ96D5vDgzNzY+/9J4rszK\n"
    "h34FJTi8G9KKMf1tZCY59b3VzaaEFSwsTpHlGb2UQfa+R5qPwziXl+C54A==\n"
    "-----END PUBLIC KEY-----\n"
;
