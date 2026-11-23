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
static bool s_pre_roll_enabled, s_pre_roll_uploading;
static bool s_first_phrase_speech_seen, s_first_phrase_closed, s_live_buffer_active;
static uint32_t s_first_phrase_silence_samples, s_first_phrase_end, s_live_read_end;
static uint32_t s_wake_boundary_sample, s_afe_sample_end, s_pre_roll_dropped_frames;
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
#include "wake_fifo.inc"

static void reset(void)
{
    wake_word_set_pre_roll(false);
    queued=sent=returned=head=tail=0; ring_capacity=WAKE_WORD_RB_ITEM_COUNT;
    live_queued=live_sent=live_returned=live_head=live_tail=0;
    live_capacity=WAKE_WORD_LIVE_RB_ITEM_COUNT;
    s_afe_sample_end=s_overflow_log_ms=0; s_audio_buffer_enabled=true;
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
    for(unsigned i=15;i<=30;++i) read_frame((int16_t)i);
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
    uint32_t sample_end=0;
    int16_t output[512];
    for(unsigned i=1;i<=32;++i) {
        CHECK(wake_word_read_audio(output,sizeof(output),&sample_end)==sizeof(output));
        CHECK(output[0]==i && output[511]==i);
    }
    /* 即使所有 PCM 都被取去编码，网络尚未发完时也必须继续保护首句。 */
    CHECK(!queued && s_pre_roll_enabled && s_pre_roll_uploading);
    uint32_t end=s_audio_progress.written_end;
    wake_word_confirm_audio_sent(end-8193);
    CHECK(s_pre_roll_enabled); /* 512ms + 1 sample，不能提前解除。 */
    wake_word_confirm_audio_sent(end+1);
    CHECK(s_pre_roll_enabled); /* 无效的未来采样不能解除。 */
    current=APP_STATE_PLAYING;
    for(unsigned i=33;i<=52;++i) push_frame((int16_t)i,false);
    CHECK(queued==20 && !s_pre_roll_dropped_frames);
    for(unsigned i=33;i<=36;++i) {
        CHECK(wake_word_read_audio(output,sizeof(output),&sample_end)==sizeof(output));
        CHECK(output[0]==i);
    }
    end=s_audio_progress.written_end;
    wake_word_confirm_audio_sent(end-8193);
    CHECK(s_pre_roll_enabled);
    wake_word_confirm_audio_sent(end-8192);
    CHECK(!s_pre_roll_enabled && !s_pre_roll_uploading);
    wake_word_confirm_audio_sent(end); /* 重复确认幂等。 */
    CHECK(queued==16); /* 确认本身不清任何待传采样。 */
    /* uint32 采样坐标跨界时仍按差值判断；单个连接可持续多小时。 */
    reset(); s_afe_sample_end=UINT32_MAX-8191; push_frame(900,true);
    current=APP_STATE_WS_CONNECTING_WAKE;
    for(unsigned i=1;i<=20;++i) push_frame((int16_t)i,false);
    CHECK(wake_word_prepare_pre_roll_upload());
    for(unsigned i=1;i<=4;++i)
        CHECK(wake_word_read_audio(output,sizeof(output),&sample_end)==sizeof(output));
    end=s_audio_progress.written_end;
    wake_word_confirm_audio_sent(end-8193); CHECK(s_pre_roll_enabled);
    wake_word_confirm_audio_sent(end-8192); CHECK(!s_pre_roll_enabled);
}
static void handshake_checks(unsigned frames)
{
    reset(); push_frame(900,false); push_frame(901,true);
    CHECK(!queued && s_pre_roll_enabled && !s_pre_roll_uploading);
    CHECK(s_wake_boundary_sample==1024 && s_audio_progress.read_end==1024);
    for(unsigned i=1;i<=frames;++i) {
        /* 第一帧仍处 WAIT_WAKE：覆盖事件已发布、主任务尚未响应的窗口。 */
        current=i==1 ? APP_STATE_WAIT_WAKE : APP_STATE_WS_CONNECTING_WAKE;
        push_frame((int16_t)i,false);
    }
    CHECK(queued==frames && returned==0 && !s_pre_roll_dropped_frames);
    current=APP_STATE_WS_CONNECTED; push_frame((int16_t)(frames+1),false);
    CHECK(wake_word_prepare_pre_roll_upload());
    CHECK(wake_word_prepare_pre_roll_upload()); /* 重复准备不清缓存。 */
    CHECK(s_pre_roll_uploading && queued==frames+1);
    current=APP_STATE_LISTENING; push_frame((int16_t)(frames+2),false);
    CHECK(queued==frames+2 && returned==0); /* 认证后的 producer 不能裁回512ms。 */
    CHECK(read_frame(1)==1024+512 && s_pre_roll_enabled);
    current=APP_STATE_PLAYING; push_frame((int16_t)(frames+3),false);
    for(unsigned i=2;i<=frames+3;++i) read_frame((int16_t)i);
    CHECK(!queued && !s_pre_roll_enabled && !s_pre_roll_uploading);
    CHECK(s_audio_progress.read_end==s_audio_progress.written_end);
    for(unsigned i=1;i<=30;++i) push_frame((int16_t)i,false);
    CHECK(queued==16); read_frame(15); /* 追近实时后恢复普通上限。 */
}
static void boundary_and_cancel_checks(void)
{
    reset(); current=APP_STATE_PLAYING;
    for(unsigned i=1;i<=8;++i) push_frame((int16_t)i,false);
    current=APP_STATE_WAIT_WAKE; push_frame(900,true);
    CHECK(!queued && returned==8 && s_wake_boundary_sample==9*512);
    push_frame(901,false); CHECK(wake_word_prepare_pre_roll_upload());
    CHECK(read_frame(901)==10*512); /* 判定帧和其前音频均不上传；不额外裁0.5~1s。 */
    CHECK(!s_pre_roll_enabled);
    wake_word_set_pre_roll(false);
    CHECK(!queued && !s_audio_progress.written_end && !s_wake_boundary_sample);
    current=APP_STATE_WS_CONNECTING_WAKE; push_frame(902,false);
    CHECK(!queued && !wake_word_prepare_pre_roll_upload()); /* 无唤醒的按键/提醒不预录音。 */
    current=APP_STATE_WAIT_WAKE; push_frame(910,true); push_frame(911,false);
    wake_word_set_pre_roll(false); /* 连接失败/用户取消。 */
    CHECK(!queued && !s_pre_roll_enabled && !s_pre_roll_uploading);
    push_frame(920,true); push_frame(921,false);
    CHECK(wake_word_prepare_pre_roll_upload()); read_frame(921); CHECK(!queued);
}
static void overflow_checks(void)
{
    reset(); push_frame(900,true); current=APP_STATE_WS_CONNECTING_WAKE;
    for(unsigned i=1;i<=WAKE_WORD_RB_ITEM_COUNT+11;++i) push_frame((int16_t)i,false);
    CHECK(queued==WAKE_WORD_RB_ITEM_COUNT && !returned && s_pre_roll_dropped_frames==11);
    CHECK(s_audio_progress.written_end==512*(1+WAKE_WORD_RB_ITEM_COUNT));
    CHECK(wake_word_prepare_pre_roll_upload()); current=APP_STATE_PLAYING;
    read_frame(1); push_frame(999,false); /* 腾出空间后可追加最新音频，已保留帧不乱序。 */
    CHECK(queued==WAKE_WORD_RB_ITEM_COUNT);
    for(unsigned i=2;i<=WAKE_WORD_RB_ITEM_COUNT;++i) read_frame((int16_t)i);
    read_frame(999); CHECK(!queued && !s_pre_roll_enabled && s_pre_roll_dropped_frames==11);
    reset(); ring_capacity=2; push_frame(900,true);
    current=APP_STATE_WS_CONNECTING_WAKE;
    push_frame(1,false); push_frame(2,false); push_frame(3,false);
    CHECK(queued==2 && s_pre_roll_dropped_frames==1); /* RTOS实际空间不足也丢新保旧。 */
    CHECK(wake_word_prepare_pre_roll_upload()); read_frame(1); read_frame(2);
    CHECK(!queued);
}
#if WAKE_WORD_FIRST_PHRASE_HANDOFF_ENABLED
static void enter_lite_mode(void)
{
    s_mode=WAKE_WORD_MODE_LITE;
    reset_first_phrase_detector_locked();
}
static void first_phrase_handoff_checks(void)
{
    /* Silence before the first speech is not a phrase; a short gap is retained. */
    reset(); push_frame(900,true); current=APP_STATE_WS_CONNECTING_WAKE; enter_lite_mode();
    for(unsigned i=1;i<=20;++i) push_vad_frame((int16_t)i,false,VAD_SILENCE);
    CHECK(!s_first_phrase_speech_seen && !s_first_phrase_closed && queued==20);
    push_frame(1021,false); CHECK(s_first_phrase_speech_seen);
    for(unsigned i=22;i<=25;++i) push_vad_frame((int16_t)i,false,VAD_SILENCE);
    CHECK(!s_first_phrase_closed && s_first_phrase_silence_samples==2048);
    push_frame(26,false); CHECK(!s_first_phrase_silence_samples);
    for(unsigned i=27;i<=30;++i) push_vad_frame((int16_t)i,false,VAD_SILENCE);
    CHECK(!s_first_phrase_closed);
    push_vad_frame(31,false,VAD_SILENCE);
    CHECK(s_first_phrase_closed && !s_live_buffer_active && queued==31);
    uint32_t end=s_first_phrase_end;
    CHECK(end==32*512 && s_live_read_end==end && !live_queued);
    for(unsigned i=32;i<=71;++i) push_frame((int16_t)i,false);
    CHECK(s_first_phrase_end==end && queued==31 && live_queued==16);
    CHECK(s_live_read_end==end+24*512 && s_audio_progress.read_end==512);
    CHECK(s_audio_progress.written_end==72*512 && !s_pre_roll_dropped_frames);
    CHECK(wake_word_prepare_pre_roll_upload()); current=APP_STATE_PLAYING;
    /* TX progress cannot unlock a frozen phrase, even when the live tail is short. */
    wake_word_confirm_audio_sent(s_audio_progress.written_end);
    CHECK(s_pre_roll_enabled && s_first_phrase_closed && !s_live_buffer_active);
    for(unsigned i=1;i<=31;++i) CHECK(read_frame((int16_t)(i==21 ? 1021 : i))==(i+1)*512);
    CHECK(!queued && s_pre_roll_enabled && !s_live_buffer_active);
    /* Source switches at the next read, without a TX confirmation or a server ACK. */
    int16_t output[512]; uint32_t sample_end=0;
    CHECK(wake_word_read_audio(output,sizeof(output),&sample_end)==sizeof(output));
    CHECK(output[0]==56 && output[511]==56 && sample_end==57*512);
    CHECK(s_live_buffer_active && !s_pre_roll_enabled && !s_pre_roll_uploading);
    for(unsigned i=57;i<=71;++i) CHECK(read_frame((int16_t)i)==(i+1)*512);
    CHECK(!live_queued && s_audio_progress.read_end==s_audio_progress.written_end);
    CHECK(wake_word_read_audio(output,sizeof(output),&sample_end)==0);
    /* Later audio remains ordinary bounded live FIFO for the whole session. */
    for(unsigned i=72;i<=101;++i) push_frame((int16_t)i,false);
    CHECK(!queued && live_queued==16);
    for(unsigned i=86;i<=101;++i) CHECK(read_frame((int16_t)i)==(i+1)*512);
    CHECK(!live_queued && s_audio_progress.read_end==s_audio_progress.written_end);
    wake_word_set_pre_roll(false);
    CHECK(!queued && !live_queued && !s_first_phrase_closed && !s_live_buffer_active);
    CHECK(!s_audio_progress.written_end && !s_live_read_end && !s_first_phrase_end);
}

