/**
 * @file audio_in.c
 * @brief 音频采集服务 (P0 收音链路核心)
 *
 * 架构说明（唤醒词模式）：
 *  wake_word.c 的 feed_Task 持续读取 I²S DMA，detect_Task 将 AFE 输出写入 ring buffer。
 *  audio_in_start_from_ringbuf() 启动后从该 ring buffer 读取 PCM，
 *  自身不再初始化 I²S。
 *
 *  链路: (AFE) → ring buffer → Opus 编码 → WS JSON；VAD 仅记录活动状态。
 *
 *  采样: 16 kHz, 16-bit, mono
 *  VAD:  能量阈值 + 静音超时（由 wake_word 通过 AFE 提供统一 PCM 流）
 */

#include "audio_in.h"
#include "board_config.h"
#include "wake_word.h"
#include "ws_service.h"
#include "wifi_service.h"
#include "opus_codec.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <stdatomic.h>

static const char *TAG = "audio_in";
/* 编码 core 0/prio 4 低于 AFE feed(5)；发送 core 1/prio 4 低于解码(5)
 * 和 PCM(6)。等待 WS 时编码可继续，短队列满则反压，不丢首句。 */
#define AUDIO_UPLOAD_CORE 0
#define AUDIO_UPLOAD_PRIORITY 4
#define AUDIO_TX_CORE 1
#define AUDIO_TX_PRIORITY 4
#define AUDIO_TX_QUEUE_PACKETS 4u /* 含正在发送的包，最多 80 ms */
#define AUDIO_UPLOAD_WAIT_MS 10u
#define AUDIO_TX_QUEUE_TIMEOUT_US 2000000LL

typedef struct {
    uint8_t data[WS_OPUS_FRAME_MAX_BYTES];
    size_t length;
    uint32_t sample_end;
    uint32_t connection_generation;
} audio_tx_packet_t;

/** 全局句柄（供 app_state.c 启动时使用） */
static audio_in_handle_t s_audio_handle = NULL;

/* ===========================================================================
 * 内部结构
 * ========================================================================= */

struct audio_in_impl {
    /* 配置 */
    audio_in_config_t        cfg;
    int                      frame_samples;  /* 每帧 sample 数 */

    /* AFE ring buffer（由 wake_word.c 提供） */
    RingbufHandle_t          ringbuf;

    /* VAD 状态机 */
    vad_state_t              vad_state;
    int16_t                 *vad_buf;       /* 仅注册整句回调时分配；实时上传不需要 */
    size_t                   vad_buf_cap;
    size_t                   vad_buf_len;
    int64_t                  speech_start_ms;
    int64_t                  last_speech_ms;

    /* 能量阈值滑动窗口 */
    int64_t                  energy_sum;    /* 8 帧均方值相加可能超过 int32_t */
    int32_t                  energy_count;
    int32_t                  energy_avg;

    /* Opus 编码器 */
    void                    *opus_enc;
    int16_t                  opus_pcm[320];
    size_t                   opus_samples;

    /* 连续上传由 AFE 的真实采样时钟驱动；每 320 个样本组成 20 ms Opus。 */
    int64_t                  stats_at;
    uint32_t                 stats_packets, encode_max_us, send_max_us;
    uint64_t                 encode_work_us, send_work_us; /* 含调度等待的累计墙钟时间 */
    uint32_t                 sent_end; /* h->lock 保护，成功发送后才推进 */

    /* 单生产者/单消费者固定槽；发送返回前 head 不出队，锁外借用仍有效。
     * PSRAM 在启动分配，仅两任务都退出后释放。head/count 由 lock 保护。 */
    audio_tx_packet_t       *tx_packets;
    unsigned                tx_head, tx_count;

    /* 任务 */
    TaskHandle_t             task_h;
    TaskHandle_t             tx_task_h;
    SemaphoreHandle_t        done_sem;
    SemaphoreHandle_t        tx_done_sem;
    SemaphoreHandle_t        paused_sem;
    atomic_bool              running;
    atomic_bool              paused;
    atomic_bool              tasks_ready;
    atomic_uint              pause_epoch;
    atomic_uint              pause_ack_epoch;
    atomic_uint              tx_pause_ack_epoch;
    uint32_t                 connection_generation;

