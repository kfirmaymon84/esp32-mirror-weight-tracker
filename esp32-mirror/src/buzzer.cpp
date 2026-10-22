#include "buzzer.h"

#include <Arduino.h>
#include <driver/dac.h>
#include <driver/rtc_io.h>
#include <math.h>

static const int CH = 4;      // LEDC channel (avoid 0 which tone()/others grab)
static const int RES = 10;    // duty resolution bits (0..1023)
static int level = SOUND_MID;
static int s_pin = -1;        // speaker pin (for the DAC melodies)

// Duty per level (of 1024). Fundamental amplitude ~ sin(pi * duty):
//   Low 2% ≈ -24 dB, Mid 8% ≈ -12 dB, High 50% = full.
static const int DUTY[4] = { 0, 20, 82, 512 };

void buzzerBegin(int pin, int enablePin) {
  if (enablePin >= 0) {             // speaker amplifier: enable is active LOW
    pinMode(enablePin, OUTPUT);
    digitalWrite(enablePin, LOW);
  }
  ledcSetup(CH, 2000, RES);
  ledcAttachPin(pin, CH);
  ledcWrite(CH, 0);
  s_pin = pin;
}

void buzzerSetLevel(int l) { level = (l < SOUND_OFF || l > SOUND_HIGH) ? SOUND_MID : l; }
int buzzerLevel() { return level; }
const char *buzzerLevelName(int l) {
  static const char *n[4] = { "Off", "Low", "Mid", "High" };
  return (l >= 0 && l <= 3) ? n[l] : "?";
}

void buzzerTone(int freqHz, int durationMs) {
  if (level == SOUND_OFF) { delay(durationMs); return; }
  if (freqHz <= 0) {
    ledcWrite(CH, 0);
    delay(durationMs);
    return;
  }
  ledcWriteTone(CH, freqHz);
  ledcWrite(CH, DUTY[level]);     // duty sets the volume
  delay(durationMs);
  ledcWrite(CH, 0);
}

void buzzerAccept() {
  buzzerTone(2200, 80);
  buzzerTone(2800, 120);
}

void buzzerReject() {
  buzzerTone(700, 250);
}

void buzzerClick() {
  buzzerTone(3200, 12);
}

// ── Melodies (DAC sine synthesis) ───────────────────────────────────────────
// Each note: a sine voice with an attack, an exponential decay (tau), an optional
// pitch glide, and a tone colour. Notes overlap, so chords ring together.
enum Timbre : uint8_t {
  T_WARM,      // sine + a little 2nd harmonic (soft bell)
  T_PURE,      // plain sine
  T_GLASS,     // + an inharmonic partial at 2.76x (struck glass)
  T_MARIMBA,   // + a 4x partial, for short woody plucks
  T_CHORUS,    // two voices ~7 cents apart (gentle shimmer)
};
struct Note {
  uint16_t atMs, hz, tauMs;
  float amp;
  Timbre timbre;        // defaults to T_WARM
  uint16_t attackMs;    // 0 = 6 ms
  uint16_t endHz;       // 0 = no glide
  uint16_t glideMs;     // glide hz -> endHz over this long
};
struct Melody { const char *name; const Note *notes; uint8_t n; uint16_t lenMs; };

// "Startup" set, modelled on a reference startup sound: octave leaps on C,
// 250 ms apart, in pure sine tones. The "bell" notes add a partial two octaves
// up that fades fast (the glassy ping), and the last bell rings on with a soft
// reverb-like tail. "hi" variants sit an octave up, where the small speaker is
// louder. Notes: C4 262 C5 523 C6 1047 G6 1568 C7 2093 (C8 4186 as a partial).
#define TONE(at, hz, tau, amp) {at, hz, tau, amp, T_PURE, 4}
#define BELL(at, hz, tau, amp) \
  {at, hz, tau, amp, T_PURE, 4}, {at, (uint16_t)(4 * (hz)), (uint16_t)((tau) / 3), 0.9f * (amp), T_PURE, 2}
