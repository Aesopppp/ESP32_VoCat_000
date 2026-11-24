/**
 * @file wake_word.c
 * @brief 唤醒词检测实现 — esp-sr WakeNet
 *
 * 任务分工：
 *  feed_Task   (core 0) : I²S DMA → AFE feed
 *  detect_Task (core 1) : AFE fetch → WAKENET_DETECTED → 回调
 *
 * PCM 数据通过 ring buffer 共享给 audio_in：
 *  detect_Task 写入 AFE 输出 → ring buffer → audio_in_task 编码上传
 */

#include "wake_word.h"
#include "board_config.h"
#include "event_bus.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "audio_out.h"
#include "app_state.h"
#include "model_path.h"
#include "string.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "esp_timer.h"

static const char *TAG = "wake_word";

/* ===========================================================================
 * 全局状态
 * ========================================================================= */

/** AFE 接口与实例 */
static const esp_afe_sr_iface_t *s_afe_iface     = NULL;
static void                    *s_afe_data_wake = NULL;  /* Full AFE (with WakeNet) - for WAIT_WAKE */
static void                    *s_afe_data_lite = NULL;  /* AEC+VAD only (no WakeNet) - for LISTENING/PLAYING */
static void                    *s_afe_data      = NULL;  /* Currently active handle */
static wake_word_mode_t         s_mode          = WAKE_WORD_MODE_WAKE;

/** ring buffer 句柄（detect_Task → audio_in） */
static RingbufHandle_t  s_ringbuf = NULL;
/* 首句闭合后滚动保存实时 PCM；大 FIFO 冻结直到唯一读取端读完。 */
static RingbufHandle_t  s_live_ringbuf = NULL;

/** feed/detect 任务句柄 */
static TaskHandle_t     s_feed_task_h    = NULL;
static TaskHandle_t     s_detect_task_h  = NULL;

/** 运行标志 */
static volatile bool    s_task_running   = false;

/** 唤醒状态（detect_Task 写，app_main 读） */
static volatile wake_word_state_t s_wake_state = WAKE_STATE_IDLE;

/** 唤醒回调 */
static wake_word_detected_cb_t  s_wake_cb       = NULL;
static void                    *s_wake_cb_arg   = NULL;

/** 栈高水位 */
static uint32_t s_feed_stack_hiwat   = 0;
static uint32_t s_detect_stack_hiwat = 0;

/** fetch 帧大小（samples） */
static int s_afe_chunksize = 0;
static int s_fetch_chunksize = 0;
/* 普通连续上传只保留短 FIFO；仅唤醒建连和随后的追赶阶段保护更长历史。 */
#define AUDIO_UPLOAD_FIFO_MS 512u
#define AUDIO_DIAGNOSTIC_ENERGY 2500
static SemaphoreHandle_t s_audio_buffer_lock;
static bool s_audio_buffer_enabled = true; /* 同一缓存锁保护；会话内一直允许连续上传。 */
static bool s_pre_roll_enabled = false; /* 与 FIFO 一起由 s_audio_buffer_lock 保护。 */
static bool s_pre_roll_uploading;
static bool s_hot_pre_roll; /* Only a WakeNet wake using an already connected socket. */
static bool s_first_phrase_speech_seen, s_first_phrase_closed, s_live_buffer_active;
static uint32_t s_first_phrase_silence_samples, s_first_phrase_end, s_live_read_end;
static uint32_t s_wake_boundary_sample = 0;
static uint32_t s_afe_sample_end; /* 每个有效 fetch 帧推进，包括待机；不随 FIFO 清空复位。 */
static uint32_t s_pre_roll_dropped_frames;
static wake_word_audio_progress_t s_audio_progress;
static uint32_t s_overflow_log_ms;
static uint32_t s_overflow_frames; /* 限频日志之间累计，不能只报最后一次丢帧。 */
static int32_t afe_frame_energy(const int16_t *pcm, int samples)
{
    if (!pcm || samples <= 0) return 0;
    int64_t sum = 0;
    for (int i = 0; i < samples; ++i) {
        int32_t sample = pcm[i];
        sum += (int64_t)sample * sample;
    }
    return (int32_t)(sum / samples);
}


static size_t audio_buffer_frames_locked(RingbufHandle_t ringbuf)
{
    UBaseType_t waiting = 0;
    if (ringbuf) vRingbufferGetInfo(ringbuf, NULL, NULL, NULL, NULL, &waiting);
    return waiting;
}

static bool discard_oldest_audio_locked(RingbufHandle_t ringbuf, uint32_t *read_end)
{
    if (!ringbuf) return false;
    size_t bytes;
    void *old = xRingbufferReceive(ringbuf, &bytes, 0);
    if (!old) return false;
    *read_end += bytes / sizeof(int16_t);
    vRingbufferReturnItem(ringbuf, old);
    return true;
}

/* 清空和模式切换共用此入口，复位同一 FIFO 的采样位置。调用方持锁。 */
static void clear_audio_buffer_locked(void)
{
    while (discard_oldest_audio_locked(s_ringbuf, &s_audio_progress.read_end)) {}
    while (discard_oldest_audio_locked(s_live_ringbuf, &s_live_read_end)) {}
    memset(&s_audio_progress, 0, sizeof(s_audio_progress));
    s_overflow_frames = 0;
    s_pre_roll_enabled = s_pre_roll_uploading = s_hot_pre_roll = false;
    s_first_phrase_speech_seen = s_first_phrase_closed = s_live_buffer_active = false;
    s_first_phrase_silence_samples = s_first_phrase_end = s_live_read_end = 0;
    s_wake_boundary_sample = s_pre_roll_dropped_frames = 0;
}

