#pragma once
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// One producer owns the sequential decoder on core 0. The UI on core 1 only
// copies a ready frame to internal RAM and renders it. Eight PSRAM slots absorb
// variable decode work; the producer blocks when the queue is full.
struct ReadyVideoFrame { uint8_t *pixels; uint32_t decodeUs; int status; uint32_t index;
#ifdef VIDEO_AUDIO
  uint32_t expandUs;
#endif
};
constexpr unsigned VIDEO_PREFETCH_SLOTS = 8;
static QueueHandle_t videoFreeQueue = nullptr, videoReadyQueue = nullptr;
static uint8_t *videoSlots = nullptr, *videoDecodeWork = nullptr;
static std::atomic<bool> videoWorkerStop{false}, videoWorkerDone{true};

static void videoDecodeWorker(void *) {
  uint32_t index = 0;
  while (!videoWorkerStop.load()) {
    uint8_t *slot;
    if (xQueueReceive(videoFreeQueue, &slot, pdMS_TO_TICKS(10)) != pdTRUE) continue;
    if (videoWorkerStop.load()) break;
    if (index == videoInfo.frame_count) {
      videoSource.pos = 0;
      bap_rewind(videoDecoder);
      index = 0;
    }
    uint32_t start = micros();
    int status = bap_next_frame(videoDecoder, videoDecodeWork);
    uint32_t decodeUs=micros()-start;
#ifdef VIDEO_AUDIO
    uint32_t expandStart=micros();
    if(!status)for(int y=0;y<videoInfo.store_h;++y)
      bap_expand_row(videoDecodeWork+y*videoInfo.stride,
          reinterpret_cast<uint16_t*>(slot)+y*videoInfo.scale*videoOutW,
          videoInfo.store_w,videoInfo.bpp,videoInfo.scale,videoOutW,videoLut);
    ReadyVideoFrame ready{slot,decodeUs,status,index,micros()-expandStart};
#else
    if (!status) memcpy(slot, videoDecodeWork, videoInfo.frame_bytes);
    ReadyVideoFrame ready{slot, micros() - start, status, index};
#endif
    // Owning a free slot guarantees space in the equally sized ready queue.
    xQueueSend(videoReadyQueue, &ready, portMAX_DELAY);
    ++index;
    if (status) break;
  }
  videoWorkerDone.store(true);
  vTaskDelete(nullptr);  // no shared buffer/queue accesses after publishing done
}

static void stopVideoPrefetch() {
  videoWorkerStop.store(true);
  // The worker's only idle wait is the bounded free-slot wait above. Join before
  // freeing the decoder, slots or queues, including on swipe and decode failure.
  while (!videoWorkerDone.load()) delay(1);
  if (videoFreeQueue) vQueueDelete(videoFreeQueue);
  if (videoReadyQueue) vQueueDelete(videoReadyQueue);
  videoFreeQueue = videoReadyQueue = nullptr;
  free(videoSlots); videoSlots = nullptr;
  free(videoDecodeWork); videoDecodeWork = nullptr;
}

static bool startVideoPrefetch() {
  videoFreeQueue = xQueueCreate(VIDEO_PREFETCH_SLOTS, sizeof(uint8_t *));
  videoReadyQueue = xQueueCreate(VIDEO_PREFETCH_SLOTS, sizeof(ReadyVideoFrame));
#ifdef VIDEO_AUDIO
  uint32_t slotBytes=videoOutW*videoInfo.store_h*videoInfo.scale*sizeof(uint16_t);
#else
  uint32_t slotBytes=videoInfo.frame_bytes;
#endif
  videoSlots = static_cast<uint8_t *>(heap_caps_malloc(
      VIDEO_PREFETCH_SLOTS * slotBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  videoDecodeWork = static_cast<uint8_t *>(heap_caps_malloc(
      videoInfo.frame_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!videoFreeQueue || !videoReadyQueue || !videoSlots || !videoDecodeWork) {
    stopVideoPrefetch(); return false;
  }
  for (unsigned i = 0; i < VIDEO_PREFETCH_SLOTS; ++i) {
    uint8_t *slot = videoSlots + i * slotBytes;
    xQueueSend(videoFreeQueue, &slot, 0);
  }
  videoWorkerStop.store(false); videoWorkerDone.store(false);
  if (xTaskCreatePinnedToCore(videoDecodeWorker, "video-decode", 4096, nullptr, 1,
                             nullptr, 0) != pdPASS) {
    videoWorkerDone.store(true); stopVideoPrefetch(); return false;
  }
  // Prime three frames before starting the presentation clock.
  while (uxQueueMessagesWaiting(videoReadyQueue) < 3 && !videoWorkerDone.load()) delay(1);
  return uxQueueMessagesWaiting(videoReadyQueue) > 0;
}
