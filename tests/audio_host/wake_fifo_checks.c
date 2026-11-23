/* 运行生产 AFE->FIFO 门禁。环形缓冲用固定帧计数桩替代，不模拟声学和并发。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../main/wake_word.h"
#include "freertos/semphr.h"
#include "esp_log.h"
static unsigned checks, queued, sent, returned;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
#define AUDIO_UPLOAD_FIFO_MS 512u
#define AUDIO_DIAGNOSTIC_ENERGY 2500
#define BSP_I2S_SAMPLE_RATE 16000
#define VAD_SPEECH 1
typedef enum {
    APP_STATE_WAIT_WAKE, APP_STATE_LISTENING, APP_STATE_PLAYING,
    APP_STATE_WS_CONNECTING_WAKE, APP_STATE_WS_CONNECTED
} app_state_t;
typedef struct { int16_t *data; int data_size, vad_state; } afe_fetch_result_t;
static app_state_t current;
static app_state_t app_state_get(void) { return current; }
static RingbufHandle_t s_ringbuf=(void*)1;
static SemaphoreHandle_t s_audio_buffer_lock=(void*)2;
static bool s_audio_buffer_enabled=true;
static bool s_pre_roll_enabled=false;
static uint32_t s_preroll_end_sample=0;
static bool s_preroll_drained=false;
static uint32_t s_wake_boundary_sample=0;
static int s_fetch_chunksize=512;
static wake_word_audio_progress_t s_audio_progress;
static uint32_t s_overflow_log_ms, s_overflow_frames;
static int16_t frame[512];
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
static void vRingbufferGetInfo(void *r,void *a,void *b,void *c,void *d,UBaseType_t *n)
{ (void)r; (void)a; (void)b; (void)c; (void)d; *n=queued; }
static void *xRingbufferReceive(void *r,size_t *bytes,int t)
{ (void)r; (void)t; if(!queued) return NULL; --queued; *bytes=sizeof(frame); return frame; }
static void vRingbufferReturnItem(void *r,void *p) { (void)r; (void)p; ++returned; }
static int xRingbufferSend(void *r,void *p,size_t bytes,int t)
{ (void)r; (void)p; (void)t; CHECK(bytes==sizeof(frame)); ++queued; ++sent; return pdTRUE; }
static int64_t test_time_us=32000;
int64_t esp_timer_get_time(void) { return test_time_us; }
unsigned esp_log_timestamp(void) { return (unsigned)(test_time_us/1000); }
#include "wake_fifo.inc"
int main(void)
{
    for(unsigned i=0;i<512;++i) frame[i]=1000;
    afe_fetch_result_t res={frame,sizeof(frame),VAD_SPEECH};
    current=APP_STATE_WAIT_WAKE; buffer_afe_audio(&res); CHECK(!queued);
    current=APP_STATE_LISTENING; buffer_afe_audio(&res); CHECK(queued==1 && sent==1);
    current=APP_STATE_PLAYING; buffer_afe_audio(&res); CHECK(queued==2 && sent==2);
    CHECK(s_audio_progress.written_end==1024 && s_audio_progress.speech_end==1024);
    CHECK(s_audio_progress.last_voice_ms==32);
    test_time_us=64000; buffer_afe_audio(&res); CHECK(s_audio_progress.last_voice_ms==64);
    test_time_us=96000; res.vad_state=0; buffer_afe_audio(&res);
    CHECK(s_audio_progress.last_voice_ms==64); /* 有能量但 AFE 非人声，不续时。 */
    res.vad_state=VAD_SPEECH; memset(frame,0,sizeof(frame)); buffer_afe_audio(&res);
    CHECK(s_audio_progress.last_voice_ms==64); /* 静音包不续时。 */
    for(unsigned i=0;i<512;++i) frame[i]=1000;
    wake_word_set_audio_buffer_enabled(false);
    CHECK(!queued && returned==5 && !s_audio_progress.written_end && !s_audio_progress.last_voice_ms);
    for(unsigned i=0;i<10000;++i) buffer_afe_audio(&res);
    CHECK(!queued && sent==5 && !s_overflow_frames && !s_audio_progress.written_end);
    wake_word_set_audio_buffer_enabled(true); current=APP_STATE_WAIT_WAKE;
    buffer_afe_audio(&res); CHECK(!queued);
    /* pre-roll: 连接期间也入队，模式切换不清空，按采样位置裁唤醒词。 */
    wake_word_set_pre_roll(true); current=APP_STATE_WS_CONNECTING_WAKE;
    buffer_afe_audio(&res); CHECK(queued==1 && sent==6);
    current=APP_STATE_WS_CONNECTED;
    buffer_afe_audio(&res); CHECK(queued==2 && sent==7);
    wake_word_mark_wake_boundary();
    buffer_afe_audio(&res); CHECK(queued==3 && sent==8); /* 唤醒后的第一句 */
    wake_word_discard_audio_before(12u*512u); /* 裁掉唤醒词边界前的两帧 */
    CHECK(queued==0);
    wake_word_set_pre_roll(false);
    CHECK(!queued && !s_audio_progress.written_end);
    current=APP_STATE_LISTENING; buffer_afe_audio(&res); CHECK(queued==1);
    current=APP_STATE_PLAYING;
    for(unsigned i=0;i<30;++i) buffer_afe_audio(&res);
    CHECK(queued==16); /* 普通全双工仍保留原有 512 ms 上限。 */
    /* 方案 A：pre-roll 补发完毕后切到 2 帧实时缓冲，避免打断失效。 */
    wake_word_set_preroll_replay_end(s_audio_progress.written_end);
    wake_word_finish_pre_roll_replay();
    CHECK(wake_word_get_preroll_replay_end()==s_audio_progress.written_end);
    for(unsigned i=0;i<30;++i) buffer_afe_audio(&res);
    CHECK(queued==2); /* 补发后只留 2 帧实时缓冲。 */
    CHECK(s_audio_progress.written_end > 0 && s_audio_progress.read_end > 0);
    test_time_us=(int64_t)UINT32_MAX*1000+1000;
    buffer_afe_audio(&res); CHECK(s_audio_progress.last_voice_ms==UINT32_MAX);
    test_time_us+=32000; buffer_afe_audio(&res); CHECK(s_audio_progress.last_voice_ms==32);
    printf("PASS: %u production microphone FIFO checks\n",checks); return 0;
}