    /* 回调 */
    audio_in_pcm_cb_t        pcm_cb;
    void                    *pcm_cb_arg;
    audio_in_utterance_cb_t  utt_cb;
    void                    *utt_cb_arg;

    /* 互斥 */
    SemaphoreHandle_t        lock;
};

/* ===========================================================================
 * 工具函数
 * ========================================================================= */

static int32_t calc_frame_energy(const int16_t *pcm, int samples)
{
    int64_t sum = 0;
    for (int i = 0; i < samples; i++) {
        int32_t s = pcm[i];
        sum += (int64_t)s * s;
    }
    return (int32_t)(sum / samples);  /* 均方值 */
}

static void vad_reset(audio_in_handle_t h)
{
    h->vad_buf_len     = 0;
    h->vad_state       = VAD_STATE_IDLE;
    h->speech_start_ms = 0;
    h->last_speech_ms  = 0;
    h->energy_sum      = 0;
    h->energy_count    = 0;
    h->energy_avg      = 0;
}

/* ===========================================================================
 * VAD 状态机
 * ========================================================================= */

static void process_vad_frame(audio_in_handle_t h, const int16_t *pcm,
                              size_t pcm_samples, uint32_t now_ms)
{
    int32_t energy = calc_frame_energy(pcm, pcm_samples);
    h->energy_sum += energy;
    h->energy_count++;

    /* 滑动平均 */
    if (h->energy_count >= 8) {
        h->energy_avg = h->energy_sum / h->energy_count;
        h->energy_sum  = 0;
        h->energy_count = 0;
    }

    bool is_speech = (h->energy_avg > h->cfg.vad_threshold);

    switch (h->vad_state) {
        case VAD_STATE_IDLE:
            if (is_speech) {
                h->vad_state       = VAD_STATE_SPEECH;
                h->speech_start_ms = now_ms;
                h->last_speech_ms  = now_ms;
                h->vad_buf_len     = 0;
                ESP_LOGD(TAG, "VAD: SPEECH start (energy=%ld)", (long)energy);

                /* 事件 */
                event_bus_publish(EV_AUDIO_SPEECH_START, NULL, 0);
            }
            break;

        case VAD_STATE_SPEECH:
            if (is_speech) {
                h->last_speech_ms = now_ms;
            }

            /* 累积 PCM */
            if (h->vad_buf != NULL && h->vad_buf_len + pcm_samples * 2 <= h->vad_buf_cap) {
                memcpy(h->vad_buf + h->vad_buf_len / 2,
                       pcm, pcm_samples * 2);
                h->vad_buf_len += pcm_samples * 2;
            }

            /* 静默超时 */
            if (!is_speech && (now_ms - h->last_speech_ms) >= h->cfg.silence_timeout_ms) {
                h->vad_state = VAD_STATE_SILENCE;
                ESP_LOGD(TAG, "VAD: silence timeout, len=%zu", h->vad_buf_len);
            }
            break;

        case VAD_STATE_SILENCE:
            /* 累积尾部 PCM */
            if (h->vad_buf != NULL && h->vad_buf_len + pcm_samples * 2 <= h->vad_buf_cap) {
                memcpy(h->vad_buf + h->vad_buf_len / 2, pcm, pcm_samples * 2);
                h->vad_buf_len += pcm_samples * 2;
            }
            break;
    }

    /* SPEECH/SILENCE → IDLE（发送句子） */
    if (h->vad_state == VAD_STATE_SILENCE) {
        int64_t speech_dur_ms = h->last_speech_ms - h->speech_start_ms;

        if (speech_dur_ms >= h->cfg.min_utterance_ms) {
            ESP_LOGD(TAG, "VAD: utterance done, dur=%lld ms", speech_dur_ms);

            /* 回调只借用缓存直到返回，无须再次复制整句。默认未注册回调，
             * 仍保留活动事件供空闲计时使用，不积累/复制任何整句 PCM。 */
            if (h->utt_cb && h->vad_buf_len)
                h->utt_cb(h->utt_cb_arg, h->vad_buf, h->vad_buf_len);

            event_bus_publish(EV_AUDIO_SPEECH_END, NULL, 0);
        }

        vad_reset(h);
    }
}

/* ===========================================================================
 * Opus 编码（每帧实时编码 + WS 发送）
 * ========================================================================= */

