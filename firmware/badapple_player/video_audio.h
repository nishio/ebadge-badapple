#pragma once
// Optional full-clip audio (-DVIDEO_AUDIO). Starts muted; tap UNMUTE on screen.
#include <driver/i2s_std.h>
#include <esp_timer.h>
#include <ima_mono.h>
#include <vendor/es8311.h>
#include <atomic>
#include "audio_output.h"

static i2s_chan_handle_t avTx=nullptr;
static es8311_handle_t avCodec=nullptr;
static uint8_t *avData=nullptr;
static int16_t *avPcm=nullptr;
static int16_t avStereo[512];
static uint32_t avHeader[16]={};
static std::atomic<bool> avStop{false},avDone{true},avRunning{false},avError{false};
static std::atomic<uint32_t> avSubmitted{0},avDma{0},avDmaAt{0},avStarves{0};
static uint32_t avCursor=0,avBlock=0,avPcmPos=2041,avStarted=0;
static std::atomic<uint32_t> avDecodeSum{0},avDecodeMax{0},avBlocks{0};
static portMUX_TYPE avClockMux=portMUX_INITIALIZER_UNLOCKED;
static bool avEnabled=false,avLoaded=false;
static int avVolume=50;
#ifdef VIDEO_AUDIO_SILENT
static AudioOutputState avOutput(true);
#else
static AudioOutputState avOutput(false);
#endif
static int32_t avSkewMin=INT32_MAX,avSkewMax=INT32_MIN,avSkewLast=0;
static uint32_t avFramesShown=0,avExpandMax=0;
static uint64_t avExpandSum=0;
// UI-core statistics over consumed frames, with no per-frame serial output.
// Sample queue AFTER panel transfer, matching the earlier video-only trace.
static uint32_t avQueueHist[VIDEO_PREFETCH_SLOTS+1]={};
static uint32_t avQueueSamples=0,avQueueMin=VIDEO_PREFETCH_SLOTS,avQueueMinFrame=0;
static uint32_t avQueueBelowFullRun=0,avQueueBelowFullLongest=0;
static uint64_t avProducerSum=0;
static uint32_t avProducerMax=0,avProducerOverBudget=0;
static void avRecordQueue(uint32_t index,uint32_t count) {
  if(count>VIDEO_PREFETCH_SLOTS)return;
  ++avQueueHist[count];++avQueueSamples;
  if(count<avQueueMin){avQueueMin=count;avQueueMinFrame=index;}
  if(count<VIDEO_PREFETCH_SLOTS){++avQueueBelowFullRun;avQueueBelowFullLongest=max(avQueueBelowFullLongest,avQueueBelowFullRun);}
  else avQueueBelowFullRun=0;
}
static Arduino_Canvas avButtons(246,42,nullptr),avLabel(246,42,nullptr);
constexpr int AV_MUTE_X=232,AV_MUTE_Y=46,AV_MUTE_W=124,AV_MUTE_H=42;
static bool avUiReady=false;