#define TAIL(at, hz) {at, hz, 700, 0.12f, T_PURE, 150}
static const Note M_STARTUP[] = {        // boo-BING-ding-ding, boo-BING ... bing
  TONE(0, 262, 180, 1.0f), BELL(250, 523, 110, 0.75f), TONE(500, 1047, 230, 0.8f),
  TONE(750, 1047, 230, 0.8f), TONE(1000, 262, 180, 1.0f), BELL(1250, 523, 140, 0.8f),
  TAIL(1250, 523), BELL(1750, 523, 110, 0.3f)};
static const Note M_STARTUP_HI[] = {     // the same, an octave up
  TONE(0, 523, 180, 0.9f), BELL(250, 1047, 110, 0.7f), TONE(500, 2093, 230, 0.65f),
  TONE(750, 2093, 230, 0.65f), TONE(1000, 523, 180, 0.9f), BELL(1250, 1047, 140, 0.75f),
  TAIL(1250, 1047), BELL(1750, 1047, 110, 0.3f)};
static const Note M_QUICK[] = {          // one bar, faster, landing on the bell
  TONE(0, 262, 140, 1.0f), BELL(170, 523, 100, 0.75f), TONE(340, 1047, 180, 0.8f),
  TONE(510, 1047, 180, 0.8f), BELL(680, 523, 170, 0.85f), TAIL(680, 523)};
static const Note M_QUICK_HI[] = {       // the quick one, an octave up
  TONE(0, 523, 140, 0.9f), BELL(170, 1047, 100, 0.7f), TONE(340, 2093, 180, 0.65f),
  TONE(510, 2093, 180, 0.65f), BELL(680, 1047, 170, 0.8f), TAIL(680, 1047)};
static const Note M_RISE[] = {           // climbs through the fifth, lands on the bell
  TONE(0, 523, 160, 0.9f), BELL(200, 1047, 110, 0.7f), TONE(400, 1568, 200, 0.65f),
  TONE(600, 2093, 220, 0.6f), BELL(850, 1047, 170, 0.8f), TAIL(850, 1047)};
static const Note M_PING[] = {           // the short take: C6, ring up to C7, answer
  TONE(0, 1047, 120, 0.85f), TONE(100, 2093, 350, 0.7f), TONE(350, 1047, 250, 0.5f),
  TAIL(100, 2093)};
#undef TONE
#undef BELL
#undef TAIL

#define NOTES(a) (uint8_t)(sizeof(a) / sizeof((a)[0]))
static const Melody MELODIES[] = {
  {"Startup", M_STARTUP, NOTES(M_STARTUP), 2500},   {"Startup hi", M_STARTUP_HI, NOTES(M_STARTUP_HI), 2500},
  {"Quick", M_QUICK, NOTES(M_QUICK), 1500},         {"Quick hi", M_QUICK_HI, NOTES(M_QUICK_HI), 1500},
  {"Rise", M_RISE, NOTES(M_RISE), 1700},            {"Ping", M_PING, NOTES(M_PING), 1200},
};
static const int N_MELODIES = sizeof(MELODIES) / sizeof(MELODIES[0]);
static const int BOOT_MELODY = 4;    // "Rise" (picked on the audition app)

int buzzerMelodyCount() { return N_MELODIES; }
const char *buzzerMelodyName(int id) {
  return (id >= 0 && id < N_MELODIES) ? MELODIES[id].name : "?";
}

void buzzerBoot() { buzzerMelody(BOOT_MELODY); }