static void first_phrase_empty_live_and_wrap_checks(void)
{
    /* A closed phrase can hand off before any new PCM exists. Its last samples
     * may still be in the encoder half-frame: read_audio must not fabricate PCM. */
    reset(); s_afe_sample_end=UINT32_MAX-1535; push_frame(900,true);
    current=APP_STATE_WS_CONNECTING_WAKE; enter_lite_mode();
    uint32_t boundary=s_wake_boundary_sample;
    push_frame(1001,false);
    for(unsigned i=2;i<=6;++i) push_vad_frame((int16_t)i,false,VAD_SILENCE);
    CHECK(s_first_phrase_closed && queued==6 && !live_queued);
    CHECK(wake_word_prepare_pre_roll_upload()); current=APP_STATE_LISTENING;
    for(unsigned i=1;i<=6;++i) CHECK(read_frame((int16_t)(i==1 ? 1001 : i))==boundary+i*512);
    int16_t output[512]; uint32_t sample_end=0;
    CHECK(wake_word_read_audio(output,sizeof(output),&sample_end)==0);
    CHECK(s_live_buffer_active && !s_pre_roll_enabled);
    for(unsigned i=7;i<=30;++i) push_frame((int16_t)i,false);
    for(unsigned i=15;i<=30;++i) CHECK(read_frame((int16_t)i)==boundary+i*512);
    CHECK(s_audio_progress.written_end==s_audio_progress.read_end);
    /* Full replay FIFO still closes; missing new frames remain explicit. */
    reset(); ring_capacity=2; push_frame(900,true); current=APP_STATE_WS_CONNECTING_WAKE; enter_lite_mode();
    push_frame(1001,false); push_frame(2,false);
    for(unsigned i=3;i<=7;++i) push_vad_frame((int16_t)i,false,VAD_SILENCE);
    CHECK(s_first_phrase_closed && s_pre_roll_dropped_frames==5 && queued==2);
    push_frame(8,false); CHECK(live_queued==1);
    CHECK(wake_word_prepare_pre_roll_upload()); current=APP_STATE_LISTENING;
    CHECK(read_frame(1001)==1024); CHECK(read_frame(2)==1536);
    CHECK(read_frame(8)==2048); /* Coordinates count successfully stored samples only. */
    CHECK(s_audio_progress.written_end==s_audio_progress.read_end);
    /* Failure/stop must also drain the side FIFO, so no old live audio reappears. */
    push_frame(9,false); wake_word_set_audio_buffer_enabled(false);
    CHECK(!queued && !live_queued && !s_first_phrase_closed && !s_live_buffer_active);
    wake_word_set_audio_buffer_enabled(true); current=APP_STATE_WAIT_WAKE;
    push_frame(900,true); push_frame(10,false); CHECK(wake_word_prepare_pre_roll_upload());
    CHECK(read_frame(10)==s_wake_boundary_sample+512 && !live_queued);
}

