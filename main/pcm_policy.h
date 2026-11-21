#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PCM_CAPACITY 8000u
/* 16 kHz：300 ms 起播储备，覆盖约 230 ms 的短暂供数间隔；总容量仍 500 ms。 */
#define PCM_PREFILL 4800u
#define PCM_BLOCK 160u
#define PCM_DMA_BLOCKS 6u
/* 无结束事件时，短流也最多等待 300 ms；到点播出已有数据，不等下一包。 */
#define PCM_PREFILL_WAIT_MS 300u
#define PCM_VOICE_MEAN_ABS 64u
#define PCM_VOICE_MIN_SAMPLES 640u /* 40 ms 连续有效幅度，过滤单个脉冲。 */
/* UI 只跟随成功入队的音频，短暂断包不切画面；不用于有效语音/静默计时。 */
#define PCM_UI_HOLD_MS 700u
static inline bool pcm_can_start(size_t count, bool eof, uint32_t waited_ms)
{ return count > 0 && (count >= PCM_PREFILL || eof || waited_ms >= PCM_PREFILL_WAIT_MS); }
/* 这是活动检测而非语义识别；阈值按增益前的 signed 16-bit PCM 计。 */
static inline bool pcm_voice_update(uint32_t *qualified_samples, uint32_t abs_sum, size_t samples)
{
    if (!samples || abs_sum / samples < PCM_VOICE_MEAN_ABS) {
        *qualified_samples = 0;
        return false;
    }
    if (*qualified_samples < PCM_VOICE_MIN_SAMPLES) {
        *qualified_samples += (uint32_t)samples;
        if (*qualified_samples > PCM_VOICE_MIN_SAMPLES) *qualified_samples = PCM_VOICE_MIN_SAMPLES;
    }
    return *qualified_samples >= PCM_VOICE_MIN_SAMPLES;
}
static inline size_t pcm_take_count(size_t count)
{ return count < PCM_BLOCK ? count : PCM_BLOCK; }
/* Submitted samples can still be in TX DMA: keep this horizon behind the
 * producer. Trim older history after a delayed microphone read. */
#define PCM_DMA_SAMPLES (PCM_DMA_BLOCKS * PCM_BLOCK)
static inline size_t pcm_reference_skip(size_t count, size_t requested)
{ return count > requested + PCM_DMA_SAMPLES ? count - requested - PCM_DMA_SAMPLES : 0; }
static inline size_t pcm_reference_available(size_t count)
{ return count > PCM_DMA_SAMPLES ? count - PCM_DMA_SAMPLES : 0; }
static inline int16_t pcm_scale(int16_t sample, unsigned gain, unsigned total)
{ return (int32_t)sample * (int32_t)gain / (int32_t)total; }