void buzzerMelody(int id) {
  if (id < 0 || id >= N_MELODIES || level == SOUND_OFF) return;
  const Melody &m = MELODIES[id];

  // Only GPIO25/26 have a DAC; anywhere else, play the notes as square tones.
  dac_channel_t ch = s_pin == 25 ? DAC_CHANNEL_1 : s_pin == 26 ? DAC_CHANNEL_2
                                                               : DAC_CHANNEL_MAX;
  if (ch == DAC_CHANNEL_MAX) {
    for (int i = 0; i < m.n; i++) {
      int gap = (i + 1 < m.n) ? m.notes[i + 1].atMs - m.notes[i].atMs : 250;
      buzzerTone(m.notes[i].hz, gap > 0 ? gap : 120);
    }
    return;
  }

  static int8_t SINE[256];
  static bool sineReady = false;
  if (!sineReady) {
    for (int i = 0; i < 256; i++) SINE[i] = (int8_t)lroundf(127.0f * sinf(i * 6.2831853f / 256));
    sineReady = true;
  }

  const int SR = 12000;      // samples/s: 7 voices fit the 83 us budget; top partial ~5.3 kHz
  const float GAIN[4] = {0, 0.16f, 0.32f, 0.55f};   // per volume level
  const float PHASE_PER_HZ = 4294967296.0f / SR;
  const int MAXN = 12;
  int n = m.n < MAXN ? m.n : MAXN;
  uint32_t ph[MAXN] = {0}, ph2[MAXN] = {0};
  float env[MAXN], dec[MAXN], ratio2[MAXN];
  int start[MAXN], attack[MAXN], glide[MAXN];
  for (int i = 0; i < n; i++) {
    const Note &nt = m.notes[i];
    dec[i] = expf(-1000.0f / (nt.tauMs * (float)SR));
    start[i] = nt.atMs * SR / 1000;
    attack[i] = (nt.attackMs ? nt.attackMs : 6) * SR / 1000;
    glide[i] = (nt.endHz && nt.glideMs) ? nt.glideMs * SR / 1000 : 0;
    ratio2[i] = nt.timbre == T_GLASS ? 2.76f : nt.timbre == T_MARIMBA ? 4.0f
              : nt.timbre == T_CHORUS ? 1.004f : 2.0f;
    env[i] = 0;
  }

  // Hand the pin from the LEDC to the DAC. Ramp 0 -> mid-scale slowly so the
  // amplifier doesn't "pop", play, then ramp back down and return the pin.
  ledcWrite(CH, 0);
  ledcDetachPin(s_pin);
  dac_output_enable(ch);
  for (int v = 0; v <= 128; v += 2) { dac_output_voltage(ch, v); delayMicroseconds(400); }

  const float g = GAIN[level];
  const int total = (int)((uint32_t)m.lenMs * SR / 1000);
  const uint32_t t0 = micros();
  for (int s = 0; s < total; s++) {
    float acc = 0;
    for (int i = 0; i < n; i++) {
      const Note &nt = m.notes[i];
      int k = s - start[i];
      if (k < 0) continue;
      float e;
      if (k < attack[i]) e = (float)k / attack[i];
      else { env[i] = (k == attack[i]) ? 1.0f : env[i] * dec[i]; e = env[i]; }
      if (e < 0.008f && k > attack[i]) continue;   // below the 8-bit DAC's step

      float hz = nt.hz;                              // pitch glide
      if (glide[i]) hz = k < glide[i] ? nt.hz + (nt.endHz - nt.hz) * (float)k / glide[i]
                                      : nt.endHz;
      uint32_t inc = (uint32_t)(hz * PHASE_PER_HZ);
      uint32_t p = ph[i], p2 = ph2[i];
      float v = SINE[p >> 24];
      switch (nt.timbre) {
        case T_WARM:    v += 0.18f * SINE[(p << 1) >> 24]; break;
        case T_PURE:    break;
        case T_GLASS:   v += 0.22f * SINE[p2 >> 24]; break;
        case T_MARIMBA: v += 0.25f * SINE[p2 >> 24]; break;
        case T_CHORUS:  v = 0.5f * (v + SINE[p2 >> 24]); break;
      }
      acc += nt.amp * e * v;
      ph[i] = p + inc;
      ph2[i] = p2 + (uint32_t)(inc * ratio2[i]);
    }
    int v = 128 + (int)lroundf(acc * g);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    while ((int32_t)(micros() - (t0 + (uint32_t)((uint64_t)s * 1000000 / SR))) < 0) {}
    dac_output_voltage(ch, (uint8_t)v);
  }

  for (int v = 128; v >= 0; v -= 2) { dac_output_voltage(ch, v); delayMicroseconds(400); }
  dac_output_disable(ch);
  rtc_gpio_deinit((gpio_num_t)s_pin);   // DAC left the pad on the RTC mux: give it back
  ledcAttachPin(s_pin, CH);             // to the GPIO matrix, or the tones go silent
  ledcWrite(CH, 0);
}