static bool avCheck(esp_err_t err,const char *stage) {
  if(err==ESP_OK)return true;
  avError.store(true);Serial.printf("av error stage=%s code=%s\n",stage,esp_err_to_name(err));return false;
}
static bool IRAM_ATTR avSent(i2s_chan_handle_t,i2s_event_data_t *event,void *) {
  portENTER_CRITICAL_ISR(&avClockMux);
  uint32_t total=avDma.fetch_add(event->size/4)+event->size/4;
  avDmaAt.store(uint32_t(esp_timer_get_time()));
  portEXIT_CRITICAL_ISR(&avClockMux);
  // A completed DMA block without enough submitted source data indicates starvation.
  // The final padded block and post-EOF silence are excluded.
  if(total<avHeader[3] && total>avSubmitted.load())avStarves.fetch_add(1);
  return false;
}
static uint32_t avClockSamples() {
  if(!avRunning.load())return 0;
  portENTER_CRITICAL(&avClockMux);
  uint32_t a=avDma.load(),t=avDmaAt.load();
  portEXIT_CRITICAL(&avClockMux);
  uint32_t fraction=min(uint32_t(256),uint32_t(uint64_t(micros()-t)*22050/1000000));
  return a+fraction; // keep elapsed time beyond EOF so late video is not hidden
}
static void avReport(const char *label) {
  Serial.printf("av %s running=%d done=%d error=%d volume=%d pa=%d output_muted=%d mute_locked=%d submitted=%lu dma=%lu starvation_blocks=%lu decode_avg_us=%lu decode_max_us=%lu frames=%lu skew_min_us=%ld skew_max_us=%ld skew_last_us=%ld internal=%u psram=%u\n",
    label,avRunning.load(),avDone.load(),avError.load(),avVolume,digitalRead(PA),avOutput.muted,avOutput.locked,
    (unsigned long)avSubmitted.load(),(unsigned long)avDma.load(),(unsigned long)avStarves.load(),
    (unsigned long)(avBlocks?avDecodeSum/avBlocks:0),(unsigned long)avDecodeMax,(unsigned long)avFramesShown,
    (long)(avFramesShown?avSkewMin:0),(long)(avFramesShown?avSkewMax:0),(long)avSkewLast,
    heap_caps_get_free_size(MALLOC_CAP_INTERNAL),heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.printf("av rgb producer_avg_us=%lu producer_max_us=%lu\n",(unsigned long)(avFramesShown?avExpandSum/avFramesShown:0),(unsigned long)avExpandMax);
  Serial.printf("av queue after_present_samples=%lu min=%lu min_frame=%lu below_full_longest=%lu hist_0_to_8=",
    (unsigned long)avQueueSamples,(unsigned long)avQueueMin,(unsigned long)avQueueMinFrame,(unsigned long)avQueueBelowFullLongest);
  for(unsigned i=0;i<=VIDEO_PREFETCH_SLOTS;++i)Serial.printf(i?",%lu":"%lu",(unsigned long)avQueueHist[i]);
  Serial.println();
  Serial.printf("av producer decode_rgb_avg_us=%lu decode_rgb_max_us=%lu over_frame_budget=%lu\n",
    (unsigned long)(avFramesShown?avProducerSum/avFramesShown:0),(unsigned long)avProducerMax,(unsigned long)avProducerOverBudget);
}
static void avControls() {
  if(!avUiReady)return;
  avButtons.fillScreen(RGB565_BLACK);avButtons.setTextColor(RGB565_WHITE);avButtons.setTextSize(2);
  for(int x : {0,68,196})avButtons.drawRoundRect(x,0,x==68?110:50,42,8,0x4DD7);
  avButtons.setCursor(20,13);avButtons.print("-");avButtons.setCursor(100,13);avButtons.print("STOP");
  avButtons.setCursor(214,13);avButtons.print("+");
  avLabel.fillScreen(RGB565_BLACK);avLabel.setTextColor(avOutput.muted?0xFFE0:RGB565_WHITE);
  avLabel.setTextSize(1);avLabel.setCursor(10,4);avLabel.print(avOutput.muted?"MUTED":"AUDIO ON");
  char text[16];snprintf(text,sizeof(text),"%d/100",avVolume);
  avLabel.setTextSize(2);avLabel.setCursor(10,21);avLabel.print(text);
  const char *action=avOutput.locked?"LOCKED":avOutput.muted?"UNMUTE":"MUTE";
  avLabel.setTextColor(RGB565_WHITE);
  avLabel.drawRoundRect(122,0,AV_MUTE_W,AV_MUTE_H,8,avOutput.locked?0x8410:0x4DD7);
  avLabel.setCursor(122+(AV_MUTE_W-strlen(action)*12)/2,13);avLabel.print(action);
  display.draw16bitRGBBitmap(110,388,avButtons.getFramebuffer(),246,42);
  display.draw16bitRGBBitmap(110,AV_MUTE_Y,avLabel.getFramebuffer(),246,42);
}
static bool avSetMuted(bool muted) {
  bool ok=avOutput.setMuted(muted,avRunning.load(),
    [](bool value){return avCodec?avCheck(es8311_voice_mute(avCodec,value),"user_mute"):value;},
    [](bool enabled){digitalWrite(PA,enabled?HIGH:LOW);});
  Serial.printf("av mute requested=%d output_muted=%d mute_locked=%d pa=%d ok=%d\n",
    muted,avOutput.muted,avOutput.locked,digitalRead(PA),ok);
  if(avLoaded)avControls();return ok;
}
static void avChangeVolume(int delta) {
  if(!avCodec)return;
  int actual=0;
  if(avCheck(es8311_voice_volume_set(avCodec,constrain(avVolume+delta,0,100),&actual),"volume"))avVolume=actual;
  Serial.printf("av volume=%d\n",avVolume);avControls();
}
static void avShutdown() {
  digitalWrite(PA,LOW);
  if(avCodec)es8311_voice_mute(avCodec,true);
  avOutput.muted=true; // A fresh playback always requires an explicit UNMUTE.
  avStop.store(true);while(!avDone.load())delay(1);
  avRunning.store(false);
  if(avTx){if(avEnabled)i2s_channel_disable(avTx);i2s_del_channel(avTx);avTx=nullptr;}
  avEnabled=false;
  if(avCodec){es8311_delete(avCodec);avCodec=nullptr;}
  free(avData);avData=nullptr;free(avPcm);avPcm=nullptr;avLoaded=false;
}
// Assemble exactly 256 stereo frames per DMA write, zero-padding only EOF.
static unsigned avNextChunk() {
  unsigned count=0;
  while(count<256 && avCursor<avHeader[3]) {
    if(avPcmPos==2041) {
      if((avBlock+1)*1024>avHeader[6]){avError.store(true);return 0;}
      uint32_t start=micros();size_t n=ima_mono_decode(avData+avBlock*1024,1024,avPcm,2041);
      uint32_t us=micros()-start;
      if(n!=2041){avError.store(true);return 0;}
      avDecodeSum+=us;avDecodeMax.store(max(avDecodeMax.load(),us));++avBlocks;++avBlock;avPcmPos=0;
    }
    avStereo[2*count]=avStereo[2*count+1]=avPcm[avPcmPos++];++count;++avCursor;
  }
  for(unsigned i=count;i<256;++i)avStereo[2*i]=avStereo[2*i+1]=0;
  return count;
}
static void avWorker(void *) {
  while(!avRunning.load() && !avStop.load())vTaskDelay(1);
  while(!avStop.load() && avCursor<avHeader[3]) {
    if(!avNextChunk())break;
    size_t written=0;
    if(!avCheck(i2s_channel_write(avTx,avStereo,sizeof(avStereo),&written,100),"write") || written!=sizeof(avStereo)){
      avError.store(true);break;
    }
    avSubmitted.store(avCursor);
  }
  avDone.store(true);vTaskDelete(nullptr);
}
static bool avPrepare() {
  avError.store(false);avStop.store(false);avDone.store(true);avRunning.store(false);
  avCursor=avBlock=avBlocks=avDecodeMax=0;avPcmPos=2041;avDecodeSum=0;
  avDma.store(0);avSubmitted.store(0);avStarves.store(0);
  avSkewMin=INT32_MAX;avSkewMax=INT32_MIN;avSkewLast=0;avFramesShown=0;avExpandSum=0;avExpandMax=0;
  memset(avQueueHist,0,sizeof(avQueueHist));avQueueSamples=0;avQueueMin=VIDEO_PREFETCH_SLOTS;avQueueMinFrame=0;
  avQueueBelowFullRun=avQueueBelowFullLongest=0;avProducerSum=0;avProducerMax=avProducerOverBudget=0;
  uint32_t offset=(BAP_HEADER_BYTES+videoInfo.payload_bytes+4095)&~4095u;
  if(offset+sizeof(avHeader)>videoPart->size || esp_partition_read(videoPart,offset,avHeader,sizeof(avHeader))!=ESP_OK)return false;
  if(avHeader[0]!=0x31504141 || avHeader[1]!=1 || avHeader[2]!=22050 || avHeader[4]!=1024 || avHeader[5]!=2041 ||
     avHeader[8]!=videoInfo.crc32 || avHeader[9]!=32+videoInfo.payload_bytes || avHeader[10]!=videoInfo.frame_count ||
     avHeader[11]!=videoInfo.fps_num || avHeader[12]!=videoInfo.fps_den || !avHeader[3] ||
     avHeader[6]!=((uint64_t(avHeader[3])+2040)/2041)*1024 ||
     avHeader[3]!=uint64_t(videoInfo.frame_count)*videoInfo.fps_den*22050/videoInfo.fps_num ||
     uint64_t(offset)+64+avHeader[6]>videoPart->size)return false;
  avData=(uint8_t*)heap_caps_malloc(avHeader[6],MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  avPcm=(int16_t*)heap_caps_malloc(2041*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!avData || !avPcm)return false;
  if(esp_partition_read(videoPart,offset+64,avData,avHeader[6])!=ESP_OK || bap_crc32(0,avData,avHeader[6])!=avHeader[7])return false;
  i2s_chan_config_t ch=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO,I2S_ROLE_MASTER);
  ch.dma_desc_num=8;ch.dma_frame_num=256;ch.auto_clear=true;
  if(!avCheck(i2s_new_channel(&ch,&avTx,nullptr),"channel"))return false;
  i2s_std_config_t cfg={};cfg.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(22050);
  cfg.slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO);
  cfg.gpio_cfg.mclk=GPIO_NUM_16;cfg.gpio_cfg.bclk=GPIO_NUM_9;cfg.gpio_cfg.ws=GPIO_NUM_45;cfg.gpio_cfg.dout=GPIO_NUM_8;cfg.gpio_cfg.din=I2S_GPIO_UNUSED;
  if(!avCheck(i2s_channel_init_std_mode(avTx,&cfg),"std"))return false;
  i2s_event_callbacks_t callbacks={};callbacks.on_sent=avSent;
  if(!avCheck(i2s_channel_register_event_callback(avTx,&callbacks,nullptr),"callback"))return false;
  if(!avCheck(i2s_channel_enable(avTx),"mclk"))return false;avEnabled=true;
  avCodec=es8311_create(0,ES8311_ADDRRES_0);if(!avCodec)return false;
  es8311_clock_config_t clock={};clock.mclk_from_mclk_pin=true;clock.mclk_frequency=22050*256;clock.sample_frequency=22050;
  if(!avCheck(es8311_init(avCodec,&clock,ES8311_RESOLUTION_16,ES8311_RESOLUTION_16),"codec") ||
     !avCheck(es8311_microphone_config(avCodec,false),"mic_off") ||
     !avCheck(es8311_voice_volume_set(avCodec,avVolume,nullptr),"volume") ||
     !avCheck(es8311_voice_mute(avCodec,true),"mute"))return false;
  if(!avCheck(i2s_channel_disable(avTx),"disable"))return false;avEnabled=false;
  for(int i=0;i<8;++i){
    if(!avNextChunk())return false;size_t loaded=0;
    if(!avCheck(i2s_channel_preload_data(avTx,avStereo,sizeof(avStereo),&loaded),"preload") || loaded!=sizeof(avStereo))return false;
  }
  avSubmitted.store(avCursor);avDma.store(0);
  if(!avUiReady)avUiReady=avButtons.begin() && avLabel.begin();
  if(!avUiReady)return false;
  avDone.store(false);
  if(xTaskCreatePinnedToCore(avWorker,"av-audio",4096,nullptr,2,nullptr,0)!=pdPASS){avDone.store(true);return false;}
  // Silent load measurement keeps BOTH codec mute and PA off; decoding/I2S continue.
  if(!avCheck(es8311_voice_mute(avCodec,avOutput.muted),"arm_codec"))return false;
  avLoaded=true;
  Serial.printf("av ready asset_offset=%lu payload=%lu samples=%lu volume=%d crc=ok preloaded=%lu\n",(unsigned long)offset,(unsigned long)avHeader[6],(unsigned long)avHeader[3],avVolume,(unsigned long)avSubmitted.load());
  return true;
}
static bool avStart() {
  if(!avLoaded)return false;
  avDma.store(0);avStarves.store(0);avDmaAt.store(micros());avStarted=micros();
  if(!avCheck(i2s_channel_enable(avTx),"start"))return false;
  avEnabled=true;digitalWrite(PA,avOutput.muted?LOW:HIGH);avRunning.store(true);return true;
}
static void avRecordFrame(uint32_t index) {
  int32_t us=int32_t(uint64_t(avClockSamples())*1000000/22050)-int32_t(uint64_t(index)*1000000*videoInfo.fps_den/videoInfo.fps_num);
  avSkewLast=us;avSkewMin=min(avSkewMin,us);avSkewMax=max(avSkewMax,us);++avFramesShown;
}