static void fail_audio_upload(audio_in_handle_t h, esp_err_t err)
{
    /* 编码与 TX 可同时失败，一次运行最多发布一次恢复事件。 */
    if (!atomic_exchange(&h->running, false)) return;
    ESP_LOGW(TAG, "Audio upload failed: %s; requesting recovery", esp_err_to_name(err));
    event_bus_publish(EV_AUDIO_UPLOAD_FAILED, (void *)(uintptr_t)h->connection_generation, 0);
}

/* 每个任务在自己的安全边界确认同一暂停代次；不持锁等待。 */
static bool wait_upload_enabled(audio_in_handle_t h, bool tx)
{
    while (h->running && (!atomic_load(&h->tasks_ready) || atomic_load(&h->paused))) {
        if (atomic_load(&h->paused)) {
            atomic_store(tx ? &h->tx_pause_ack_epoch : &h->pause_ack_epoch,
                         atomic_load(&h->pause_epoch));
            xSemaphoreGive(h->paused_sem);
        }
        vTaskDelay(pdMS_TO_TICKS(AUDIO_UPLOAD_WAIT_MS));
    }
    return h->running;
}

/* 只返回单个生产者可写的空槽，消费者不会访问尚未发布的 tail。
 * 队列满最多等待 2 秒活跃时间；暂停不计入拥塞，10 ms 检查停止/暂停。 */
static audio_tx_packet_t *wait_tx_slot(audio_in_handle_t h)
{
    int64_t waited_us = 0;
    while (wait_upload_enabled(h, false)) {
        xSemaphoreTake(h->lock, portMAX_DELAY);
        audio_tx_packet_t *slot = h->tx_count < AUDIO_TX_QUEUE_PACKETS ?
            &h->tx_packets[(h->tx_head + h->tx_count) % AUDIO_TX_QUEUE_PACKETS] : NULL;
        xSemaphoreGive(h->lock);
        if (slot) return slot;
        if (waited_us >= AUDIO_TX_QUEUE_TIMEOUT_US) {
            fail_audio_upload(h, ESP_ERR_TIMEOUT);
            return NULL;
        }
        int64_t at = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(AUDIO_UPLOAD_WAIT_MS));
        waited_us += esp_timer_get_time() - at;
    }
    return NULL;
}

static void encode_and_queue(audio_in_handle_t h, const int16_t *pcm, size_t pcm_samples,
                             uint32_t sample_start)
{
    if (h->opus_enc == NULL) {
        opus_enc_config_t cfg = {
            .sample_rate = OPUS_SAMPLE_RATE,
            .channels    = OPUS_CHANNELS,
            .complexity  = OPUS_COMPLEXITY,
            .bitrate     = ACOS_OPUS_BITRATE,
            .frame_ms    = 20,
        };
        h->opus_enc = opus_enc_create(&cfg);
        if (h->opus_enc == NULL) {
            fail_audio_upload(h, ESP_ERR_NO_MEM);
            return;
        }
    }

    /* 32 ms AFE 帧跨越 20 ms Opus 边界时保留半包，不补静音/重复采样。 */
    size_t consumed = 0;
    while (consumed < pcm_samples && h->running) {
        if (!wait_upload_enabled(h, false)) break;
        size_t count = 320 - h->opus_samples;
        if (count > pcm_samples - consumed) count = pcm_samples - consumed;
        memcpy(h->opus_pcm + h->opus_samples, pcm + consumed, count * sizeof(int16_t));
        h->opus_samples += count;
        consumed += count;
        if (h->opus_samples == 320) {
            audio_tx_packet_t *packet = wait_tx_slot(h);
            if (!packet) break;
            int64_t encode_at = esp_timer_get_time();
            int opus_len = opus_enc_encode(h->opus_enc, h->opus_pcm, sizeof(h->opus_pcm),
                                          packet->data, sizeof(packet->data));
            uint32_t encode_us = (uint32_t)(esp_timer_get_time() - encode_at);
            xSemaphoreTake(h->lock, portMAX_DELAY);
            h->encode_work_us += encode_us;
            if (encode_us > h->encode_max_us) h->encode_max_us = encode_us;
            xSemaphoreGive(h->lock);
            if (opus_len <= 0 || (size_t)opus_len > sizeof(packet->data)) {
                ESP_LOGW(TAG, "Opus encode failed: %d", opus_len);
                fail_audio_upload(h, ESP_FAIL);
                break;
            }
            packet->length = (size_t)opus_len;
            packet->sample_end = sample_start + consumed;
            packet->connection_generation = h->connection_generation;
            /* 发布只持短锁；暂停发生在编码中时，完整包保留在队列中。 */
            xSemaphoreTake(h->lock, portMAX_DELAY);
            ++h->tx_count;
            xSemaphoreGive(h->lock);
            h->opus_samples = 0;
        }
    }
}

