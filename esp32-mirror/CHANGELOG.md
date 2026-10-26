# Firmware changelog

One section per firmware version (`FW_VERSION` / `FW_BUILD` in `src/config.h`),
newest first. Both boards (3.5" e32r35t, 4" e32r40t) always share a version.

How to use it:
- While working, add each user-visible change under **Unreleased**.
- Before a release: bump `FW_VERSION` and `FW_BUILD`, then rename "Unreleased" to
  `## Firmware <version> (build <build>) — <date>`.
- `py -3 ota/release.py beta` refuses to publish without that section, and uses
  it as the release notes on GitHub (promote carries the same notes to stable).

## Unreleased

## Firmware 7 (build 7) — 2026-10-26

- Fixed: changing Wi-Fi on a signed-in device showed "Enter your password too"
  (the setup page pre-fills the email). Leaving the password empty now keeps the
  account as it is, silently.
- Releases now come with release notes from this changelog.

## Firmware 6 (build 6) — 2026-10-26

First release published on GitHub (beta channel).

- **OKOK scales only.** Xiaomi S200 support removed; every unit runs the same
  public firmware, and no build contains personal data.
- **Update channels.** Stable (everyone) and beta (new versions first). Beta is
  switched over USB only (`b`); there's no on-screen button.
- **Overnight updates.** Automatic checks run 02:00–05:00 local time while nobody
  is around; a device that missed 3 nights may update at any quiet moment.
- **Settings → Firmware:** version, and **Check now**, which installs at once.
- **Time zone** picked from the phone's own zone on the setup page (40 zones,
  daylight saving checked against the IANA database); **Settings → Time zone**
  to change it.
- Phone pages opened from Settings can be **cancelled with a tap**; the setup
  screen points to 192.168.4.1 if the page doesn't pop up.
- **Wi-Fi reset needs a second tap** within 4 seconds.
- USB: `k` forgets a build that was rolled back.

## Firmware 5 (build 5) — 2026-10-26

Flashed over USB only (never published).

- **Accounts.** Sign in or create an account on the phone setup page; weigh-ins
  and settings (units, sound, goal, height) live in the account (Firebase) and
  follow you to a new device. The device keeps a token, never the password; TLS
  to Google is certificate-verified.
- **Settings → Account:** sync state, Sync now, Sign out (clears the device).
- Readings carry a readable date (`at`); the history downloads in small pages,
  and a cut-short download is never kept as the history.
- **Signed automatic updates** from GitHub releases, with rollback: a version
  that crashes on startup is undone and never retried.
- **Settings → Scale:** scan for nearby OKOK scales and pick yours.
- Every stand on the scale is shown; the scale's repeat of its last locked
  weight isn't counted as a new weigh-in; the same weight within a minute isn't
  saved twice.
- **Goal ring and %** measure the journey from the first weigh-in.
- No boot melody during quiet hours; no error lines in the log of a new device.

## Firmware 4 — 2026-10-20 … 2026-10-22

- **Halo UI:** icon rail (Home, Trends, Settings), goal ring, gloss palette for
  mirror film.
- **3.5" E32R35T** board supported alongside the 4".
- **BMI** on Home, from the height in Settings.
- **Boot melody** ("Rise") and working click / accept / reject sounds.
- **Trends:** 14D / 30D / 90D / All, lines from each weigh-in to the trend, legend;
  same-day readings count as one day in the trend.
- Fixed: a false "screen held at boot" erased the saved Wi-Fi.
- Readings taken before the clock was set get their real time once it is.
- Battery support removed (USB powered).

## Firmware 3 — 2026-10-20

- Ported to the **4" E32R40T** board (ST7796S, 480×320) with an **OKOK broadcast
  scale**.
- Settings tiles: sync, units, sound (Off/Low/Mid/High), whole-number goal, touch
  calibration, scale, update, Wi-Fi; partial screen redraws.
- Google Sheet chosen per device on the setup page (nothing compiled in).

## Firmware 2 — 2026-09-10

- First version: 2.8" display, Xiaomi S200 BLE reader, Google Sheet sync.
