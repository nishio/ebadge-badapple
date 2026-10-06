// Bad Apple!! player for the Waveshare ESP32-S3-Touch-AMOLED-1.75C.
// Plays the .bap clip (and, with -DVIDEO_AUDIO, its .aap soundtrack) written
// to the ffat partition by scripts/upload_video.sh and scripts/upload_av_audio.py.
#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <TouchDrvCSTXXX.hpp>
#include <pin_config.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <bap_decode.h>
#include <bap_render.h>
#include "video_profile.h"

Arduino_ESP32QSPI bus(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 display(&bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
#include "video_te.h"
TouchDrvCST92xx touch;

enum ScreenMode : uint8_t { IDLE_MODE, VIDEO_MODE };
// The clip lives in the ffat data partition, written by scripts/upload_video.sh.
// It survives sketch uploads, which only touch the app partitions below it.

bool touchOk, fingerDown = false, gestureDone = false;
volatile bool touchEvent = false;
float startX = 0, startY = 0;
uint32_t lastTouch = 0;
ScreenMode screenMode = IDLE_MODE;

const esp_partition_t *videoPart = nullptr;
bap_header videoInfo;
bool videoPresent = false, videoVerified = false;
bap_decoder *videoDecoder = nullptr;
uint8_t *videoFrame = nullptr;
uint16_t *videoRgb = nullptr;
int videoOriginX = 0, videoOriginY = 0, videoOutW = 0;
bool videoPrepared = false, videoSync = false;
uint32_t videoNextTe = 0, videoTeStride = 2;
uint32_t videoPreparedDecodeUs = 0, videoPreparedUs = 0;
uint16_t videoLut[BAP_LUT_MAX];   // grey ramp, stored byte-swapped for DMA
uint32_t videoIndex = 0, videoNextUs = 0, videoIntervalUs = 0, videoDropped = 0;
struct VideoSource { uint32_t pos; } videoSource;

void centered(const char *text, int y, int size, uint16_t color) {
  display.setTextSize(size);
  display.setTextColor(color);
  display.setCursor((LCD_WIDTH - int(strlen(text)) * 6 * size) / 2, y);
  display.print(text);
}

void drawIdle() {
  display.fillScreen(RGB565_BLACK);
  centered("Bad Apple!!", 170, 3, RGB565_WHITE);
  if (videoPresent) centered("tap to play", 230, 2, 0x8410);
  else {
    centered("no clip in flash", 222, 2, 0xFBE0);
    centered("see README: preparing the data", 250, 1, 0x8410);
  }
}

size_t videoRead(void *ctx, uint8_t *dst, size_t len) {
  VideoSource *s = static_cast<VideoSource *>(ctx);
  uint32_t left = videoInfo.payload_bytes - s->pos;
  size_t n = len < left ? len : left;
  if (n && esp_partition_read(videoPart, BAP_HEADER_BYTES + s->pos, dst, n) != ESP_OK)
    return 0;
  s->pos += n;
  return n;
}

#include "video_prefetch.h"
#ifdef VIDEO_AUDIO
#include "video_audio.h"
#endif

// Reading the whole payload once is cheaper than discovering a corrupt clip as
// a glitch on screen. Done on first use, not at boot, so startup stays quick.
bool videoPayloadOk() {
  if (videoVerified) return true;
  uint8_t chunk[1024];
  uint32_t crc = 0, pos = 0, start = millis();
  while (pos < videoInfo.payload_bytes) {
    size_t n = videoInfo.payload_bytes - pos;
    if (n > sizeof(chunk)) n = sizeof(chunk);
    if (esp_partition_read(videoPart, BAP_HEADER_BYTES + pos, chunk, n) != ESP_OK) return false;
    crc = bap_crc32(crc, chunk, n);
    pos += n;
  }
  videoVerified = crc == videoInfo.crc32;
  Serial.printf("video crc %s in %lu ms\n", videoVerified ? "ok" : "MISMATCH",
                static_cast<unsigned long>(millis() - start));
  return videoVerified;
}

void stopVideo() {
#ifdef VIDEO_AUDIO
  avShutdown();
#endif
  videoPrepared = false;
  stopVideoPrefetch();
  if (videoDecoder) { bap_close(videoDecoder); videoDecoder = nullptr; }
  free(videoFrame); videoFrame = nullptr;
  free(videoRgb); videoRgb = nullptr;
}

// Returns false and leaves the screen alone when there is nothing to play.
bool startVideo() {
  if (!videoPresent || !videoPayloadOk()) return false;
  videoOutW = videoInfo.store_w * videoInfo.scale;
  int outH = videoInfo.store_h * videoInfo.scale;
  // Match the CO5300 rectangle alignment used by Waveshare's LVGL rounder:
  // even starts and odd inclusive ends. The full RGB rectangle must satisfy the same rule.
  if ((videoOutW & 1) || (outH & 1)) {
    Serial.println("video needs even output width and height for CO5300");
    return false;
  }
  videoOriginX = ((LCD_WIDTH - videoOutW) / 2) & ~1;
  videoOriginY = ((LCD_HEIGHT - outH) / 2) & ~1;

  // Prepare the entire RGB frame before the next TE edge. Only the final DMA
  // burst touches the panel; no expansion pauses between partial rectangles.
  videoFrame = static_cast<uint8_t *>(heap_caps_malloc(videoInfo.frame_bytes,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  videoRgb = static_cast<uint16_t *>(heap_caps_malloc(
      videoOutW * outH * sizeof(uint16_t),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  bap_build_lut(videoLut, videoInfo.bpp);
  Serial.printf("memory frame_internal=%d rgb_internal=%d cpu_mhz=%lu free_internal=%u\n",
      esp_ptr_internal(videoFrame), esp_ptr_internal(videoRgb),
      (unsigned long)getCpuFrequencyMhz(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  videoSource.pos = 0;
  videoDecoder = bap_open(&videoInfo, videoRead, &videoSource);
  if (!videoFrame || !videoRgb || !videoDecoder) {
    Serial.println("video full-frame allocation failed"); stopVideo(); return false;
  }
  if (!startVideoPrefetch()) { stopVideo(); return false; }
  Serial.printf("prefetch slots=%u ready=%u decode_core=0 render_core=%d\n",
      VIDEO_PREFETCH_SLOTS, (unsigned)uxQueueMessagesWaiting(videoReadyQueue), xPortGetCoreID());
#ifdef VIDEO_AUDIO
  if (!avPrepare()) { Serial.println("av prepare failed"); stopVideo(); return false; }
#endif
  screenMode = VIDEO_MODE;
  display.fillScreen(RGB565_BLACK);
#ifdef VIDEO_AUDIO
  avControls();
#endif
  videoIndex = videoDropped = 0;
  videoIntervalUs = 1000000ULL * videoInfo.fps_den / videoInfo.fps_num;
  videoSync = videoTeMeasuredPeriod != 0;
  videoPrepared = false;
  if (videoSync) {
    videoTeStride = max(uint32_t(1), (videoIntervalUs + videoTeMeasuredPeriod / 2) / videoTeMeasuredPeriod);
    videoIntervalUs = videoTeStride * videoTeMeasuredPeriod;
    videoNextTe = snapshotVideoTe().count + 2;
  }
  videoNextUs = micros();
#ifdef VIDEO_PROFILE
  vp = {}; vpLast = {}; vpLoops = 0; vpLastPresented = 0; videoWaiting = false;
  vp.start = 0;
#endif
  Serial.printf("video %ux%u %ubpp x%u -> %dx%d at (%d,%d) %u frames nominal=%u fps full_rgb_bytes=%u\n",
                videoInfo.store_w, videoInfo.store_h, videoInfo.bpp, videoInfo.scale,
                videoOutW, outH, videoOriginX, videoOriginY, videoInfo.frame_count,
                videoInfo.fps_num / videoInfo.fps_den, videoOutW * outH * sizeof(uint16_t));
  Serial.printf("video sync=%d te_period_us=%lu stride=%lu interval_us=%lu\n", videoSync,
      (unsigned long)videoTeMeasuredPeriod, (unsigned long)videoTeStride, (unsigned long)videoIntervalUs);
  return true;
}

void leaveVideo() {
#ifdef VIDEO_AUDIO
  avReport("leaving");
#endif
  Serial.printf("video stopped at frame %lu, %lu dropped\n",
                static_cast<unsigned long>(videoIndex),
                static_cast<unsigned long>(videoDropped));
  stopVideo();
  screenMode = IDLE_MODE;
  drawIdle();
}

void serviceVideo() {
#ifdef VIDEO_AUDIO
  if(avError.load()) { leaveVideo(); return; }
  if(videoIndex==videoInfo.frame_count) {
    if(avDma.load()>=avHeader[3]) {
      avReport("complete");
#ifdef VIDEO_PROFILE
      vp.elapsed=micros()-vp.start;vpLast=vp;++vpLoops;
      profileReport();
#endif
      leaveVideo();
    }
    return;
  }
#endif
  VideoTeSnapshot te = snapshotVideoTe();
  // Read the clock AFTER the ISR snapshot. Otherwise an edge between the two
  // reads makes (now - rise) underflow and falsely declares the signal lost.
  uint32_t now = micros();
  if (videoSync && now - te.rise > 3 * videoTeMeasuredPeriod) {
    // A disconnected/stopped TE must not hang video or gesture handling.
    videoSync = false;
    videoNextUs = now;
#ifdef VIDEO_PROFILE
    ++vp.teFallback;
#endif
  }
#ifdef VIDEO_PROFILE
  bool due = videoSync ? static_cast<int32_t>(te.count - videoNextTe) >= 0
                       : static_cast<int32_t>(now - videoNextUs) >= 0;
#endif
  uint32_t expectedIndex = videoIndex == videoInfo.frame_count ? 0 : videoIndex;

  // Expand ahead of the deadline into one internal DMA frame. The producer
  // owns the compressed stream; its ready slot is returned after the copy.
  if (!videoPrepared) {
    ReadyVideoFrame ready;
    if (xQueueReceive(videoReadyQueue, &ready, 0) != pdTRUE) {
#ifdef VIDEO_PROFILE
      if (due && !videoWaiting) { ++vp.underruns; videoWaiting = true; }
#endif
      return;
    }
    if (ready.status != 0 || ready.index != expectedIndex) {
      Serial.printf("video decode/order error expected=%lu got=%lu status=%d\n",
          (unsigned long)expectedIndex, (unsigned long)ready.index, ready.status);
      leaveVideo(); return;
    }
    uint32_t prepareStart = micros();
#ifdef VIDEO_AUDIO
    memcpy(videoRgb,ready.pixels,videoOutW*videoInfo.store_h*videoInfo.scale*sizeof(uint16_t));
    xQueueSend(videoFreeQueue,&ready.pixels,0);
    avExpandSum+=ready.expandUs;avExpandMax=max(avExpandMax,ready.expandUs);
    uint32_t producerUs=ready.decodeUs+ready.expandUs;
    avProducerSum+=producerUs;avProducerMax=max(avProducerMax,producerUs);
    if(uint64_t(producerUs)*videoInfo.fps_num>1000000ULL*videoInfo.fps_den)++avProducerOverBudget;
#else
    memcpy(videoFrame, ready.pixels, videoInfo.frame_bytes);
    xQueueSend(videoFreeQueue, &ready.pixels, 0);
    for (int y = 0; y < videoInfo.store_h; ++y)
      bap_expand_row(videoFrame + y * videoInfo.stride,
                     videoRgb + y * videoInfo.scale * videoOutW,
                     videoInfo.store_w, videoInfo.bpp, videoInfo.scale,
                     videoOutW, videoLut);
#endif
    videoPrepared = true;
    videoPreparedDecodeUs = ready.decodeUs;
    videoPreparedUs = micros() - prepareStart;
#ifdef VIDEO_PROFILE
    videoWaiting = false;
#endif
  }

#ifdef VIDEO_AUDIO
  // Original 30 fps timeline follows I2S. Wait for the first eligible TE edge;
  // most intervals are two edges, occasionally one, preserving every frame.
  if(avRunning.load() && uint64_t(avClockSamples())*videoInfo.fps_num <
      uint64_t(videoIndex)*22050*videoInfo.fps_den)return;
#endif
  te = snapshotVideoTe(); now = micros();
  if (videoSync) {
    if (static_cast<int32_t>(te.count - videoNextTe) < 0) return;
    uint32_t phase = now - te.rise;
    // Never begin a transfer at an arbitrary late position in panel scanning.
    if (phase > 500) {
#ifdef VIDEO_PROFILE
#ifdef VIDEO_AUDIO
      // Becoming due mid-scan normally defers to the next TE, not a missed deadline.
      int64_t lateness=int64_t(uint64_t(avClockSamples())*1000000/22050)-
          int64_t(uint64_t(videoIndex)*1000000*videoInfo.fps_den/videoInfo.fps_num);
      if(lateness>int64_t(videoTeMeasuredPeriod)+500)++vp.teMissed;
#else
      ++vp.teMissed;
#endif
#endif
      videoNextTe = te.count + 1;
      return;
    }
  } else if (static_cast<int32_t>(now - videoNextUs) < 0) return;

  bool wrapped = videoIndex == videoInfo.frame_count;
  if (wrapped) {
#ifdef VIDEO_PROFILE
    vp.elapsed = now - vp.start;
    vpLast = vp;
    ++vpLoops;
    vp = {}; vp.start = now;
#endif
    videoIndex = 0;
  }
#ifdef VIDEO_PROFILE
  if (!vp.start) vp.start = now;
  profileDecoded(videoPreparedDecodeUs);
  vp.prepareSum += videoPreparedUs;
  vp.expandSum += videoPreparedUs;
  if (videoPreparedUs > vp.prepareMax) vp.prepareMax = videoPreparedUs;
  if (videoSync && now - te.rise > vp.tePhaseMax) vp.tePhaseMax = now - te.rise;
#endif
#ifdef VIDEO_AUDIO
  if(!avRunning.load() && !avStart()){ leaveVideo(); return; }
  avRecordFrame(videoIndex);
#endif
#ifdef VIDEO_PROFILE
  uint32_t drawStart = micros();
#endif
  display.draw16bitBeRGBBitmap(videoOriginX, videoOriginY, videoRgb,
                               videoOutW, videoInfo.store_h * videoInfo.scale);
#ifdef VIDEO_AUDIO
  avRecordQueue(videoIndex,(uint32_t)uxQueueMessagesWaiting(videoReadyQueue));
#endif
#ifdef VIDEO_PROFILE
  uint32_t drawUs = micros() - drawStart;
  vp.transferSum += drawUs;
  profilePresented(drawUs);
#endif
  videoPrepared = false;
  ++videoIndex;
  if (videoSync) {
#ifdef VIDEO_AUDIO
    videoNextTe = te.count + 1;
#else
    videoNextTe = te.count + videoTeStride;
#endif
    videoNextUs = te.rise + videoIntervalUs;
  } else {
    videoNextUs += videoIntervalUs;
    if (static_cast<int32_t>(micros() - videoNextUs) > static_cast<int32_t>(videoIntervalUs)) {
      videoNextUs = micros() + videoIntervalUs;
#ifdef VIDEO_PROFILE
      ++vp.resyncs;
#endif
    }
  }
#ifdef VIDEO_PROFILE
  vp.elapsed = micros() - vp.start;
  if (wrapped) { // report after the burst, never inside the TE start window
    profileReportOne("last", vpLast);
    profileParts("last", vpLast);
  }
#endif
}

// A tap or swipe on the idle screen plays; a swipe during playback stops.
bool handleGesture(float dx, float dy, bool released) {
  bool swipe = fabsf(dx) >= 60 || fabsf(dy) >= 60;
  if (screenMode == VIDEO_MODE) {
    if (swipe) { leaveVideo(); return true; }
    return false;
  }
  if (swipe || released) { startVideo(); return true; }
  return false;
}

void setup() {
  Serial.begin(115200);
  // Never let a log line stall the playback loop. On USB-Serial-JTAG, write()
  // blocks while the host is plugged in but nothing is reading the port: the TX
  // ring fills and the core retries for up to 20 x tx_timeout_ms = 2 s before
  // giving up (HWCDC.cpp, "host backpressure"). A zero timeout drops the line
  // instead of waiting; with a reader attached the ring drains as usual.
  Serial.setTxTimeoutMs(0);
#ifdef VIDEO_AUDIO
  pinMode(PA,OUTPUT);digitalWrite(PA,LOW);
#endif
  Wire.begin(IIC_SDA, IIC_SCL);
  if (!display.begin()) { Serial.println("ERROR display init"); while (true) delay(1000); }
  display.setBrightness(128);
  beginVideoTe();
  touch.setPins(TP_RST, TP_INT);
  touchOk = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);
  if (touchOk) {
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setMirrorXY(true, true);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), [](){ touchEvent = true; }, FALLING);
  }
  videoPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                       ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat");
  if (videoPart) Serial.printf("ffat address=%08lx size=%lu flash=%lu\n",
      (unsigned long)videoPart->address, (unsigned long)videoPart->size,
      (unsigned long)ESP.getFlashChipSize());
  uint8_t head[BAP_HEADER_BYTES];
  videoPresent = videoPart &&
                 esp_partition_read(videoPart, 0, head, sizeof(head)) == ESP_OK &&
                 bap_parse_header(head, sizeof(head), &videoInfo) == 0 &&
                 BAP_HEADER_BYTES + videoInfo.payload_bytes <= videoPart->size;
  drawIdle();
  Serial.printf("Bad Apple player ready; touch=%d PSRAM=%u video=%d\n", touchOk,
                ESP.getPsramSize(), videoPresent);
#ifndef NO_AUTOSTART
  startVideo();   // -DNO_AUTOSTART waits on the idle screen for a tap instead
#endif
}

void loop() {
  uint32_t now = millis();
  if (touchOk && touchEvent) {
    touchEvent = false;
    int16_t x[5], y[5];
    uint8_t count = touch.getPoint(x,y,5);
    if (count) {
      lastTouch = now;
      if (!fingerDown) {
        fingerDown = true; gestureDone = count != 1;
        startX = x[0]; startY = y[0];
#ifdef VIDEO_AUDIO
        if(screenMode==VIDEO_MODE && count==1 &&
           x[0]>=AV_MUTE_X && x[0]<AV_MUTE_X+AV_MUTE_W &&
           y[0]>=AV_MUTE_Y && y[0]<AV_MUTE_Y+AV_MUTE_H) {
          avSetMuted(!avOutput.muted);gestureDone=true;
        }
        if(screenMode==VIDEO_MODE && count==1 && y[0]>=388 && y[0]<=430) {
          if(x[0]>=110 && x[0]<160){avChangeVolume(-5);gestureDone=true;}
          else if(x[0]>=178 && x[0]<288){leaveVideo();gestureDone=true;}
          else if(x[0]>=306 && x[0]<=356){avChangeVolume(5);gestureDone=true;}
        }
#endif
      }
      if (count != 1) gestureDone = true;
      if (!gestureDone && handleGesture(x[0]-startX, y[0]-startY, false)) gestureDone = true;
    }
    // An empty read can be an ACK without new coordinates. Let the inactivity
    // timeout end the gesture, instead of retriggering during the same drag.
  }
  if (fingerDown && now-lastTouch > 200) {
    fingerDown = false;
    if (!gestureDone && screenMode == IDLE_MODE) handleGesture(0, 0, true);  // a tap
  }
  if (Serial.available()) {
    while (Serial.available()) {
      char command = Serial.read();
#ifdef VIDEO_AUDIO
      if(command=='+')avChangeVolume(5);
      if(command=='-')avChangeVolume(-5);
      if(command=='m')avSetMuted(true); // Serial can mute; unmute is a deliberate screen tap.
      if(command=='?')avReport("status");
#endif
      if (command == 's' && screenMode == VIDEO_MODE) leaveVideo();
      if (command == 'p' && screenMode != VIDEO_MODE) startVideo();
    }
#ifdef VIDEO_PROFILE
    profileReport();
    Serial.printf("health mode=%u internal=%u psram=%u worker_done=%d\n",
        (unsigned)screenMode, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM), videoWorkerDone.load());
#endif
  }
  if (screenMode == VIDEO_MODE) { serviceVideo(); return; }
  delay(2);
}