/* 发送一个已发布的包，唯一调用者为 TX task；返回 false 表示本次未发送成功。 */
static bool transmit_next_packet(audio_in_handle_t h)
{
    if (!wait_upload_enabled(h, true)) return false;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    audio_tx_packet_t *packet = h->tx_count ? &h->tx_packets[h->tx_head] : NULL;
    xSemaphoreGive(h->lock);
    if (!packet) return false;
    if (packet->connection_generation != ws_service_generation()) {
        fail_audio_upload(h, ESP_ERR_INVALID_STATE);
        return false;
    }
    /* 连接生命周期必须先等两任务退出再重连；不在 h->lock 内等待网络。 */
    int64_t sent_at = esp_timer_get_time();
    esp_err_t err = ws_service_send_opus_frame(packet->data, packet->length);
    uint32_t send_us = (uint32_t)(esp_timer_get_time() - sent_at);
    uint32_t sample_end = packet->sample_end;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->send_work_us += send_us;
    if (send_us > h->send_max_us) h->send_max_us = send_us;
    if (err == ESP_OK) {
        ++h->stats_packets;
        h->sent_end = sample_end;
        h->tx_head = (h->tx_head + 1) % AUDIO_TX_QUEUE_PACKETS;
        --h->tx_count;
    }
    xSemaphoreGive(h->lock);
    if (err == ESP_OK) wake_word_confirm_audio_sent(sample_end);
    else fail_audio_upload(h, err);
    return err == ESP_OK;
}

/* 每 5 秒输出一次；所有共享计数在同一短锁中拍快照并清零。
 * 耗时含调度等待；tx_queue_ms 含正在发送的包，pending_ms 含 FIFO/半包/队列。 */
static void log_upload_stats(audio_in_handle_t h)
{
    int64_t now = esp_timer_get_time();
    /* stats_at 仅编码任务写，启动时尚无任务读取。 */
    if (now - h->stats_at < 5000000) return;
    wake_word_audio_progress_t progress;
    wake_word_get_audio_progress(&progress);
    xSemaphoreTake(h->lock, portMAX_DELAY);
    int64_t window_us = now - h->stats_at;
    uint32_t packets = h->stats_packets, encode_max = h->encode_max_us, send_max = h->send_max_us;
    uint64_t encode_work = h->encode_work_us, send_work = h->send_work_us;
    uint32_t tx_queue_ms = h->tx_count * 20;
    int32_t pending = (int32_t)(progress.written_end - h->sent_end);
    h->stats_at = now;
    h->stats_packets = h->encode_max_us = h->send_max_us = 0;
    h->encode_work_us = h->send_work_us = 0;
    xSemaphoreGive(h->lock);
    ESP_LOGI(TAG, "Upload stats: window_ms=%lu packets=%lu audio_ms=%lu fifo_ms=%lu "
             "tx_queue_ms=%lu pending_ms=%lu encode_max_us=%lu send_max_us=%lu "
             "encode_work_ms=%lu send_work_ms=%lu",
             (unsigned long)(window_us / 1000), (unsigned long)packets,
             (unsigned long)(packets * 20),
             (unsigned long)((uint32_t)(progress.written_end - progress.read_end) / 16),
             (unsigned long)tx_queue_ms, (unsigned long)(pending > 0 ? pending / 16 : 0),
             (unsigned long)encode_max, (unsigned long)send_max,
             (unsigned long)(encode_work / 1000), (unsigned long)(send_work / 1000));
}

static void audio_tx_task(void *param)
{
    audio_in_handle_t h = param;
    while (h->running) {
        bool sent = transmit_next_packet(h);
        if (h->running)
            vTaskDelay(sent ? 1 : pdMS_TO_TICKS(AUDIO_UPLOAD_WAIT_MS));
    }
    xSemaphoreGive(h->tx_done_sem);
    vTaskDelete(NULL);
}

/* ===========================================================================
 * ring buffer 读取任务
 * ========================================================================= */

