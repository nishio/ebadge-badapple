#pragma once
#ifdef VIDEO_PROFILE
// RAM-only measurements, also readable after an entire loop without a reader.
struct VideoProfile {
  uint32_t frames = 0, shown = 0, decodeMax = 0, drawMax = 0;
  uint32_t gapMax = 0, over50 = 0, over100 = 0, resyncs = 0;
  uint32_t start = 0, elapsed = 0, underruns = 0;
  uint32_t prepareMax = 0, tePhaseMax = 0, teMissed = 0, teFallback = 0;
  uint64_t prepareSum = 0;
  uint64_t decodeSum = 0, drawSum = 0, expandSum = 0, transferSum = 0;
};
static VideoProfile vp, vpLast;
static bool videoWaiting = false;
static uint32_t vpLoops = 0, vpLastPresented = 0;
static void profileReportOne(const char *label, const VideoProfile &p) {
  Serial.printf("profile %s loops=%lu frames=%lu shown=%lu elapsed_us=%lu resync=%lu underruns=%lu\n",
      label, (unsigned long)vpLoops, (unsigned long)p.frames, (unsigned long)p.shown,
      (unsigned long)p.elapsed, (unsigned long)p.resyncs, (unsigned long)p.underruns);
  Serial.printf("timing %s decode_avg_us=%lu max=%lu draw_avg_us=%lu max=%lu gap_max_us=%lu over50=%lu over100=%lu\n",
      label, (unsigned long)(p.frames ? p.decodeSum / p.frames : 0), (unsigned long)p.decodeMax,
      (unsigned long)(p.shown ? p.drawSum / p.shown : 0), (unsigned long)p.drawMax,
      (unsigned long)p.gapMax, (unsigned long)p.over50, (unsigned long)p.over100);
}
static void profileParts(const char *label, const VideoProfile &p) {
  Serial.printf("parts %s expand_avg_us=%lu transfer_avg_us=%lu\n", label,
      (unsigned long)(p.shown ? p.expandSum / p.shown : 0),
      (unsigned long)(p.shown ? p.transferSum / p.shown : 0));
  Serial.printf("sync %s prepare_avg_us=%lu prepare_max_us=%lu te_phase_max_us=%lu missed=%lu fallback=%lu\n",
      label, (unsigned long)(p.frames ? p.prepareSum / p.frames : 0),
      (unsigned long)p.prepareMax, (unsigned long)p.tePhaseMax,
      (unsigned long)p.teMissed, (unsigned long)p.teFallback);
}
static void profileReport() {
  profileReportOne("last", vpLast);
  profileParts("last", vpLast);
  profileReportOne("current", vp);
  profileParts("current", vp);
}
static void profileDecoded(uint32_t us) {
  ++vp.frames;
  vp.decodeSum += us;
  if (us > vp.decodeMax) vp.decodeMax = us;
}
static void profilePresented(uint32_t us) {
  ++vp.shown;
  vp.drawSum += us;
  if (us > vp.drawMax) vp.drawMax = us;
  uint32_t now = micros();
  if (vpLastPresented) {
    uint32_t gap = now - vpLastPresented;
    if (gap > vp.gapMax) vp.gapMax = gap;
    vp.over50 += gap > 50000;
    vp.over100 += gap > 100000;
  }
  vpLastPresented = now;
}
#endif