/* 新 LITE AFE 不继承 WAKE AFE 的 VAD 状态；只复位检测器，不动已缓存 PCM。
 * 闭合的首句边界是只读的，不能被重复模式通知重新打开。调用方持缓存锁。 */
static void reset_first_phrase_detector_locked(void)
{
    if (s_first_phrase_closed) return;
    s_first_phrase_speech_seen = false;
    s_first_phrase_silence_samples = 0;
}

static bool observe_first_phrase_locked(int vad_state, bool valid_voice, bool lite_source)
{
    /* 唤醒词的 SPEECH 尾状态不算第一句话。WAKE 模式仍完整缓存 PCM；
     * 等真正切入 LITE 后才使用该 AFE 自己的 VAD 来划分首句。 */
    if (!WAKE_WORD_FIRST_PHRASE_HANDOFF_ENABLED || !s_pre_roll_enabled ||
        s_first_phrase_closed || !s_live_ringbuf || s_mode != WAKE_WORD_MODE_LITE || !lite_source)
        return false;
    if (vad_state == VAD_SPEECH) {
        if (valid_voice) s_first_phrase_speech_seen = true;
        /* 弱音也中止静音累计；宁可多留历史，不用低能量误切正常句子。 */
        s_first_phrase_silence_samples = 0;
    } else if (vad_state == VAD_SILENCE && s_first_phrase_speech_seen) {
        s_first_phrase_silence_samples += s_fetch_chunksize;
        bool short_activation = s_hot_pre_roll &&
            s_audio_progress.written_end - s_wake_boundary_sample <=
                BSP_I2S_SAMPLE_RATE / 1000u * AUDIO_UPLOAD_FIFO_MS;
        if (!short_activation && s_first_phrase_silence_samples >=
            BSP_I2S_SAMPLE_RATE / 1000u * WAKE_WORD_FIRST_PHRASE_SILENCE_MS) {
            /* 包含已存入的句尾静音；首句满队列时缺失帧已计入 dropped_new。
             * 这里只分段补传缓存，不判断语义、控制播放或发送 cancel。 */
            s_first_phrase_closed = true;
            s_first_phrase_end = s_audio_progress.written_end;
            s_live_read_end = s_first_phrase_end;
            return true;
        }
    }
    return false;
}

void wake_word_get_audio_progress(wake_word_audio_progress_t *progress)
{
    if (!progress) return;
    memset(progress, 0, sizeof(*progress));
    if (!s_audio_buffer_lock) return;
    xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
    *progress = s_audio_progress;
    xSemaphoreGive(s_audio_buffer_lock);
}

size_t wake_word_read_audio(int16_t *pcm, size_t capacity, uint32_t *sample_end)
{
    if (!s_ringbuf || !pcm || !sample_end) return 0;
    xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
    bool switched = false, was_hot = false;
    uint32_t skipped_samples = 0, live_samples = 0;
    if (s_first_phrase_closed && !s_live_buffer_active &&
        audio_buffer_frames_locked(s_ringbuf) == 0) {
        /* 上一帧已交给同一个编码任务，半个 Opus 帧和 TX 队列仍保持顺序。
         * 只跳过 live FIFO 已淘汰的旧帧，不丢首句，不等待 TX/平台确认。 */
        skipped_samples = s_live_read_end - s_audio_progress.read_end;
        s_audio_progress.read_end = s_live_read_end;
        live_samples = s_audio_progress.written_end - s_live_read_end;
        s_live_buffer_active = true;
        was_hot = s_hot_pre_roll;
        s_pre_roll_enabled = s_pre_roll_uploading = s_hot_pre_roll = false;
        switched = true;
    }
    RingbufHandle_t ringbuf = s_live_buffer_active ? s_live_ringbuf : s_ringbuf;
    size_t bytes = 0;
    void *item = xRingbufferReceive(ringbuf, &bytes, 0);
    if (item) {
        s_audio_progress.read_end += bytes / sizeof(int16_t);
        if (s_live_buffer_active) s_live_read_end = s_audio_progress.read_end;
        *sample_end = s_audio_progress.read_end;
        if (bytes <= capacity) memcpy(pcm, item, bytes);
        /* 超容量返回实际长度，让消费者显式报错，不能把异常帧当作队列空。 */
        vRingbufferReturnItem(ringbuf, item);
    }
    xSemaphoreGive(s_audio_buffer_lock);
    if (switched)
        ESP_LOGI(TAG, "%s queued; switched to live audio (skipped_ms=%lu live_ms=%lu)",
                 was_hot ? "Hot pre-roll" : "First phrase",
                 (unsigned long)(skipped_samples / (BSP_I2S_SAMPLE_RATE / 1000u)),
                 (unsigned long)(live_samples / (BSP_I2S_SAMPLE_RATE / 1000u)));
    return bytes;
}