static void first_phrase_mode_boundary_checks(void)
{
    reset(); push_frame(900,true); current=APP_STATE_WS_CONNECTING_WAKE;
    /* WAKE's trailing speech and following silence cannot finish the first phrase. */
    push_frame(1001,false);
    for(unsigned i=0;i<12;++i) push_vad_frame(0,false,VAD_SILENCE);
    CHECK(queued==13 && !s_first_phrase_speech_seen && !s_first_phrase_closed);
    /* The production mode-switch helper resets any pending detector state only. */
    s_first_phrase_speech_seen=true; s_first_phrase_silence_samples=2048;
    uint32_t written=s_audio_progress.written_end, read=s_audio_progress.read_end;
    enter_lite_mode();
    CHECK(!s_first_phrase_speech_seen && !s_first_phrase_silence_samples);
    CHECK(queued==13 && written==s_audio_progress.written_end && read==s_audio_progress.read_end);
    for(unsigned i=0;i<512;++i) frame[i]=1000;
    afe_fetch_result_t stale_wake_frame={frame,sizeof(frame),VAD_SPEECH};
    test_time_us+=32000;
    buffer_afe_audio(&stale_wake_frame,false,false);
    CHECK(!s_first_phrase_speech_seen && queued==14); /* Late WAKE PCM is retained, its VAD ignored. */
    for(unsigned i=0;i<12;++i) push_vad_frame(0,false,VAD_SILENCE);
    CHECK(!s_first_phrase_closed && queued==26);
    /* Near-silent residual SPEECH does not arm a phrase, but later real speech does. */
    push_frame(2,false);
    for(unsigned i=0;i<5;++i) push_vad_frame(0,false,VAD_SILENCE);
    CHECK(!s_first_phrase_speech_seen && !s_first_phrase_closed);
    push_frame(1002,false); CHECK(s_first_phrase_speech_seen);
    for(unsigned i=0;i<4;++i) push_vad_frame(0,false,VAD_SILENCE);
    CHECK(!s_first_phrase_closed);
    push_frame(2,false); CHECK(!s_first_phrase_silence_samples); /* Weak speech keeps a phrase open. */
    for(unsigned i=0;i<5;++i) push_vad_frame(0,false,VAD_SILENCE);
    CHECK(s_first_phrase_closed);
    uint32_t end=s_first_phrase_end;
    reset_first_phrase_detector_locked();
    CHECK(s_first_phrase_closed && s_first_phrase_end==end); /* Frozen boundary is immutable. */
}
#else
static void first_phrase_disabled_checks(void)
{
    reset(); push_frame(900,true); current=APP_STATE_WS_CONNECTING_WAKE;
    push_frame(1,false);
    for(unsigned i=2;i<=50;++i) push_vad_frame((int16_t)i,false,VAD_SILENCE);
    CHECK(!s_first_phrase_closed && !s_live_buffer_active && queued==50 && !live_queued);
    CHECK(wake_word_prepare_pre_roll_upload());
    for(unsigned i=1;i<=50;++i) read_frame((int16_t)i);
    CHECK(!queued && !s_pre_roll_enabled);
}
#endif
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
    handshake_checks(63);  /* 2016 ms建连 */
    handshake_checks(157); /* 5024 ms建连 */
    boundary_and_cancel_checks(); overflow_checks();
#if WAKE_WORD_FIRST_PHRASE_HANDOFF_ENABLED
    first_phrase_handoff_checks(); first_phrase_empty_live_and_wrap_checks();
    first_phrase_mode_boundary_checks();
#else
    first_phrase_disabled_checks();
#endif
#else
    disabled_checks();
#endif
    printf("PASS: %u production microphone FIFO checks (pre_roll=%d first_phrase=%d)\n",checks,
           WAKE_WORD_CONNECT_PRE_ROLL_ENABLED, WAKE_WORD_FIRST_PHRASE_HANDOFF_ENABLED);
    return 0;
}
