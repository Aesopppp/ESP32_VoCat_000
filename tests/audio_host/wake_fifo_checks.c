/* 运行生产 AFE->FIFO 门禁和补传；环形缓冲桩保存真实帧内容/顺序，不模拟声学与多核。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../main/wake_word.h"
#include "freertos/semphr.h"
#include "esp_log.h"
static unsigned checks, queued, sent, returned, head, tail;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
#define AUDIO_UPLOAD_FIFO_MS 512u
#ifndef WAKE_WORD_REPLAY_CAP_MS
#define WAKE_WORD_REPLAY_CAP_MS 6000u
#define WAKE_WORD_PRE_ROLL_MAX_MS 8000u
#endif
#define AUDIO_DIAGNOSTIC_ENERGY 2500
#define BSP_I2S_SAMPLE_RATE 16000
#define VAD_SPEECH 1
#define VAD_SILENCE 0
typedef enum {
    APP_STATE_WAIT_WAKE, APP_STATE_LISTENING, APP_STATE_PLAYING,
    APP_STATE_WS_CONNECTING_WAKE, APP_STATE_WS_CONNECTED
} app_state_t;
typedef struct { int16_t *data; int data_size, vad_state; } afe_fetch_result_t;
static app_state_t current;
static wake_word_mode_t s_mode=WAKE_WORD_MODE_WAKE;
static app_state_t app_state_get(void) { return current; }
static RingbufHandle_t s_ringbuf=(void*)1;
static RingbufHandle_t s_live_ringbuf=(void*)3;
static SemaphoreHandle_t s_audio_buffer_lock=(void*)2;
static bool s_audio_buffer_enabled=true;
static wake_word_state_t s_wake_state=WAKE_STATE_IDLE;
static bool s_pre_roll_enabled, s_pre_roll_uploading, s_pre_roll_replay_sealed;
static bool s_first_phrase_speech_seen, s_first_phrase_closed, s_live_buffer_active;
static uint32_t s_first_phrase_silence_samples, s_first_phrase_end, s_live_read_end;
static uint32_t s_pre_roll_replay_end;
static uint32_t s_wake_boundary_sample, s_afe_sample_end, s_pre_roll_dropped_frames;
static uint32_t s_pre_roll_dropped_older_frames;
static int s_fetch_chunksize=512;
static wake_word_audio_progress_t s_audio_progress;
static uint32_t s_overflow_log_ms, s_overflow_frames;
static int16_t frame[512], ring[WAKE_WORD_RB_ITEM_COUNT][512];
static unsigned ring_capacity=WAKE_WORD_RB_ITEM_COUNT;
static unsigned live_queued, live_sent, live_returned, live_head, live_tail;
static unsigned live_capacity=WAKE_WORD_LIVE_RB_ITEM_COUNT;
static int16_t live_ring[WAKE_WORD_LIVE_RB_ITEM_COUNT][512];
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
static void vRingbufferGetInfo(void *r,void *a,void *b,void *c,void *d,UBaseType_t *n)
{ (void)a; (void)b; (void)c; (void)d; *n=r==s_live_ringbuf ? live_queued : queued; }
static void *xRingbufferReceive(void *r,size_t *bytes,int t)
{
    (void)t;
    if(r==s_live_ringbuf) {
        if(!live_queued) return NULL;
        int16_t *item=live_ring[live_head]; live_head=(live_head+1)%WAKE_WORD_LIVE_RB_ITEM_COUNT;
        --live_queued; *bytes=sizeof(frame); return item;
    }
    if(!queued) return NULL;
    int16_t *item=ring[head]; head=(head+1)%WAKE_WORD_RB_ITEM_COUNT;
    --queued; *bytes=sizeof(frame); return item;
}
static void vRingbufferReturnItem(void *r,void *p)
{ (void)p; if(r==s_live_ringbuf) ++live_returned; else ++returned; }
static int xRingbufferSend(void *r,void *p,size_t bytes,int t)
{
    (void)t; CHECK(bytes==sizeof(frame));
    if(r==s_live_ringbuf) {
        if(live_queued>=live_capacity) return pdFALSE;
        memcpy(live_ring[live_tail],p,bytes); live_tail=(live_tail+1)%WAKE_WORD_LIVE_RB_ITEM_COUNT;
        ++live_queued; ++live_sent; return pdTRUE;
    }
    if(queued>=ring_capacity) return pdFALSE;
    memcpy(ring[tail],p,bytes); tail=(tail+1)%WAKE_WORD_RB_ITEM_COUNT;
    ++queued; ++sent; return pdTRUE;
}
static int64_t test_time_us;
int64_t esp_timer_get_time(void) { return test_time_us; }
unsigned esp_log_timestamp(void) { return (unsigned)(test_time_us/1000); }
static char last_latency_event[64];
static uint32_t last_latency_ms;
void latency_log_event(const char *event, uint32_t monotonic_ms)
{
    snprintf(last_latency_event,sizeof(last_latency_event),"%s",event ? event : "");
    last_latency_ms=monotonic_ms;
}
#include "wake_fifo.inc"

static void reset(void)
{
    wake_word_set_pre_roll(false);
    queued=sent=returned=head=tail=0; ring_capacity=WAKE_WORD_RB_ITEM_COUNT;
    s_pre_roll_dropped_frames=s_pre_roll_dropped_older_frames=0;
    live_queued=live_sent=live_returned=live_head=live_tail=0;
    live_capacity=WAKE_WORD_LIVE_RB_ITEM_COUNT;
    s_afe_sample_end=s_overflow_log_ms=0; s_audio_buffer_enabled=true; s_wake_state=WAKE_STATE_IDLE;
    test_time_us=0; current=APP_STATE_WAIT_WAKE; s_mode=WAKE_WORD_MODE_WAKE;
}
static void push_vad_frame(int16_t sequence, bool wake_detected, int vad_state)
{
    for(unsigned i=0;i<512;++i) frame[i]=sequence;
    afe_fetch_result_t res={frame,sizeof(frame),vad_state};
    test_time_us+=32000;
    buffer_afe_audio(&res,wake_detected,s_mode==WAKE_WORD_MODE_LITE);
}
static void push_frame(int16_t sequence, bool wake_detected)
{ push_vad_frame(sequence,wake_detected,VAD_SPEECH); }
static uint32_t read_frame_raw(uint32_t *sample_end)
{
    int16_t output[512];
    size_t bytes=wake_word_read_audio(output,sizeof(output),sample_end);
    return (uint32_t)bytes;
}
static uint32_t read_frame(int16_t expected)
{
    int16_t output[512]; uint32_t sample_end=0;
    CHECK(wake_word_read_audio(output,sizeof(output),&sample_end)==sizeof(output));
    CHECK(output[0]==expected && output[511]==expected);
    wake_word_confirm_audio_sent(sample_end);
    return sample_end;
}
static void normal_fifo_checks(void)
{
    reset(); push_frame(1000,false); CHECK(!queued);
    current=APP_STATE_LISTENING; push_frame(1000,false); CHECK(queued==1 && sent==1);
    current=APP_STATE_PLAYING; push_frame(1000,false); CHECK(queued==2 && sent==2);
    CHECK(s_audio_progress.written_end==1024 && s_audio_progress.speech_end==1024);
    CHECK(s_audio_progress.last_voice_ms==96);
    afe_fetch_result_t res={frame,sizeof(frame),0};
    test_time_us+=32000; buffer_afe_audio(&res,false,s_mode==WAKE_WORD_MODE_LITE);
    CHECK(s_audio_progress.last_voice_ms==96); /* 有能量但 AFE 非人声，不续时。 */
    push_frame(0,false); CHECK(s_audio_progress.last_voice_ms==96); /* 静音不续时。 */
    wake_word_set_audio_buffer_enabled(false);
    CHECK(!queued && returned==4 && !s_audio_progress.written_end && !s_audio_progress.last_voice_ms);
    for(unsigned i=0;i<40;++i) push_frame(1000,false);
    CHECK(!queued && sent==4 && !s_overflow_frames && !s_audio_progress.written_end);
    wake_word_set_audio_buffer_enabled(true); current=APP_STATE_WAIT_WAKE;
    push_frame(1000,false); CHECK(!queued && !wake_word_prepare_pre_roll_upload());
    current=APP_STATE_PLAYING;
    for(unsigned i=1;i<=30;++i) push_frame((int16_t)i,false);
    CHECK(queued==16); /* 普通全双工仍保留原有 512 ms 上限。 */
    for(unsigned i=1;i<=16;++i) {
        int16_t output[512]; uint32_t sample_end=0;
        CHECK(wake_word_read_audio(output,sizeof(output),&sample_end)==sizeof(output));
        wake_word_confirm_audio_sent(sample_end);
    }
    CHECK(!queued && s_audio_progress.written_end==s_audio_progress.read_end);
    test_time_us=(int64_t)UINT32_MAX*1000+1000-32000;
    push_frame(1000,false); CHECK(s_audio_progress.last_voice_ms==UINT32_MAX);
    push_frame(1000,false); CHECK(s_audio_progress.last_voice_ms==32);
}

