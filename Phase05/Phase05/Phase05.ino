#include "BurstConfig.h"
#include "BurstStorage.h"
#include "RgbStream.h"
#include <esp_timer.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <new>

namespace {
RTC_DATA_ATTR uint32_t boots=0;
RTC_DATA_ATTR bool holdSleep=false;
BurstStats stats;
bool ready=false, capturing=false, finished=false, fatal=false, cleaned=false, depthInitialized=false;
std::atomic<bool> depthRunning{false}, depthAlive{false};
std::atomic<uint32_t> depthCount{0}, depthErrors{0}, depthGaps{0}, depthOverflows{0};
DepthFrame* frames=nullptr;
uint8_t* jpeg=nullptr;
uint64_t deadline=0, sleepAt=0, nextRgbDue=0, lastDma=0;
uint32_t sleepSeconds=BurstConfig::kSleepSeconds;
char command[96]; size_t commandUsed=0; bool commandOverflow=false;

void depthTask(void*) {
  DepthFrame frame; bool previous=false; uint32_t last=0;
  while(depthRunning && uint64_t(esp_timer_get_time())<deadline) {
    const int result=pollDepth(frame);
    if(result<0) { ++depthErrors; break; }
    if(result==1) {
      if(previous) {
        const uint32_t delta=frame.number-last;
        if(!delta || delta>0x7fffffff) { ++depthErrors; break; }
        depthGaps+=delta-1;
      }
      previous=true; last=frame.number;
      if(frame.opticalEarliestUs>=stats.startUs && frame.opticalLatestUs<=deadline) {
        const uint32_t index=depthCount;
        if(index>=BurstConfig::kDepthCapacity) { ++depthOverflows; break; }
        // Only this task writes the bounded archive; SD reads it after join.
        new (&frames[index]) DepthFrame(frame);
        depthCount.store(index+1,std::memory_order_release);
      }
    }
    delay(1);
  }
  digitalWrite(3,LOW);
  depthRunning=false; depthAlive=false; vTaskDelete(nullptr);
}
void status() {
  Serial.printf("STATUS ready=%u capturing=%u finished=%u fatal=%u hold=%u burst=%lu rgb_frames=%lu depth_frames=%lu depth_gaps=%lu depth_overflows=%lu rgb_missed=%lu rgb_errors=%lu depth_errors=%lu led=%u boot_count=%lu wake_reason=%u uptime_us=%llu\n",
    ready,capturing && depthRunning,finished,fatal,holdSleep,(unsigned long)burstNumber(),
    (unsigned long)stats.rgbCount,(unsigned long)depthCount,(unsigned long)depthGaps,(unsigned long)depthOverflows,
    (unsigned long)stats.rgbMissed,(unsigned long)rgbErrors,(unsigned long)depthErrors,digitalRead(3),
    (unsigned long)boots,unsigned(esp_sleep_get_wakeup_cause()),uint64_t(esp_timer_get_time()));
  storageStatus();
}
bool stopAcquisition() {
  capturing=false; digitalWrite(3,LOW); depthRunning=false;
  const uint32_t start=millis();
  while(depthAlive && millis()-start<2000) delay(5);
  if(depthAlive) return false;
  stats.depthCount=depthCount; stats.depthErrors=depthErrors; stats.depthGaps=depthGaps;
  stats.depthOverflows=depthOverflows;
  stats.sensorStandby=!depthInitialized || depthStandby();
  stats.cameraOff=stopRgbStream(); stats.rgbErrors=rgbErrors;
  return stats.sensorStandby && stats.cameraOff;
}
void completeBurst(bool normal) {
  stats.stopUs=esp_timer_get_time();
  if(!stopAcquisition()) fatal=true;
  stats.success=normal && !fatal && stats.depthCount>=285 && stats.rgbCount>=28 &&
    !stats.depthGaps && !stats.depthOverflows && !stats.rgbErrors && !stats.depthErrors && !stats.rgbMissed;
  if(!finishBurstStorage(stats,frames)) { fatal=true; Serial.println("ERROR storage_finalize"); }
  finished=true; sleepAt=esp_timer_get_time()+2000000;
  Serial.printf("BURST_DONE burst=%lu success=%u depth_frames=%lu rgb_frames=%lu depth_gaps=%lu duration_us=%llu led=0 standby=%u camera_off=%u\n",
    (unsigned long)burstNumber(),stats.success && !fatal,(unsigned long)stats.depthCount,(unsigned long)stats.rgbCount,
    (unsigned long)stats.depthGaps,stats.stopUs-stats.startUs,stats.sensorStandby,stats.cameraOff);
  status();
}
void enterSleep() {
  if(capturing || fatal || !finished || !stats.sensorStandby || !stats.cameraOff) return;
  if(!closeBurstStorage()) { fatal=true; Serial.println("ERROR storage_close_before_sleep"); return; }
  digitalWrite(3,LOW); esp_sleep_enable_timer_wakeup(uint64_t(sleepSeconds)*1000000);
  Serial.printf("SLEEP seconds=%lu led=0 next_boot=%lu\n",(unsigned long)sleepSeconds,(unsigned long)(boots+1));
  Serial.flush(); delay(100);
  gpio_hold_en(GPIO_NUM_3); gpio_deep_sleep_hold_en(); esp_deep_sleep_start();
}
void execute() {
  command[commandUsed]=0;
  if(!strcmp(command,"STATUS")) status();
  else if(!strcmp(command,"HOLD")) { holdSleep=true; Serial.println("HOLD enabled=1"); status(); }
  else if(!strcmp(command,"ALLOW_SLEEP")) { holdSleep=false; sleepAt=esp_timer_get_time()+1000000; Serial.println("HOLD enabled=0"); status(); }
  else if(!strcmp(command,"STOP")) { holdSleep=true; if(capturing) completeBurst(false); else status(); }
  else if(!strcmp(command,"REBOOT")) {
    if(capturing) completeBurst(false);
    Serial.println("REBOOTING"); Serial.flush(); delay(50); ESP.restart();
  }
  else if(!strncmp(command,"SLEEP ",6)) {
    char* end=nullptr; const unsigned long value=strtoul(command+6,&end,10);
    if(!capturing && finished && !fatal && end!=command+6 && !*end && value>=1 && value<=86400) {
      sleepSeconds=value; holdSleep=false; enterSleep();
    } else Serial.println("ERROR SLEEP requires_finished_burst_and_seconds_1_86400");
  } else if(!strncmp(command,"GET ",4)) {
    if(capturing || !finished) { Serial.println("ERROR GET wait_for_burst_end"); return; }
    unsigned long burst=0, sequence=0; char kind[16],tail=0;
    const int count=sscanf(command+4,"%lu %15s %lu %c",&burst,kind,&sequence,&tail);
    if(count<2 || count>3) Serial.println("ERROR GET syntax=GET_burst_KIND_sequence");
    else downloadBurstFile(burst,kind,sequence);
  } else if(strncmp(command,"ACK ",4)) Serial.println("ERROR commands=STATUS,HOLD,ALLOW_SLEEP,GET,STOP,REBOOT,SLEEP_seconds");
}
void console() {
  while(Serial.available()) {
    const char c=Serial.read(); if(c=='\r') continue;
    if(c=='\n') {
      if(!commandOverflow) execute(); else Serial.println("ERROR command_too_long");
      commandUsed=0; commandOverflow=false;
    } else if(commandUsed<sizeof(command)-1) command[commandUsed++]=c;
    else commandOverflow=true;
  }
}
bool checkDepthCadence() {
  DepthFrame frame; unsigned count=0; uint64_t first=0,last=0;
  const uint64_t end=esp_timer_get_time()+6000000;
  while(count<16 && uint64_t(esp_timer_get_time())<end) {
    console(); const int result=pollDepth(frame);
    if(result<0) return false;
    if(result==1) {
      if(count>=4) { if(!first) first=frame.hostUs; last=frame.hostUs; }
      ++count;
    }
    delay(1);
  }
  const uint64_t mean=count>5 ? (last-first)/(count-5) : 0;
  Serial.printf("DEPTH_CADENCE samples=%u iterations=%u period_ms=%lu mean_interval_us=%llu\n",
    count,BurstConfig::kDepthIterations,(unsigned long)BurstConfig::kDepthPeriodMs,mean);
  return count==16 && mean>=95000 && mean<=105000;
}
}