void wake_word_confirm_audio_sent(uint32_t sample_end)
{
    if (!s_audio_buffer_lock) return;
    bool caught_up = false;
    uint32_t pending_samples = 0, dropped = 0;
    xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
    /* 首句尚未闭合时保留提前追近实时的路径。闭合后由读取端切源，
     * 不让迟到的发送确认打乱两个 FIFO 的边界。生命周期切换仍须先
     * 停止两个上传任务再清 FIFO，禁止旧连接确认影响新连接。 */
    pending_samples = s_audio_progress.written_end - sample_end;
    if (s_pre_roll_enabled && s_pre_roll_uploading && !s_first_phrase_closed &&
        pending_samples <= BSP_I2S_SAMPLE_RATE / 1000u * AUDIO_UPLOAD_FIFO_MS) {
        s_pre_roll_enabled = s_pre_roll_uploading = s_hot_pre_roll = false;
        dropped = s_pre_roll_dropped_frames;
        caught_up = true;
    }
    xSemaphoreGive(s_audio_buffer_lock);
    if (caught_up)
        ESP_LOGI(TAG, "Pre-roll replay caught up; live FIFO restored (pending_ms=%lu dropped_new=%lu)",
                 (unsigned long)(pending_samples / (BSP_I2S_SAMPLE_RATE / 1000u)),
                 (unsigned long)dropped);
}

/* WakeNet 判定帧本身不上传；其后 PCM 在建连/补传期间受保护。普通连续
 * 上传仍只保留 512 ms。预录音满时丢新帧保首句，诊断明确报告这段缺失。 */
static void buffer_afe_audio(const afe_fetch_result_t *res, bool wake_detected, bool lite_source)
{
    if (!s_ringbuf || !res->data || res->data_size <= 0) return;
    const size_t frame_bytes = (size_t)s_fetch_chunksize * sizeof(int16_t);
    if ((size_t)res->data_size != frame_bytes) {
        ESP_LOGW(TAG, "Unexpected AFE frame size: %d (expected %u)",
                 res->data_size, (unsigned)frame_bytes);
        return;
    }
    xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
    s_afe_sample_end += s_fetch_chunksize;
    if (WAKE_WORD_CONNECT_PRE_ROLL_ENABLED && wake_detected && s_audio_buffer_enabled) {
        /* 同一锁内裁掉判定帧及其之前的声音，再允许后续帧入队；先于发布
         * 唤醒事件，故主任务尚未处理事件的 WAIT_WAKE 短窗口也不会丢句首。 */
        clear_audio_buffer_locked();
        s_wake_boundary_sample = s_afe_sample_end;
        s_audio_progress.written_end = s_audio_progress.read_end = s_wake_boundary_sample;
        s_pre_roll_enabled = true;
        xSemaphoreGive(s_audio_buffer_lock);
        return;
    }
    app_state_t current = app_state_get();
    bool pre_roll = s_pre_roll_enabled &&
        (current == APP_STATE_WAIT_WAKE || current == APP_STATE_WS_CONNECTING_WAKE ||
         current == APP_STATE_WS_CONNECTED);
    if (!s_audio_buffer_enabled ||
        (current != APP_STATE_LISTENING && current != APP_STATE_PLAYING && !pre_roll)) {
        xSemaphoreGive(s_audio_buffer_lock);
        return;
    }
    size_t dropped = 0;
    bool stored = false;
    bool extended_capture = false;
    bool live_tail = s_first_phrase_closed;
    RingbufHandle_t ringbuf = live_tail ? s_live_ringbuf : s_ringbuf;
    uint32_t *read_end = live_tail ? &s_live_read_end : &s_audio_progress.read_end;
    if (s_pre_roll_enabled && !s_first_phrase_closed) {
        size_t queued_frames = audio_buffer_frames_locked(s_ringbuf);
        /* A slow activation must not cut a hole in the user's sentence.
         * Keep the same socket and let the existing cold first-phrase policy
         * protect a longer prefix if the normal live window is exceeded. */
        extended_capture = s_hot_pre_roll && !s_pre_roll_uploading &&
            queued_frames == (BSP_I2S_SAMPLE_RATE / 1000u * AUDIO_UPLOAD_FIFO_MS) / s_fetch_chunksize;
        if (queued_frames < WAKE_WORD_RB_ITEM_COUNT)
            stored = xRingbufferSend(s_ringbuf, res->data, frame_bytes, 0) == pdTRUE;
        if (!stored) {
            ++s_pre_roll_dropped_frames;
            uint32_t now = esp_log_timestamp();
            if ((uint32_t)(now - s_overflow_log_ms) >= 2000) {
                s_overflow_log_ms = now;
                ESP_LOGW(TAG, "Pre-roll FIFO full: discarded %lu new frames (%lu ms); first phrase retained",
                         (unsigned long)s_pre_roll_dropped_frames,
                         (unsigned long)(s_pre_roll_dropped_frames * s_fetch_chunksize /
                                         (BSP_I2S_SAMPLE_RATE / 1000u)));
            }
        }
    } else {
        const size_t max_frames = (BSP_I2S_SAMPLE_RATE / 1000u * AUDIO_UPLOAD_FIFO_MS) / s_fetch_chunksize;
        while (audio_buffer_frames_locked(ringbuf) >= max_frames &&
               discard_oldest_audio_locked(ringbuf, read_end))
            ++dropped;
        while (!(stored = xRingbufferSend(ringbuf, res->data, frame_bytes, 0) == pdTRUE)) {
            if (!discard_oldest_audio_locked(ringbuf, read_end)) break;
            ++dropped;
        }
        if (s_live_buffer_active) s_audio_progress.read_end = s_live_read_end;
    }
    bool valid_voice = res->vad_state == VAD_SPEECH &&
                       afe_frame_energy(res->data, s_fetch_chunksize) >= AUDIO_DIAGNOSTIC_ENERGY;
    if (stored) {
        s_audio_progress.written_end += s_fetch_chunksize;
        if (valid_voice) {
            s_audio_progress.speech_end = s_audio_progress.written_end;
            uint32_t voice_ms=(uint32_t)(esp_timer_get_time()/1000);
            s_audio_progress.last_voice_ms=voice_ms ? voice_ms : UINT32_MAX;
        }
    }
    bool first_phrase_closed_now = observe_first_phrase_locked(res->vad_state, valid_voice, lite_source);
    uint32_t phrase_end_snapshot = s_first_phrase_end;
    s_overflow_frames += dropped;
    uint32_t now = esp_log_timestamp(), report_dropped = 0;
    if (s_overflow_frames && (uint32_t)(now - s_overflow_log_ms) >= 2000) {
        s_overflow_log_ms = now;
        report_dropped = s_overflow_frames;
        s_overflow_frames = 0;
    }
    xSemaphoreGive(s_audio_buffer_lock);
    if (extended_capture)
        ESP_LOGI(TAG, "Activation exceeded %u ms of PCM; retaining first phrase on the same wake",
                 (unsigned)AUDIO_UPLOAD_FIFO_MS);
    if (first_phrase_closed_now)
        ESP_LOGI(TAG, "First phrase boundary captured; later audio uses live FIFO (end_sample=%lu)",
                 (unsigned long)phrase_end_snapshot);
    if (report_dropped)
        ESP_LOGW(TAG, "Microphone FIFO behind: discarded %lu old frames (%lu ms) since last report; keeping live audio",
                 (unsigned long)report_dropped,
                 (unsigned long)(report_dropped * s_fetch_chunksize / (BSP_I2S_SAMPLE_RATE / 1000u)));
}


