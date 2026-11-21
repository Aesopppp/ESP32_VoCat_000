/* 直接运行生产 PCM 输出步骤；无 EOF 的短流、排空、续播与活动计时。
 * 驱动桩每块推进 10 ms，不模拟真实 DMA 时延、声学或 RTOS 调度。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_err.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "../../main/pcm_policy.h"
#define AUDIO_OUT_SAMPLE_RATE 16000
#define AUDIO_OUT_STATE_IDLE 1
#define AUDIO_OUT_STATE_PLAYING 2
#define AUDIO_OUT_STATE_ERROR 3
#define EV_AUDIO_OUT_START 1
#define ESP_LOGI(...) ((void)0)
static unsigned checks, events, writes;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static void *speaker=(void*)1;
static SemaphoreHandle_t output_lock=(void*)2;
static atomic_bool stop_requested, playback_streaming;
static atomic_uint last_voice_ms;
static bool stream_open,stream_eof,stream_drained=true,output_started;
static bool pcm_received;
static uint32_t last_pcm_ms;
static size_t pcm_read,pcm_write,pcm_count;
static unsigned drain_blocks,fade_samples;
static uint32_t voice_samples, received_voice_samples;
static uint32_t received_abs_sum;
static size_t received_samples;
static int16_t pcm_ring[PCM_CAPACITY],last_sample;
static int state,output_volume=0;
static uint32_t underruns,supplied_samples,decoded_packets,decode_max_us,write_max_us;
static uint32_t decode_total_us,decode_samples,decode_over_budget;
static uint32_t signal_peak,signal_samples,signal_nonzero;
static uint64_t signal_abs_sum;
static int64_t stats_at,prefill_started_us,test_time_us;
static bool driver_fails;
static esp_err_t output_pcm_block(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
int64_t esp_timer_get_time(void) { return test_time_us; }
void vTaskDelay(TickType_t t) { test_time_us+=(int64_t)t*1000; (void)output_pcm_block(); }
static void event_bus_publish(int ev,void *p,size_t n)
{ (void)p; (void)n; CHECK(ev==EV_AUDIO_OUT_START); ++events; }
static esp_err_t write_block(int16_t *pcm)
{ (void)pcm; ++writes; test_time_us+=10000; return driver_fails?ESP_FAIL:ESP_OK; }
#include "audio_continuous.inc"
static void steps(unsigned count) { while(count--) CHECK(output_pcm_block()==ESP_OK); }
static void begin(void)
{
    stream_open=false; stop_requested=false; playback_streaming=false;
    last_sample=0; driver_fails=false; test_time_us=1000000;
    events=writes=0; audio_out_set_streaming(true);
}
static void fill(int16_t *p,size_t n,int value) { for(size_t i=0;i<n;++i) p[i]=(int16_t)value; }
int main(void)
{
    int16_t pcm[PCM_PREFILL];
    CHECK(!pcm_can_start(0,false,1000));
    CHECK(!pcm_can_start(320,false,PCM_PREFILL_WAIT_MS-1));
    CHECK(pcm_can_start(320,false,PCM_PREFILL_WAIT_MS));
    begin(); steps(100);
    CHECK(!audio_out_is_streaming() && !pcm_received && events==0);
    CHECK(audio_out_write(pcm,0)==ESP_OK); steps(1);
    CHECK(!audio_out_is_streaming() && !pcm_received); /* 开流/空包不点亮画面。 */
    begin(); fill(pcm,320,1000);
    CHECK(audio_out_write(pcm,320*sizeof(int16_t))==ESP_OK);
    steps(30); CHECK(supplied_samples==0 && pcm_count==320);
    CHECK(audio_out_is_streaming() && events==1); /* 预填期间已显示，不等幅度/40 ms。 */
    steps(2); CHECK(supplied_samples==320 && pcm_count==0); /* 短流不等 EOF。 */
    steps(8); CHECK(audio_out_is_drained() && stream_open && state==AUDIO_OUT_STATE_IDLE);
    CHECK(audio_out_is_streaming()); /* 排空后短暂停顿不切回监听。 */
    CHECK(!audio_out_get_last_voice_ms()); /* 20 ms 单次脉冲不足活动窗口，但音频仍完整输出。 */
    fill(pcm,PCM_PREFILL,1000);
    CHECK(audio_out_write(pcm,sizeof(pcm))==ESP_OK);
    CHECK(audio_out_get_last_voice_ms()==(uint32_t)(test_time_us/1000)); /* 入队即续期。 */
    audio_out_set_streaming(true); /* 幂等调用不能清除之前或这次缓存。 */
    steps(4); CHECK(audio_out_get_last_voice_ms()!=0 && audio_out_is_streaming());
    CHECK(events==1 && output_volume==0); /* 硬件静音不改变远端有效语音活动。 */
    uint32_t active=audio_out_get_last_voice_ms();
    steps(26); CHECK(supplied_samples==320+PCM_PREFILL && pcm_count==0);
    CHECK(audio_out_get_last_voice_ms()>active);
    active=audio_out_get_last_voice_ms(); steps(30);
    CHECK(audio_out_is_drained() && audio_out_is_streaming());
    steps(10); /* 最后入队后 700 ms。 */
    CHECK(audio_out_is_drained() && !audio_out_is_streaming() && stream_open);
    CHECK(audio_out_get_last_voice_ms()==active);
    fill(pcm,PCM_PREFILL,0);
    for(unsigned seconds=0;seconds<65;++seconds) {
        CHECK(audio_out_write(pcm,sizeof(pcm))==ESP_OK); steps(1);
        CHECK(audio_out_is_streaming()); /* 静音包也显示，但有效语音时刻不更新。 */
        steps(99);
        CHECK(audio_out_get_last_voice_ms()==active && !audio_out_is_streaming());
    } /* 连续静音包超过 30 秒也不续时。 */
    CHECK(supplied_samples==320+PCM_PREFILL*66);
    fill(pcm,PCM_PREFILL,63);
    CHECK(audio_out_write(pcm,sizeof(pcm))==ESP_OK); steps(40);
    CHECK(audio_out_get_last_voice_ms()==active && audio_out_is_streaming());
    /* 低幅底噪不作为有效语音，但不让播放画面闪动。 */
    fill(pcm,PCM_PREFILL,64);
    CHECK(audio_out_write(pcm,sizeof(pcm))==ESP_OK); steps(30);
    CHECK(audio_out_get_last_voice_ms()!=active); /* 阈值边界。 */
    begin(); memset(pcm,0,sizeof(pcm)); pcm[0]=INT16_MIN;
    CHECK(audio_out_write(pcm,sizeof(pcm))==ESP_OK); steps(40);
    CHECK(!audio_out_get_last_voice_ms()); /* 单点高峰不刷新待机计时。 */
    begin(); fill(pcm,PCM_PREFILL,1000);
    test_time_us=((int64_t)UINT32_MAX-39)*1000;
    CHECK(audio_out_write(pcm,sizeof(pcm))==ESP_OK); steps(4);
    CHECK(audio_out_get_last_voice_ms()==UINT32_MAX); /* 回绕瞬间不输出 0 哨兵。 */
    steps(1); CHECK(audio_out_get_last_voice_ms()==10 && audio_out_is_streaming());
    active=audio_out_get_last_voice_ms(); driver_fails=true;
    CHECK(output_pcm_block()==ESP_FAIL && state==AUDIO_OUT_STATE_ERROR);
    CHECK(audio_out_get_last_voice_ms()==active && !audio_out_is_streaming());
    driver_fails=false; steps(1); CHECK(!audio_out_is_streaming()); /* 错误后旧时刻不重亮。 */

    begin(); fill(pcm,320,0); test_time_us=0;
    CHECK(audio_out_write(pcm,320*sizeof(int16_t))==ESP_OK); steps(40);
    CHECK(audio_out_is_drained() && audio_out_is_streaming() && last_pcm_ms==0);
    test_time_us=(PCM_UI_HOLD_MS-11)*1000; steps(1);
    CHECK(audio_out_is_streaming()); /* 699 ms。 */
    test_time_us=(PCM_UI_HOLD_MS-10)*1000; steps(1);
    CHECK(!audio_out_is_streaming() && !pcm_received && !audio_out_get_last_voice_ms());
    test_time_us=((int64_t)UINT32_MAX+101)*1000; steps(1);
    CHECK(!audio_out_is_streaming()); /* 已过期时间戳不能在整周回绕后重亮。 */

    begin(); test_time_us=((int64_t)UINT32_MAX-99)*1000;
    CHECK(audio_out_write(pcm,320*sizeof(int16_t))==ESP_OK); steps(40);
    CHECK(audio_out_is_drained() && audio_out_is_streaming());
    steps(29); CHECK(audio_out_is_streaming());
    steps(1); CHECK(!audio_out_is_streaming()); /* 700 ms 保持窗跨毫秒回绕。 */

    begin(); fill(pcm,PCM_PREFILL,0);
    CHECK(audio_out_write(pcm,sizeof(pcm))==ESP_OK);
    test_time_us+=(PCM_UI_HOLD_MS+100)*1000; steps(30);
    CHECK(!pcm_count && audio_out_is_streaming()); /* 超过保持窗但 DMA 尾音尚未排空。 */
    steps(PCM_DMA_BLOCKS); CHECK(audio_out_is_streaming());
    steps(1); CHECK(audio_out_is_drained() && !audio_out_is_streaming());

    begin(); CHECK(audio_out_write(pcm,sizeof(pcm))==ESP_OK); steps(1);
    CHECK(audio_out_is_streaming());
    stop_requested=true; CHECK(!audio_out_is_streaming()); steps(1);
    CHECK(!pcm_received && !audio_out_is_streaming());
    begin(); steps(1); CHECK(!audio_out_is_streaming()); /* 新流不继承旧包和保持期。 */

    begin(); fill(pcm,960,0);
    for(unsigned packets=0;packets<600;++packets) {
        CHECK(audio_out_write(pcm,960*sizeof(int16_t))==ESP_OK); steps(6);
        CHECK(audio_out_is_streaming() && !audio_out_get_last_voice_ms());
    } /* 真实连续 60 ms 静音包超过 30 秒，即使缓冲非空也不续期。 */
    CHECK(pcm_count>0);
    fill(pcm,960,1000);
    CHECK(audio_out_write(pcm,960*sizeof(int16_t))==ESP_OK);
    active=audio_out_get_last_voice_ms(); CHECK(active==(uint32_t)(test_time_us/1000));
    steps(40); CHECK(audio_out_get_last_voice_ms()>active); /* 缓存尾音按播放时间续期。 */

    begin(); fill(pcm,320,1000);
    CHECK(audio_out_write(pcm,320*sizeof(int16_t))==ESP_OK); steps(100);
    CHECK(!audio_out_get_last_voice_ms());
    CHECK(audio_out_write(pcm,320*sizeof(int16_t))==ESP_OK); steps(100);
    CHECK(!audio_out_get_last_voice_ms()); /* 隔一秒的短脉冲不能累计过门限。 */

    begin(); fill(pcm,960,0); fill(pcm,160,1000);
    for(unsigned packets=0;packets<600;++packets) {
        CHECK(audio_out_write(pcm,960*sizeof(int16_t))==ESP_OK); steps(6);
        CHECK(!audio_out_get_last_voice_ms());
    } /* 每包10 ms脉冲+50 ms静音不应按整包均值误续期。 */
    printf("PASS: %u production continuous PCM checks\n",checks);
    return 0;
}