void setup() {
  gpio_deep_sleep_hold_dis(); gpio_hold_dis(GPIO_NUM_3);
  pinMode(3,OUTPUT); digitalWrite(3,LOW); pinMode(47,OUTPUT); digitalWrite(47,LOW); ++boots;
  Serial.setTxBufferSize(4096); Serial.setTxTimeoutMs(100); Serial.begin(115200);
  delay(1500); console();
  Serial.printf("PHASE05 boot=%lu wake_reason=%u burst_seconds=30 sleep_seconds=10 depth_fps=10 rgb_fps=1 iterations=%u jpeg_quality=6\n",
    (unsigned long)boots,unsigned(esp_sleep_get_wakeup_cause()),BurstConfig::kDepthIterations);
  if(!beginRgbStream()) { fatal=true; Serial.println("ERROR camera_initialization"); return; }
  if(!beginDepth()) { fatal=true; Serial.println("ERROR depth_initialization"); return; }
  depthInitialized=true;
  frames=static_cast<DepthFrame*>(heap_caps_malloc(BurstConfig::kDepthCapacity*sizeof(DepthFrame),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  jpeg=static_cast<uint8_t*>(heap_caps_malloc(BurstConfig::kMaxJpegBytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if(!frames || !jpeg) { fatal=true; Serial.println("ERROR capture_buffers"); return; }
  if(!beginBurstStorage(esp_sleep_get_wakeup_cause(),boots)) { fatal=true; Serial.println("ERROR storage_initialization"); return; }
  if(!startDepth() || !checkDepthCadence()) { fatal=true; Serial.println("ERROR depth_cadence_10fps_unmet"); return; }
  stats.startUs=esp_timer_get_time(); deadline=stats.startUs+BurstConfig::kBurstUs; nextRgbDue=stats.startUs;
  depthRunning=true; depthAlive=true; capturing=true; digitalWrite(3,HIGH);
  if(xTaskCreatePinnedToCore(depthTask,"depth_archive",4096,nullptr,2,nullptr,0)!=pdPASS) {
    depthAlive=false; depthRunning=false; fatal=true; Serial.println("ERROR depth_task"); return;
  }
  ready=true;
  Serial.printf("BURST_START burst=%lu start_us=%llu deadline_us=%llu depth_capacity=%u led=1\n",
    (unsigned long)burstNumber(),stats.startUs,deadline,BurstConfig::kDepthCapacity);
  status();
}
void loop() {
  console();
  if(fatal) {
    digitalWrite(3,LOW);
    if(!finished && !cleaned) { stopAcquisition(); cleaned=true; ready=false; }
    delay(20); return;
  }
  if(capturing && uint64_t(esp_timer_get_time())>=deadline) { completeBurst(true); return; }
  if(capturing && (depthErrors || depthOverflows || rgbErrors)) { fatal=true; completeBurst(false); return; }
  if(capturing) {
    const uint64_t now=esp_timer_get_time();
    if(now>nextRgbDue+BurstConfig::kRgbPeriodUs+200000) {
      ++stats.rgbMissed; nextRgbDue+=BurstConfig::kRgbPeriodUs;
    }
    RgbFrame rgb;
    if(now>=nextRgbDue && copyScheduledRgb(nextRgbDue,lastDma,stats.startUs,deadline,jpeg,rgb)) {
      uint64_t writeUs=0;
      if(!saveRgb(stats.rgbCount+1,jpeg,rgb,nextRgbDue,writeUs)) { fatal=true; completeBurst(false); return; }
      ++stats.rgbCount; lastDma=rgb.dmaUs; nextRgbDue+=BurstConfig::kRgbPeriodUs;
      if(writeUs>stats.maxWriteUs) stats.maxWriteUs=writeUs;
      Serial.printf("RGB seq=%lu dma_us=%llu depth_count=%lu jpeg_bytes=%u write_us=%llu\n",
        (unsigned long)stats.rgbCount,rgb.dmaUs,(unsigned long)depthCount,unsigned(rgb.length),writeUs);
    }
  }
  if(finished && !holdSleep && uint64_t(esp_timer_get_time())>=sleepAt) enterSleep();
  delay(1);
}