/* ===========================================================================
 * feed_Task — I²S DMA → AFE
 * ========================================================================= */

static void feed_Task(void *arg)
{
    (void)arg;
    /* codec_dev 返回所选 TDM 槽组成的 MR，无需另建软件播放参考缓冲。 */
    size_t input_bytes = s_afe_chunksize * BSP_AFE_INPUT_CHANNELS * sizeof(int16_t);
    int16_t *afe_buff = malloc(input_bytes);
    if (afe_buff == NULL) {
        ESP_LOGE(TAG, "feed: audio buffer allocation failed");
        s_task_running = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "feed_Task started (chunksize=%d, input=%s hardware reference, core %d)",
             s_afe_chunksize, BSP_AFE_INPUT_FORMAT, xPortGetCoreID());
    int mic_peak = 0, ref_peak = 0;
    uint32_t mic_clipped = 0, ref_clipped = 0;
    int64_t stats_at = esp_timer_get_time();
    uint32_t feed_max_us = 0, feed_overruns = 0, feed_count = 0;

    while (s_task_running) {
        /* 从 I²S DMA 读取 PCM */
        int ret = audio_out_read_afe_input(afe_buff, input_bytes);
        if (ret == ESP_OK) {
            /* 五秒汇总峰值/削顶数，供首次实板核对 M/R 接线与增益；不逐帧打印。 */
            for (int i = 0; i < s_afe_chunksize; ++i) {
                int m = abs((int)afe_buff[2 * i]);
                int r = abs((int)afe_buff[2 * i + 1]);
                if (m > mic_peak) mic_peak = m;
                if (r > ref_peak) ref_peak = r;
                if (m >= 32760) ++mic_clipped;
                if (r >= 32760) ++ref_clipped;
            }
            int64_t start = esp_timer_get_time();
            s_afe_iface->feed(s_afe_data, afe_buff);
            uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
            if (elapsed > feed_max_us) feed_max_us = elapsed;
            if (elapsed > (uint32_t)s_afe_chunksize * 1000000u / 16000u) ++feed_overruns;
            ++feed_count;
            if (esp_timer_get_time() - stats_at >= 5000000) {
                ESP_LOGI(TAG, "AFE stats: frames=%lu feed_max_us=%lu over_budget=%lu budget_us=%u",
                         (unsigned long)feed_count, (unsigned long)feed_max_us,
                         (unsigned long)feed_overruns, (unsigned)s_afe_chunksize * 1000000u / 16000u);
                ESP_LOGI(TAG, "AFE input MR: mic_peak=%d ref_peak=%d mic_clip=%lu ref_clip=%lu",
                         mic_peak, ref_peak, (unsigned long)mic_clipped, (unsigned long)ref_clipped);
                mic_peak = ref_peak = 0;
                mic_clipped = ref_clipped = 0;
                stats_at = esp_timer_get_time();
                feed_count = feed_max_us = feed_overruns = 0;
            }
            /* A backlogged RX queue may never block. Yield one tick per 64 ms
             * input frame so CPU0 idle/system work is not starved indefinitely. */
            vTaskDelay(1);
        } else {
            if (esp_timer_get_time() - stats_at >= 5000000) {
                ESP_LOGW(TAG, "AFE input read failed: %d", ret);
                stats_at = esp_timer_get_time();
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    free(afe_buff);
    ESP_LOGI(TAG, "feed_Task exited");
    vTaskDelete(NULL);
}

/* ===========================================================================
 * detect_Task — AFE fetch → 唤醒词检测
 * ========================================================================= */

static void detect_Task(void *arg)
{
    (void)arg;

    /* fetch 输出缓冲区（AFE 可能输出多通道，回合单通道给 audio_in） */
    int16_t *fetch_buff = malloc(s_afe_chunksize * sizeof(int16_t));
    if (fetch_buff == NULL) {
        ESP_LOGE(TAG, "detect: malloc failed");
        s_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "detect_Task started (core %d)", xPortGetCoreID());
    ESP_LOGI(TAG, "Platform interruption enabled; local VAD never cancels playback");

    while (s_task_running) {
        /* 从 AFE 取回处理结果 */
        /* 模式可能在 fetch 返回与入队之间切换；保留实际产出这帧的 AFE，
         * 不把最后一帧 WAKE 的 VAD 误当成新 LITE 的人声。 */
        void *source_afe = s_afe_data;
        afe_fetch_result_t *res = s_afe_iface->fetch(source_afe);

        if (res == NULL || res->ret_value == ESP_FAIL) {
            ESP_LOGW(TAG, "AFE fetch failed or EOF");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ── 唤醒词检测 ── */
        bool wake_detected = false;
        /* B2: only run WakeNet during WAIT_WAKE (CPU saving during LISTENING/PLAYING) */
        /* In LISTENING/PLAYING conversation_active=true,so any hit is discarded by app_main. */
        /* feed_task / AEC / VAD keep running; interruption is decided by the platform. */
        if (app_state_get() == APP_STATE_WAIT_WAKE &&
            res->wakeup_state == WAKENET_DETECTED) {
            if (s_wake_state == WAKE_STATE_IDLE) {
                s_wake_state = WAKE_STATE_DETECTED;
                wake_detected = true;
                ESP_LOGI(TAG, ">>> WakeWord DETECTED! model=%d word=%d <<<",
                         res->wakenet_model_index, res->wake_word_index);
            }
            /* 检测到后不再重复触发，直到 reset() */
        }

        buffer_afe_audio(res, wake_detected, source_afe == s_afe_data_lite);
        if (wake_detected) {
            /* 采样边界与预录音已建立，app_main 处理延迟不会丢首句。 */
            if (s_wake_cb)
                s_wake_cb(res->wake_word_index, res->wakenet_model_index, s_wake_cb_arg);
            event_bus_publish(EV_AUDIO_WAKE_DETECTED, NULL, 0);
        }

    }

    free(fetch_buff);
    ESP_LOGI(TAG, "detect_Task exited");
    vTaskDelete(NULL);
}

/* ===========================================================================
 * 公共 API
 * ========================================================================= */

esp_err_t wake_word_init(void)
{
    if (s_task_running) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing WakeWord (esp-sr)...");

    /* ── 1. 板级初始化（I²S 总线、ES7210）────────────────── */
    esp_err_t err = audio_out_init(ES8311_I2C_ADDR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_board_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ── 2. 加载模型文件（从 flash partition "model"）─────── */
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init failed — no model partition? "
                      "run `idf.py menuconfig → ESP Speech Recognition → Select wake words`");
        return ESP_FAIL;
    }
    for (int i = 0; i < models->num; i++) {
        if (strstr(models->model_name[i], ESP_WN_PREFIX) != NULL) {
            ESP_LOGI(TAG, "WakeNet model found: %s", models->model_name[i]);
        }
    }

    /* ── 3. AFE 配置 ─────────────────────────────────────── */
    /* === 3. AFE configuration: create two configs (WAKE and LITE) ===
     * WAKE config keeps the auto-loaded WakeNet model.
     * LITE config sets wakenet_model_name = NULL to skip WakeNet allocation
     * (CPU savings in LISTENING/PLAYING). Both share the same interface. */
    afe_config_t *cfg_wake = afe_config_init(BSP_AFE_INPUT_FORMAT, models,
                                              AFE_TYPE_FD, AFE_MODE_LOW_COST);
    if (cfg_wake == NULL) {
        ESP_LOGE(TAG, "afe_config_init (WAKE) failed");
        return ESP_FAIL;
    }

    /* VoCat：MR 最后一路是 ES7210 硬件回采；保留 YMOS 的 AEC/VAD 判定参数。 */
    cfg_wake->aec_init = true;
    cfg_wake->aec_mode = AEC_MODE_FD_LOW_COST;
    cfg_wake->aec_filter_length = 4;
    cfg_wake->output_playback_channel = false;
    ESP_LOGI(TAG, "AFE configured for full-duplex AEC (MR hardware reference, filter=%d)",
             cfg_wake->aec_filter_length);

    /* 可选：打印/覆盖唤醒词模型名称 */
    if (cfg_wake->wakenet_model_name) {
        ESP_LOGI(TAG, "WakeNet model: %s", cfg_wake->wakenet_model_name);
    }
    if (cfg_wake->wakenet_model_name_2) {
        ESP_LOGI(TAG, "WakeNet model 2: %s", cfg_wake->wakenet_model_name_2);
    }

    /* Clone for LITE; disable WakeNet by clearing model names. */
    afe_config_t *cfg_lite = afe_config_init(BSP_AFE_INPUT_FORMAT, models,
                                              AFE_TYPE_FD, AFE_MODE_LOW_COST);
    if (cfg_lite == NULL) {
        ESP_LOGE(TAG, "afe_config_init (LITE) failed");
        afe_config_free(cfg_wake);
        return ESP_FAIL;
    }
    cfg_lite->aec_init = true;
    cfg_lite->aec_mode = AEC_MODE_FD_LOW_COST;
    cfg_lite->aec_filter_length = 4;
    cfg_lite->output_playback_channel = false;
    /* KEY: skip WakeNet allocation in LITE mode */
    cfg_lite->wakenet_model_name   = NULL;
    cfg_lite->wakenet_model_name_2 = NULL;

    /* === 4. Get interface and create both handles === */
    s_afe_iface = esp_afe_handle_from_config(cfg_wake);
    if (s_afe_iface == NULL) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(cfg_wake);
        afe_config_free(cfg_lite);
        return ESP_FAIL;
    }

    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM before AFE handles: free=%u KB",
             (unsigned)(psram_before / 1024));

    s_afe_data_wake = s_afe_iface->create_from_config(cfg_wake);
    if (s_afe_data_wake == NULL) {
        ESP_LOGE(TAG, "AFE WAKE handle create failed");
        afe_config_free(cfg_wake);
        afe_config_free(cfg_lite);
        return ESP_FAIL;
    }
    size_t psram_after_wake = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM after WAKE handle: free=%u KB (delta=%u KB)",
             (unsigned)(psram_after_wake / 1024),
             (unsigned)((psram_before - psram_after_wake) / 1024));

    s_afe_data_lite = s_afe_iface->create_from_config(cfg_lite);
    if (s_afe_data_lite == NULL) {
        ESP_LOGE(TAG, "AFE LITE handle create failed");
        s_afe_iface->destroy(s_afe_data_wake);
        s_afe_data_wake = NULL;
        afe_config_free(cfg_wake);
        afe_config_free(cfg_lite);
        return ESP_FAIL;
    }
    size_t psram_after_lite = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM after LITE handle: free=%u KB (delta=%u KB)",
             (unsigned)(psram_after_lite / 1024),
             (unsigned)((psram_after_wake - psram_after_lite) / 1024));

    afe_config_free(cfg_wake);
    afe_config_free(cfg_lite);

    /* 两套 AFE 共用采集任务，帧长/通道必须一致，不能用较大帧勉强喂入。 */
    int cs_wake = s_afe_iface->get_feed_chunksize(s_afe_data_wake);
    int cs_lite = s_afe_iface->get_feed_chunksize(s_afe_data_lite);
    if (cs_wake <= 0 || cs_wake != cs_lite ||
        s_afe_iface->get_feed_channel_num(s_afe_data_wake) != BSP_AFE_INPUT_CHANNELS ||
        s_afe_iface->get_feed_channel_num(s_afe_data_lite) != BSP_AFE_INPUT_CHANNELS) {
        ESP_LOGE(TAG, "AFE input geometry mismatch: wake=%d lite=%d; expected MR", cs_wake, cs_lite);
        s_afe_iface->destroy(s_afe_data_wake);
        s_afe_iface->destroy(s_afe_data_lite);
        s_afe_data_wake = s_afe_data_lite = NULL;
        return ESP_FAIL;
    }
    s_afe_chunksize = cs_wake;
    /* POC-1 fix: s_fetch_chunksize was missing - broke audio upload */
    s_fetch_chunksize  = s_afe_iface->get_fetch_chunksize(s_afe_data_wake);
    ESP_LOGI(TAG, "AFE chunksize=%d samples (%.1f ms) [wake=%d lite=%d fetch=%d]",
             s_afe_chunksize, s_afe_chunksize * 1000.0f / 16000.0f,
             cs_wake, cs_lite, s_fetch_chunksize);

    /* ── 6. 创建 ring buffer ─────────────────────────────── */
    s_ringbuf = xRingbufferCreateWithCaps(WAKE_WORD_RB_TOTAL_SIZE, RINGBUF_TYPE_NOSPLIT,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (WAKE_WORD_CONNECT_PRE_ROLL_ENABLED && WAKE_WORD_FIRST_PHRASE_HANDOFF_ENABLED)
        s_live_ringbuf = xRingbufferCreateWithCaps(WAKE_WORD_LIVE_RB_TOTAL_SIZE,
                            RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_audio_buffer_lock = xSemaphoreCreateMutex();
    if (s_ringbuf == NULL || s_audio_buffer_lock == NULL ||
        (WAKE_WORD_CONNECT_PRE_ROLL_ENABLED && WAKE_WORD_FIRST_PHRASE_HANDOFF_ENABLED &&
         s_live_ringbuf == NULL)) {
        ESP_LOGE(TAG, "xRingbufferCreate failed");
        if (s_ringbuf) vRingbufferDeleteWithCaps(s_ringbuf);
        if (s_live_ringbuf) vRingbufferDeleteWithCaps(s_live_ringbuf);
        if (s_audio_buffer_lock) vSemaphoreDelete(s_audio_buffer_lock);
        s_ringbuf = s_live_ringbuf = NULL;
        s_audio_buffer_lock = NULL;
        s_afe_iface->destroy(s_afe_data_lite);
        s_afe_iface->destroy(s_afe_data_wake);
        s_afe_data_lite = s_afe_data_wake = s_afe_data = NULL;
        s_afe_iface = NULL; /* start() must reinitialize after this allocation failure. */
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Ring buffer: %d items × %d samples = %d bytes",
             WAKE_WORD_RB_ITEM_COUNT, WAKE_WORD_RB_ITEM_SIZE,
             (int)WAKE_WORD_RB_TOTAL_SIZE);

    /* ── 7. 启动 feed_Task + detect_Task ────────────────── */
    /* Set active handle BEFORE starting tasks (CRITICAL: otherwise tasks see NULL handle) */
    s_afe_data = s_afe_data_wake;
    s_mode = WAKE_WORD_MODE_WAKE;

    s_task_running = true;
    BaseType_t t0 = xTaskCreatePinnedToCore(
        feed_Task, "ww_feed",
        WAKE_WORD_FEED_STACK, NULL, 5, &s_feed_task_h, 0);
    BaseType_t t1 = xTaskCreatePinnedToCore(
        detect_Task, "ww_detect",
        WAKE_WORD_DETECT_STACK, NULL, 5, &s_detect_task_h, 1);

    if (t0 != pdPASS || t1 != pdPASS) {
        ESP_LOGE(TAG, "task creation failed (feed=%d detect=%d)", t0, t1);
        s_task_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "WakeWord initialized — feed (core0) + detect (core1) running");
    return ESP_OK;
}

void wake_word_deinit(void)
{
    if (!s_task_running) return;

    ESP_LOGI(TAG, "Stopping WakeWord...");
    s_task_running = false;

    /* 等待任务退出 */
    if (s_feed_task_h) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        s_feed_task_h = NULL;
    }
    if (s_detect_task_h) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        s_detect_task_h = NULL;
    }

    /* 释放 AFE */
    /* Release AFE handles (destroy LITE first, then WAKE) */
    if (s_afe_iface) {
        if (s_afe_data_lite) {
            s_afe_iface->destroy(s_afe_data_lite);
            s_afe_data_lite = NULL;
        }
        if (s_afe_data_wake) {
            s_afe_iface->destroy(s_afe_data_wake);
            s_afe_data_wake = NULL;
        }
        s_afe_data  = NULL;
        s_afe_iface = NULL;
    }

    /* 释放 ring buffer */
    if (s_ringbuf) {
        vRingbufferDeleteWithCaps(s_ringbuf);
        s_ringbuf = NULL;
    }
    if (s_live_ringbuf) {
        vRingbufferDeleteWithCaps(s_live_ringbuf);
        s_live_ringbuf = NULL;
    }
    if (s_audio_buffer_lock) {
        vSemaphoreDelete(s_audio_buffer_lock);
        s_audio_buffer_lock = NULL;
    }

    s_wake_state = WAKE_STATE_IDLE;
    ESP_LOGI(TAG, "WakeWord deinitialized");
}

