#pragma once
// Waveshare 1.75C schematic: LCD_TE is GPIO13. CO5300 35h/00h
// enables the vertical-blanking signal; the bundled GFX init leaves it off.
constexpr int VIDEO_TE_PIN = 13;
static volatile uint32_t videoTeRise = 0, videoTeCount = 0;
static volatile uint32_t videoTePeriod = 0, videoTeHigh = 0;
static uint32_t videoTeSamples[64], videoTeHighSamples[64];
static volatile unsigned videoTeSampleCount = 0;
static uint32_t videoTeMeasuredPeriod = 0;
struct VideoTeSnapshot { uint32_t count, rise; };
static VideoTeSnapshot snapshotVideoTe() {
  noInterrupts();
  VideoTeSnapshot s{videoTeCount, videoTeRise};
  interrupts();
  return s;
}

static void ARDUINO_ISR_ATTR videoTeEdge() {
  uint32_t now = micros();
  if (digitalRead(VIDEO_TE_PIN)) {
    if (videoTeRise) videoTePeriod = now - videoTeRise;
    videoTeRise = now;
    ++videoTeCount;
  } else {
    videoTeHigh = now - videoTeRise;
    unsigned n = videoTeSampleCount;
    if (videoTePeriod && n < 64) {
      videoTeSamples[n] = videoTePeriod;
      videoTeHighSamples[n] = videoTeHigh;
      videoTeSampleCount = n + 1;
    }
  }
}

static void beginVideoTe() {
  pinMode(VIDEO_TE_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(VIDEO_TE_PIN), videoTeEdge, CHANGE);
  bus.beginWrite();
  bus.writeC8D16(CO5300_W_SETTSL, 0);
  bus.writeC8D8(CO5300_WC_TEARON, 0);
  bus.endWrite();
  uint32_t started = millis();
  while (videoTeSampleCount < 64 && millis() - started < 1500) delay(1);
  if (videoTeSampleCount >= 32) {
    uint32_t sum = 0;
    for (unsigned i = 16; i < videoTeSampleCount; ++i) sum += videoTeSamples[i];
    videoTeMeasuredPeriod = sum / (videoTeSampleCount - 16);
    if (videoTeMeasuredPeriod < 10000 || videoTeMeasuredPeriod > 25000)
      videoTeMeasuredPeriod = 0;
  }
#ifdef VIDEO_PROFILE
  Serial.printf("te probe pin=%d samples=%u edges=%lu\n", VIDEO_TE_PIN,
      videoTeSampleCount, (unsigned long)videoTeCount);
  for (unsigned i = 0; i < videoTeSampleCount; ++i) {
    Serial.printf("te sample=%u period_us=%lu high_us=%lu\n", i,
        (unsigned long)videoTeSamples[i], (unsigned long)videoTeHighSamples[i]);
    delay(2); // startup-only output; no presentation clock has started yet
  }
#endif
}