static void audio_in_task(void *param)
{
    audio_in_handle_t h = (audio_in_handle_t)param;
    const size_t item_size = wake_word_get_chunksize() * sizeof(int16_t);
    int16_t *frame_buf = malloc(item_size);

    if (frame_buf == NULL) {
        ESP_LOGE(TAG, "frame_buf malloc failed");
        fail_audio_upload(h, ESP_ERR_NO_MEM);
        xSemaphoreGive(h->done_sem);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "audio_in task started (ringbuf, chunksize=%d, core %d)",
             wake_word_get_chunksize(), xPortGetCoreID());

    while (h->running) {
        if (!wait_upload_enabled(h, false)) break;
        uint32_t sample_end = 0;
        /* 与生产者共用短缓存锁，复制/归还和采样位置一起推进；网络发送在锁外。 */
        size_t item_size_out = wake_word_read_audio(frame_buf, item_size, &sample_end);
        if (!item_size_out) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (item_size_out > item_size) {
            fail_audio_upload(h, ESP_ERR_INVALID_SIZE);
            break;
        }

        size_t samples_read = item_size_out / sizeof(int16_t);
        uint32_t ts = (uint32_t)(esp_log_timestamp());

        /* PCM 回调 */
        if (h->pcm_cb) {
            h->pcm_cb(h->pcm_cb_arg, frame_buf, item_size_out, ts);
        }

        /* 编码后交给固定短队列；TX 在另一个核心等待网络。 */
        encode_and_queue(h, frame_buf, samples_read, sample_end - samples_read);

        /* VAD */
        if (h->running) process_vad_frame(h, frame_buf, samples_read, ts);
        log_upload_stats(h);
        /* 积压追赶期间也让低优先级任务获得运行机会；每个 AFE 帧只让出一拍，
         * 不对每包 Opus 另加 20 ms 等待。空队列仍由上面的短等待节流。 */
        if (h->running) vTaskDelay(1);
    }

    free(frame_buf);
    ESP_LOGI(TAG, "audio_in task exited");
    /* 通知 audio_in_stop() 任务已退出；不能使用 xTaskNotifyGive(NULL)，
     * 因为该 IDF 配置下 NULL 不是有效的通知目标。 */
    xSemaphoreGive(h->done_sem);
    vTaskDelete(NULL);
}

/* ===========================================================================
 * 公共 API
 * ========================================================================= */

audio_in_config_t audio_in_default_config(void)
{
    return (audio_in_config_t){
        .sample_rate         = BSP_I2S_SAMPLE_RATE,
        .frame_ms            = 32,             /* 与 AFE 帧对齐 */
        .mic_gain_db         = 30,
        .vad_threshold       = 500,
        .silence_timeout_ms  = VAD_SILENCE_TIMEOUT_MS,
        .min_utterance_ms    = VAD_MIN_UTTERANCE_MS,
        .bus                 = NULL,
    };
}

audio_in_handle_t audio_in_create(const audio_in_config_t *cfg)
{
    audio_in_handle_t h = calloc(1, sizeof(struct audio_in_impl));
    if (h == NULL) return NULL;

    h->cfg = cfg ? *cfg : audio_in_default_config();
    h->frame_samples = h->cfg.sample_rate * h->cfg.frame_ms / 1000;

    /* 默认只有实时上传和活动检测，不申请 20 秒（640 KB）的整句缓存。 */

    h->lock = xSemaphoreCreateMutex();
    if (h->lock == NULL) {
        free(h->vad_buf);
        free(h);
        return NULL;
    }

    h->done_sem = xSemaphoreCreateBinary();
    h->tx_done_sem = xSemaphoreCreateBinary();
    h->paused_sem = xSemaphoreCreateBinary();
    if (h->done_sem == NULL || h->tx_done_sem == NULL || h->paused_sem == NULL) {
        if (h->done_sem) vSemaphoreDelete(h->done_sem);
        if (h->tx_done_sem) vSemaphoreDelete(h->tx_done_sem);
        if (h->paused_sem) vSemaphoreDelete(h->paused_sem);
        vSemaphoreDelete(h->lock);
        free(h->vad_buf);
        free(h);
        return NULL;
    }

    ESP_LOGI(TAG, "audio_in created (no I²S — uses AFE ring buffer), "
             "frame_samples=%d, vad_threshold=%d",
             h->frame_samples, h->cfg.vad_threshold);
    s_audio_handle = h;
    return h;
}