RingbufHandle_t wake_word_get_ringbuf(void)
{
    return s_ringbuf;
}

int wake_word_get_chunksize(void)
{
    return s_fetch_chunksize > 0 ? s_fetch_chunksize : 512;
}

wake_word_state_t wake_word_get_state(void)
{
    return s_wake_state;
}

void wake_word_set_callback(wake_word_detected_cb_t cb, void *user_data)
{
    s_wake_cb     = cb;
    s_wake_cb_arg = user_data;
}

void wake_word_reset(void)
{
    if (s_wake_state != WAKE_STATE_IDLE) {
        ESP_LOGI(TAG, "wake_word_reset: DETECTED → IDLE");
        s_wake_state = WAKE_STATE_IDLE;
    }
}

void wake_word_clear_audio_buffer(void)
{
    if (!s_ringbuf || !s_audio_buffer_lock) return;
    xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
    clear_audio_buffer_locked();
    xSemaphoreGive(s_audio_buffer_lock);
}

void wake_word_set_audio_buffer_enabled(bool enabled)
{
    if (!s_ringbuf || !s_audio_buffer_lock) return;
    xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
    if (s_audio_buffer_enabled != enabled) clear_audio_buffer_locked();
    s_audio_buffer_enabled = enabled;
    xSemaphoreGive(s_audio_buffer_lock);
}