#if WAKE_WORD_CONNECT_PRE_ROLL_ENABLED
static void transmit_boundary_checks(void)
{
    reset(); push_frame(900,true); current=APP_STATE_WS_CONNECTING_WAKE;
    for(unsigned i=1;i<=32;++i) push_frame((int16_t)i,false);
    CHECK(wake_word_prepare_pre_roll_upload());
    for(unsigned i=1;i<=32;++i) {
        int16_t output[512]; uint32_t sample_end=0;
        CHECK(wake_word_read_audio(output,sizeof(output),&sample_end)==sizeof(output));
        CHECK(output[0]==i && output[511]==i);
    }
    CHECK(!queued && s_pre_roll_enabled && s_pre_roll_uploading);
    uint32_t end=s_audio_progress.written_end;
    wake_word_confirm_audio_sent(end-8193);
    CHECK(s_pre_roll_enabled);
    wake_word_confirm_audio_sent(end+1);
    CHECK(s_pre_roll_enabled);
    current=APP_STATE_PLAYING;
    for(unsigned i=33;i<=52;++i) push_frame((int16_t)i,false);
    CHECK(!queued && live_queued==WAKE_WORD_LIVE_RB_ITEM_COUNT && s_pre_roll_replay_sealed);
    for(unsigned i=33;i<=52;++i) { int16_t output[512]; uint32_t sample_end=0; wake_word_read_audio(output,sizeof(output),&sample_end); wake_word_confirm_audio_sent(sample_end); }
    CHECK(s_pre_roll_replay_sealed);
    reset(); s_afe_sample_end=UINT32_MAX-8191; push_frame(900,true);
    current=APP_STATE_WS_CONNECTING_WAKE;
    for(unsigned i=1;i<=20;++i) push_frame((int16_t)i,false);
    CHECK(wake_word_prepare_pre_roll_upload());
    for(unsigned i=1;i<=4;++i) { int16_t output[512]; uint32_t sample_end=0; CHECK(wake_word_read_audio(output,sizeof(output),&sample_end)==sizeof(output)); }
    end=s_audio_progress.written_end;
    CHECK(s_pre_roll_enabled && s_pre_roll_replay_sealed);
}
static void handshake_checks(unsigned frames)
{
    reset(); push_frame(900,false); push_frame(901,true);
    CHECK(!queued && s_pre_roll_enabled && !s_pre_roll_uploading);
    CHECK(s_wake_boundary_sample==1024);
    for(unsigned i=1;i<=frames;++i) {
        current=i==1 ? APP_STATE_WAIT_WAKE : APP_STATE_WS_CONNECTING_WAKE;
        push_frame((int16_t)i,false);
    }
    CHECK(queued==frames && returned==0 && s_pre_roll_dropped_frames==0);
    current=APP_STATE_WS_CONNECTED;
    CHECK(wake_word_prepare_pre_roll_upload());
    CHECK(wake_word_prepare_pre_roll_upload());
    CHECK(s_pre_roll_uploading && queued==frames);
    current=APP_STATE_LISTENING;
    push_frame((int16_t)(frames+1),false);
    CHECK(live_queued==1 && !s_live_buffer_active);
    for(unsigned i=1;i<=frames;++i) { int16_t output[512]; uint32_t sample_end=0; CHECK(wake_word_read_audio(output,sizeof(output),&sample_end)==sizeof(output)); wake_word_confirm_audio_sent(sample_end); }
    CHECK(!queued && live_queued==1 && !s_live_buffer_active);
    { int16_t output[512]; uint32_t sample_end=0; wake_word_read_audio(output,sizeof(output),&sample_end); wake_word_confirm_audio_sent(sample_end); }
    current=APP_STATE_PLAYING; push_frame((int16_t)(frames+2),false);
    { int16_t output[512]; uint32_t sample_end=0; wake_word_read_audio(output,sizeof(output),&sample_end); wake_word_confirm_audio_sent(sample_end); }
    CHECK(!queued && !s_pre_roll_enabled && !s_pre_roll_uploading);
    for(unsigned i=1;i<=30;++i) push_frame((int16_t)i,false);
    CHECK(!queued && live_queued==16);
    for(unsigned i=1;i<=16;++i) { int16_t output[512]; uint32_t sample_end=0; wake_word_read_audio(output,sizeof(output),&sample_end); wake_word_confirm_audio_sent(sample_end); }
}
static void boundary_and_cancel_checks(void)
{
    reset(); current=APP_STATE_PLAYING;
    for(unsigned i=1;i<=8;++i) push_frame((int16_t)i,false);
    current=APP_STATE_WAIT_WAKE; push_frame(900,true);
    CHECK(!queued && returned==8);
    push_frame(901,false); CHECK(wake_word_prepare_pre_roll_upload());
    { int16_t output[512]; uint32_t sample_end=0; CHECK(wake_word_read_audio(output,sizeof(output),&sample_end)==sizeof(output)); CHECK(sample_end==10*512); wake_word_confirm_audio_sent(sample_end); }
    CHECK(s_pre_roll_enabled && s_pre_roll_replay_sealed && !s_live_buffer_active);
    wake_word_set_pre_roll(false);
    CHECK(!queued && !s_audio_progress.written_end && !s_wake_boundary_sample);
    current=APP_STATE_WS_CONNECTING_WAKE; push_frame(902,false);
    CHECK(!queued && !wake_word_prepare_pre_roll_upload());
    current=APP_STATE_WAIT_WAKE; push_frame(910,true); push_frame(911,false);
    wake_word_set_pre_roll(false);
    CHECK(!queued && !s_pre_roll_enabled && !s_pre_roll_uploading);
    push_frame(920,true); push_frame(921,false);
    CHECK(wake_word_prepare_pre_roll_upload());
    for(unsigned i=0;i<2;++i) { int16_t output[512]; uint32_t sample_end=0; wake_word_read_audio(output,sizeof(output),&sample_end); wake_word_confirm_audio_sent(sample_end); }
    CHECK(!queued && !live_queued);
}
static void overflow_checks(void)
{
    reset(); push_frame(900,true); current=APP_STATE_WS_CONNECTING_WAKE;
    for(unsigned i=1;i<=WAKE_WORD_RB_ITEM_COUNT+11;++i) push_frame((int16_t)i,false);
    CHECK(queued==187);
    CHECK(wake_word_prepare_pre_roll_upload()); current=APP_STATE_PLAYING;
    for(unsigned i=1;i<=187;++i) { int16_t output[512]; uint32_t sample_end=0; CHECK(wake_word_read_audio(output,sizeof(output),&sample_end)==sizeof(output)); wake_word_confirm_audio_sent(sample_end); }
    push_frame(999,false);
    CHECK(live_queued==1);
    { int16_t output[512]; uint32_t sample_end=0; CHECK(wake_word_read_audio(output,sizeof(output),&sample_end)==sizeof(output)); wake_word_confirm_audio_sent(sample_end); }
    CHECK(!queued && s_pre_roll_replay_sealed);
    CHECK(s_pre_roll_dropped_frames==144 && s_pre_roll_dropped_older_frames==144);
    reset(); ring_capacity=2; push_frame(900,true);
    current=APP_STATE_WS_CONNECTING_WAKE;
    push_frame(1,false); push_frame(2,false); push_frame(3,false);
    CHECK(queued==2 && s_pre_roll_dropped_frames==1 && s_pre_roll_dropped_older_frames==1);
    CHECK(wake_word_prepare_pre_roll_upload());
    { int16_t output[512]; uint32_t sample_end=0; wake_word_read_audio(output,sizeof(output),&sample_end); wake_word_confirm_audio_sent(sample_end); }
    { int16_t output[512]; uint32_t sample_end=0; wake_word_read_audio(output,sizeof(output),&sample_end); wake_word_confirm_audio_sent(sample_end); }
    CHECK(!queued && !live_queued && s_pre_roll_replay_sealed);
}
#else
static void disabled_checks(void)
{
    reset(); push_frame(900,true); push_frame(901,false);
    CHECK(!queued && !s_pre_roll_enabled && !wake_word_prepare_pre_roll_upload());
    current=APP_STATE_WS_CONNECTING_WAKE;
    for(unsigned i=0;i<157;++i) push_frame(902,false);
    current=APP_STATE_WS_CONNECTED; push_frame(903,false);
    CHECK(!queued && !wake_word_prepare_pre_roll_upload());
    current=APP_STATE_LISTENING; push_frame(904,false);
    CHECK(queued==1); read_frame(904); CHECK(!queued);
}
#endif

int main(void)
{
    normal_fifo_checks();
#if WAKE_WORD_CONNECT_PRE_ROLL_ENABLED
    transmit_boundary_checks();
    handshake_checks(63);
    handshake_checks(157);
    boundary_and_cancel_checks();
    overflow_checks();
#else
    disabled_checks();
#endif
    printf("PASS: %u production microphone FIFO checks (pre_roll=%d)\n",checks,
           WAKE_WORD_CONNECT_PRE_ROLL_ENABLED);
    return 0;
}