void audio_in_destroy(audio_in_handle_t h)
{
    if (h == NULL) return;
    if (audio_in_stop(h) != ESP_OK) return;

    if (h->opus_enc) {
        opus_enc_destroy(h->opus_enc);
        h->opus_enc = NULL;
    }

    free(h->vad_buf);
    vSemaphoreDelete(h->done_sem);
    vSemaphoreDelete(h->tx_done_sem);
    vSemaphoreDelete(h->paused_sem);
    vSemaphoreDelete(h->lock);
    if (s_audio_handle == h) s_audio_handle = NULL;
    free(h);
}

audio_in_handle_t audio_in_get_handle(void)
{
    return s_audio_handle;
}

esp_err_t audio_in_start_from_ringbuf(audio_in_handle_t h, RingbufHandle_t ringbuf)
{
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    if (h->running) {
        atomic_store(&h->paused, false);
        return ESP_OK;
    }
    if (ringbuf == NULL) return ESP_ERR_INVALID_ARG;
    if (h->task_h || h->tx_task_h) {
        esp_err_t err = audio_in_stop(h);
        if (err != ESP_OK) return err;
    }

    h->tx_packets = heap_caps_malloc(sizeof(*h->tx_packets) * AUDIO_TX_QUEUE_PACKETS,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!h->tx_packets) return ESP_ERR_NO_MEM;
    h->tx_head = h->tx_count = 0;
    h->ringbuf = ringbuf;
    h->running = true;
    atomic_store(&h->tasks_ready, false);
    h->connection_generation = ws_service_generation();
    h->opus_samples = 0;
    atomic_store(&h->paused, false);
    wake_word_audio_progress_t progress;
    wake_word_get_audio_progress(&progress);
    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->sent_end = progress.read_end;
    h->stats_at = esp_timer_get_time();
    h->stats_packets = h->encode_max_us = h->send_max_us = 0;
    h->encode_work_us = h->send_work_us = 0;
    xSemaphoreGive(h->lock);
    /* 清除上一次任务的完成信号，再创建新任务。 */
    xSemaphoreTake(h->done_sem, 0);
    xSemaphoreTake(h->tx_done_sem, 0);

    /* VAD 状态复位 */
    vad_reset(h);
    /* 20 ms 连续上行不等待 Wi-Fi modem sleep。由 Wi-Fi 服务保管模式，
     * 失败只告警；结束/创建任务失败后恢复省电，监听与播放切换不反复切换。 */
    (void)wifi_service_set_audio_active(true);

    /* 首个任务等 ready，第二个创建失败时不会取 PCM/发包；先回收再释放槽。 */
    BaseType_t ret = xTaskCreatePinnedToCore(
        audio_tx_task, "audio_tx", TASK_STACK_AUDIO_IN,
        h, AUDIO_TX_PRIORITY, &h->tx_task_h, AUDIO_TX_CORE);
    if (ret == pdPASS) {
        ret = xTaskCreatePinnedToCore(
            audio_in_task, "audio_in", TASK_STACK_AUDIO_IN,
            h, AUDIO_UPLOAD_PRIORITY, &h->task_h, AUDIO_UPLOAD_CORE);
    }
    if (ret != pdPASS) {
        h->running = false;
        esp_err_t stopped = audio_in_stop(h);
        return stopped == ESP_OK ? ESP_ERR_NO_MEM : stopped;
    }
    atomic_store(&h->tasks_ready, true);
    ESP_LOGI(TAG, "Upload pipeline started: encode core=%d TX core=%d queue=%u ms",
             AUDIO_UPLOAD_CORE, AUDIO_TX_CORE, (unsigned)(AUDIO_TX_QUEUE_PACKETS * 20));
    return ESP_OK;
}

esp_err_t audio_in_pause(audio_in_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    if (!h->running) return ESP_OK;
    xSemaphoreTake(h->paused_sem, 0);
    unsigned epoch = atomic_fetch_add(&h->pause_epoch, 1) + 1;
    atomic_store(&h->paused, true);
    int64_t deadline = esp_timer_get_time() + 2000000;
    while (h->running && (atomic_load(&h->pause_ack_epoch) != epoch ||
                          atomic_load(&h->tx_pause_ack_epoch) != epoch)) {
        int64_t left_ms = (deadline - esp_timer_get_time()) / 1000;
        if (left_ms <= 0) return ESP_ERR_TIMEOUT;
        xSemaphoreTake(h->paused_sem, pdMS_TO_TICKS((uint32_t)left_ms));
    }
    return ESP_OK;
}