void wake_word_set_pre_roll(bool enabled)
{
    if (enabled || !s_ringbuf || !s_audio_buffer_lock) return;
    xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
    clear_audio_buffer_locked();
    xSemaphoreGive(s_audio_buffer_lock);
}

void wake_word_select_hot_pre_roll(void)
{
    if (!WAKE_WORD_CONNECT_PRE_ROLL_ENABLED || !s_audio_buffer_lock) return;
    xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
    if (s_pre_roll_enabled && !s_pre_roll_uploading && !s_first_phrase_closed)
        s_hot_pre_roll = true;
    xSemaphoreGive(s_audio_buffer_lock);
}

void wake_word_fallback_cold_pre_roll(void)
{
    if (!WAKE_WORD_CONNECT_PRE_ROLL_ENABLED || !s_audio_buffer_lock) return;
    xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
    if (!s_pre_roll_uploading) s_hot_pre_roll = false;
    xSemaphoreGive(s_audio_buffer_lock);
}

bool wake_word_prepare_pre_roll_upload(void)
{
    if (!WAKE_WORD_CONNECT_PRE_ROLL_ENABLED || !s_ringbuf || !s_audio_buffer_lock)
        return false;
    xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
    bool prepared = s_pre_roll_enabled;
    if (prepared && !s_pre_roll_uploading) {
        /* Freeze only the samples up to the activation ACK. The same reader
         * drains this prefix, then the ordinary live FIFO, in sample order. */
        if (s_hot_pre_roll && !s_first_phrase_closed &&
            WAKE_WORD_FIRST_PHRASE_HANDOFF_ENABLED && s_live_ringbuf) {
            s_first_phrase_closed = true;
            s_first_phrase_end = s_audio_progress.written_end;
            s_live_read_end = s_first_phrase_end;
        }
        s_pre_roll_uploading = true;
        ESP_LOGI(TAG, "%s upload ready: boundary_sample=%lu buffered_ms=%lu dropped_new=%lu",
                 s_hot_pre_roll ? "Hot pre-roll" : "Pre-roll",
                 (unsigned long)s_wake_boundary_sample,
                 (unsigned long)((s_audio_progress.written_end - s_audio_progress.read_end) /
                                 (BSP_I2S_SAMPLE_RATE / 1000u)),
                 (unsigned long)s_pre_roll_dropped_frames);
    }
    xSemaphoreGive(s_audio_buffer_lock);
    return prepared;
}

