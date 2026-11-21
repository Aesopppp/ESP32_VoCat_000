/* VoCat v1.2 audio: bounded mono PCM FIFO -> codec_dev/I2S -> ES8311.
 * One WS/decode producer serializes begin/write/end/stop. The output task owns
 * pacing; its mutex protects FIFO state and hardware writes. Microphone reads
 * use the shared RX clock, which remains running during cancellation. */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2s_std.h"
#define AUDIO_OUT_DEFAULT_VOLUME 40
#define AUDIO_OUT_SAMPLE_RATE 16000
#define AUDIO_OUT_BIT_WIDTH 16
#define AUDIO_OUT_CHANNELS 1

typedef enum {
    AUDIO_OUT_STATE_NONE=0, AUDIO_OUT_STATE_IDLE, AUDIO_OUT_STATE_PLAYING, AUDIO_OUT_STATE_ERROR
} audio_out_state_t;
typedef enum {
    AUDIO_OUT_EVENT_INIT_OK=0, AUDIO_OUT_EVENT_INIT_FAIL, AUDIO_OUT_EVENT_UNDERFLOW, AUDIO_OUT_EVENT_ERROR
} audio_out_event_type_t;
typedef void (*audio_out_event_cb_t)(audio_out_event_type_t ev, void *user_data);

esp_err_t audio_out_init(uint8_t i2c_addr);
esp_err_t audio_out_deinit(void);
/* 主任务调用，百分比 0..100；只设置硬件/RAM，不写 NVS。与 PCM 输出串行。 */
esp_err_t audio_out_set_volume(int volume);
int audio_out_get_volume(void);
/* AFE 任务唯一调用方：读取交错的 16 kHz signed 16-bit M/R，bytes 为总字节数。
 * M=MIC1，R=MIC3 硬件扬声器回采；阻塞到 codec_dev 完成读取，不混入软件参考。
 */
esp_err_t audio_out_read_afe_input(int16_t *pcm, size_t bytes);
/* true 幂等开启接收；缓存空时自然排空，新音频可继续播放，不依赖服务端 EOF。
 * false 仅供显式关闭写入后排空；正常全双工会话不调用。 */
void audio_out_set_streaming(bool active);
/* 成功入队音频后的 700 ms 保持窗，或 PCM/DMA 尚未排空；无锁快照，仅供 UI。
 * 静音音频也保持播放画面，不控制收发，不用于有效语音静默判定。 */
bool audio_out_is_streaming(void);
/* 最近成功入队或提交给输出驱动的有效 PCM 活动，增益前判定；单调毫秒低 32 位。
 * 入队取消旧倒计时，播放尾音继续续期，不要求持续静音流的整个队列排空。
 * 0 表示当前流尚无活动，其他值用无符号差计算；静音/单点脉冲不会刷新。 */
uint32_t audio_out_get_last_voice_ms(void);
bool audio_out_is_drained(void);
/* Enqueue 16 kHz mono signed 16-bit PCM. Length is bytes. Bounded backpressure. */
esp_err_t audio_out_write(const int16_t *pcm_data, size_t pcm_len);
/* Decode into a reusable buffer, then enqueue. Return decoded sample count. */
int audio_out_play_opus(const uint8_t *opus_data, size_t opus_len);
/* Discard queued PCM, ramp to silence and overwrite the TX DMA horizon. */
esp_err_t audio_out_stop(void);
/* 会话停止快速通道：仅写原子标志，解除 PCM 写入等待；不在回调内操作 I2S。 */
void audio_out_request_stop(void);
bool audio_out_stop_requested(void);
esp_err_t audio_out_clear_queue(void);
audio_out_state_t audio_out_get_state(void);
void audio_out_set_event_cb(audio_out_event_cb_t cb, void *arg);
void audio_out_dump_stats(void);
#endif