uint32_t audio_in_get_last_voice_ms(audio_in_handle_t h)
{
    if (!h || !h->running) return 0;
    wake_word_audio_progress_t progress;
    wake_word_get_audio_progress(&progress);
    return progress.last_voice_ms;
}

void audio_in_get_upload_status(audio_in_handle_t h, audio_in_upload_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    if (!h) return;
    wake_word_audio_progress_t progress;
    wake_word_get_audio_progress(&progress);
    status->speech_end = progress.speech_end;
    status->running = h->running;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    status->sent_end = h->sent_end;
    status->tx_queue_ms = h->tx_count * 20;
    int32_t pending = (int32_t)(progress.written_end - h->sent_end);
    status->pending_ms = pending > 0 ? (uint32_t)pending / 16 : 0;
    xSemaphoreGive(h->lock);
}

esp_err_t audio_in_stop(audio_in_handle_t h)
{
    if (h == NULL) return ESP_ERR_INVALID_ARG;

    h->running = false;
    int64_t deadline = esp_timer_get_time() + 2000000;
    if (h->task_h) {
        if (xSemaphoreTake(h->done_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "audio_in task did not exit within timeout");
            return ESP_ERR_TIMEOUT;
        }
        h->task_h = NULL;
    }
    if (h->tx_task_h) {
        int64_t left_ms = (deadline - esp_timer_get_time()) / 1000;
        TickType_t wait = left_ms > 0 ? pdMS_TO_TICKS((uint32_t)left_ms) : 0;
        if (xSemaphoreTake(h->tx_done_sem, wait) != pdTRUE) {
            ESP_LOGW(TAG, "audio_tx task did not exit within timeout");
            return ESP_ERR_TIMEOUT;
        }
        h->tx_task_h = NULL;
    }
    /* 任一任务超时都保留编码器、队列和性能模式，后续 stop 继续等待。 */
    free(h->tx_packets);
    h->tx_packets = NULL;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->tx_head = h->tx_count = 0;
    xSemaphoreGive(h->lock);
    atomic_store(&h->tasks_ready, false);

    /* 停止 Opus 编码器 */
    if (h->opus_enc) {
        opus_enc_destroy(h->opus_enc);
        h->opus_enc = NULL;
    }

    h->opus_samples = 0;

    /* 任务确实退出后才恢复省电；stop 超时则保持性能模式供后续恢复。 */
    (void)wifi_service_set_audio_active(false);
    ESP_LOGI(TAG, "audio_in stopped");
    return ESP_OK;
}

esp_err_t audio_in_register_pcm_cb(audio_in_handle_t h,
                                   audio_in_pcm_cb_t cb, void *user_data)
{
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    h->pcm_cb     = cb;
    h->pcm_cb_arg = user_data;
    return ESP_OK;
}

esp_err_t audio_in_register_utterance_cb(audio_in_handle_t h,
                                         audio_in_utterance_cb_t cb,
                                         void *user_data)
{
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    /* 与 start/stop 一样由主任务串行调用，只在任务完全停止后调整所有权。 */
    if (h->running || h->task_h || h->tx_task_h) return ESP_ERR_INVALID_STATE;
    if (cb && !h->vad_buf) {
        size_t capacity = (size_t)h->cfg.sample_rate * 20 * sizeof(int16_t);
        h->vad_buf = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!h->vad_buf) return ESP_ERR_NO_MEM;
        h->vad_buf_cap = capacity;
    } else if (!cb) {
        free(h->vad_buf);
        h->vad_buf = NULL;
        h->vad_buf_cap = 0;
    }
    vad_reset(h);
    h->utt_cb     = cb;
    h->utt_cb_arg = user_data;
    return ESP_OK;
}

vad_state_t audio_in_get_vad_state(audio_in_handle_t h)
{
    if (h == NULL) return VAD_STATE_IDLE;
    return h->vad_state;
}

uint32_t audio_in_get_stack_hiwat(audio_in_handle_t h)
{
    if (h == NULL || h->task_h == NULL) return 0;
    return uxTaskGetStackHighWaterMark(h->task_h);
}