esp_err_t wake_word_start(void)
{
    if (s_task_running) {
        ESP_LOGD(TAG, "wake_word already running");
        return ESP_OK;
    }
    /* 尚未初始化 → 完整初始化（首次调用） */
    if (s_afe_iface == NULL) {
        return wake_word_init();
    }
    /* 已初始化但任务停止 → 重启任务 */
    s_task_running = true;
    BaseType_t t0 = xTaskCreatePinnedToCore(
        feed_Task, "ww_feed",
        WAKE_WORD_FEED_STACK, NULL, 5, &s_feed_task_h, 0);
    BaseType_t t1 = xTaskCreatePinnedToCore(
        detect_Task, "ww_detect",
        WAKE_WORD_DETECT_STACK, NULL, 5, &s_detect_task_h, 1);
    if (t0 != pdPASS || t1 != pdPASS) {
        ESP_LOGE(TAG, "task restart failed (feed=%d detect=%d)", t0, t1);
        s_task_running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "WakeWord tasks restarted (AFE preserved)");
    return ESP_OK;
}

void wake_word_stop(void)
{
    if (!s_task_running) {
        ESP_LOGD(TAG, "wake_word already stopped");
        return;
    }
    ESP_LOGI(TAG, "Stopping WakeWord tasks (preserving AFE)...");
    s_task_running = false;

    /* 等待任务退出 */
    if (s_feed_task_h) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        s_feed_task_h = NULL;
    }
    if (s_detect_task_h) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        s_detect_task_h = NULL;
    }

    s_wake_state = WAKE_STATE_IDLE;
    ESP_LOGI(TAG, "WakeWord tasks stopped (AFE handle preserved for restart)");
}

uint32_t wake_word_get_feed_stack_hiwat(void)
{
    return s_feed_stack_hiwat;
}

uint32_t wake_word_get_detect_stack_hiwat(void)
{
    return s_detect_stack_hiwat;
}

esp_err_t wake_word_set_mode(wake_word_mode_t mode)
{
    void *target = (mode == WAKE_WORD_MODE_WAKE) ? s_afe_data_wake : s_afe_data_lite;
    if (target == NULL) {
        ESP_LOGE(TAG, "set_mode: AFE handles not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_afe_data == target) {
        /* Already in target mode - fast path, no suspend needed */
        return ESP_OK;
    }
    if (s_feed_task_h == NULL || s_detect_task_h == NULL) {
        ESP_LOGE(TAG, "set_mode: tasks not running");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "AFE mode switch: %s -> %s",
             s_mode == WAKE_WORD_MODE_WAKE ? "WAKE" : "LITE",
             mode == WAKE_WORD_MODE_WAKE ? "WAKE" : "LITE");

    /* 先取缓存锁再挂起检测任务，避免把持锁的任务挂起后永远等不到锁。 */
    if (s_audio_buffer_lock) xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
    vTaskSuspend(s_feed_task_h);
    vTaskSuspend(s_detect_task_h);

    s_afe_data = target;
    s_mode = mode;
    if (mode == WAKE_WORD_MODE_LITE) reset_first_phrase_detector_locked();

    /* 普通切换清缓存；唤醒预录音跨 WAKE -> LITE 保留相同16kHz单声道PCM。 */
    if (!s_pre_roll_enabled) clear_audio_buffer_locked();

    /* 保留本次唤醒锁存直到回待机，避免切 LITE 的短窗口重复建立首句边界。 */
    if (mode == WAKE_WORD_MODE_WAKE) s_wake_state = WAKE_STATE_IDLE;

    if (s_audio_buffer_lock) xSemaphoreGive(s_audio_buffer_lock);
    vTaskResume(s_detect_task_h);
    vTaskResume(s_feed_task_h);

    return ESP_OK;
}

wake_word_mode_t wake_word_get_mode(void)
{
    return s_mode;
}
