/* 运行真实生产编码/TX task。Windows fiber 只提供确定性 RTOS 调度桩：
 * 编码消耗 core0 时间，网络等待让出 core1；不模拟 Opus 声学或实板调度性能。 */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "../../main/audio_in.c"

static bool latency_uplink_pending;
void latency_log_event(const char *event, uint32_t ms)
{ (void)event; (void)ms; }
void latency_log_mark_upload_phase(void) { latency_uplink_pending = true; }
bool latency_log_first_live_uplink_pending(void)
{ bool pending = latency_uplink_pending; latency_uplink_pending = false; return pending; }

#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s (time=%lld)\n", __LINE__, #expr, (long long)test_time_us); exit(1); } } while (0)
static unsigned checks, produced, encoded, packets, attempts, failed_events;
static unsigned fail_packet, encode_failure, pre_roll_samples, input_limit;
static unsigned encode_cost_us, send_cost_us, stall_packet, stall_us, max_backlog;
static unsigned speech_starts, speech_ends, utterances, utterance_allocations, queue_allocations;
static unsigned generation = 7, expected_start, confirmed_end, confirm_calls;
/* FIFO handoff skips stale PCM coordinates, but never resets the encoder/TX order. */
static unsigned handoff_sample, handoff_skip_samples;
static bool allocation_fails, wifi_audio_active, wifi_mode_failure;
static unsigned wifi_mode_calls, task_create_attempt, fail_create_at;
static int64_t test_time_us, last_send_us;
static audio_in_handle_t active_input;
static bool hold_encoder, hold_tx, send_started;
static unsigned peak_queue;
static uint64_t encoded_work, sent_work;

struct test_sem { int value; bool mutex; };
struct test_task {
    void *fiber;
    void (*entry)(void *);
    void *arg;
    int64_t ready_at;
    SemaphoreHandle_t waiting;
    bool finished;
    bool tx;
};
static struct test_task tasks[2], *current_task;
static unsigned task_count;
static void *main_fiber;

static bool run_one_until(int64_t deadline)
{
    struct test_task *next = NULL;
    for (unsigned i = 0; i < task_count; ++i) {
        struct test_task *t = &tasks[i];
        if (t->finished || (t->tx ? hold_tx : hold_encoder)) continue;
        if (!next || t->ready_at < next->ready_at) next = t;
    }
    if (!next || next->ready_at > deadline) {
        if (test_time_us < deadline) test_time_us = deadline;
        return false;
    }
    if (test_time_us < next->ready_at) test_time_us = next->ready_at;
    current_task = next;
    SwitchToFiber(next->fiber);
    current_task = NULL;
    return true;
}
static void run_until(int64_t deadline)
{
    unsigned steps = 0;
    while (run_one_until(deadline)) CHECK(++steps < 1000000);
}
static void task_wait_us(uint64_t us)
{
    CHECK(current_task != NULL);
    current_task->ready_at = test_time_us + us;
    SwitchToFiber(main_fiber);
}
static VOID WINAPI task_fiber(void *arg)
{
    struct test_task *task = arg;
    task->entry(task->arg);
    CHECK(false); /* 生产任务应自行 vTaskDelete。 */
}
static void release_tasks(void)
{
    for (unsigned i = 0; i < task_count; ++i) {
        CHECK(tasks[i].finished);
        DeleteFiber(tasks[i].fiber);
    }
    memset(tasks, 0, sizeof(tasks));
    task_count = task_create_attempt = 0;
}

void *test_heap_caps_malloc(size_t bytes, unsigned caps)
{
    CHECK(caps & MALLOC_CAP_SPIRAM);
    if (bytes == 640000) ++utterance_allocations;
    else { CHECK(bytes == sizeof(audio_tx_packet_t) * AUDIO_TX_QUEUE_PACKETS); ++queue_allocations; }
    return allocation_fails ? NULL : malloc(bytes);
}
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{ SemaphoreHandle_t s = calloc(1, sizeof(*s)); s->value = 1; s->mutex = true; return s; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return calloc(1, sizeof(struct test_sem)); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait)
{
    int64_t deadline = test_time_us + (int64_t)wait * 1000;
    while (!s->value) {
        if (!wait || test_time_us >= deadline) return pdFALSE;
        CHECK(!s->mutex); /* 生产代码不得持互斥锁让出 CPU/调用网络。 */
        if (current_task) {
            current_task->waiting = s;
            current_task->ready_at = deadline;
            SwitchToFiber(main_fiber);
            current_task->waiting = NULL;
        } else if (!run_one_until(deadline)) return pdFALSE;
    }
    --s->value;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    CHECK(!s->mutex || s->value == 0);
    s->value = 1;
    for (unsigned i = 0; i < task_count; ++i) {
        if (tasks[i].waiting == s) tasks[i].ready_at = test_time_us;
    }
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t s) { free(s); }
int xPortGetCoreID(void) { return current_task && current_task->tx ? 1 : 0; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 1024; }
unsigned esp_log_timestamp(void) { return (unsigned)(test_time_us / 1000); }
int64_t esp_timer_get_time(void) { return test_time_us; }
void vTaskDelete(TaskHandle_t task)
{
    CHECK(task == NULL && current_task != NULL);
    current_task->finished = true;
    SwitchToFiber(main_fiber);
    CHECK(false);
}
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, unsigned stack,
    void *arg, unsigned priority, TaskHandle_t *task, unsigned core)
{
    CHECK(wifi_audio_active && stack == TASK_STACK_AUDIO_IN);
    CHECK(priority == 4 && core == (strcmp(name, "audio_tx") == 0 ? 1u : 0u));
    if (++task_create_attempt == fail_create_at) { *task = NULL; return pdFALSE; }
    CHECK(task_count < 2);
    struct test_task *t = &tasks[task_count++];
    *t = (struct test_task){ .entry = fn, .arg = arg, .ready_at = test_time_us,
                             .tx = strcmp(name, "audio_tx") == 0 };
    t->fiber = CreateFiber(65536, task_fiber, t);
    CHECK(t->fiber != NULL);
    *task = t;
    return pdPASS;
}
void vTaskDelay(TickType_t ticks) { task_wait_us((uint64_t)ticks * 1000); }
esp_err_t wifi_service_set_audio_active(bool active)
{
    if (!active) {
        for (unsigned i = 0; i < task_count; ++i) CHECK(tasks[i].finished);
    }
    wifi_audio_active = active; ++wifi_mode_calls;
    return wifi_mode_failure ? ESP_FAIL : ESP_OK;
}
uint32_t ws_service_generation(void) { return generation; }
static unsigned source_sample(unsigned index)
{ return index + (handoff_skip_samples && index >= handoff_sample ? handoff_skip_samples : 0); }
static unsigned source_end(unsigned index)
{ return index + (handoff_skip_samples && index > handoff_sample ? handoff_skip_samples : 0); }
esp_err_t ws_service_send_opus_frame(const uint8_t *data, size_t len)
{
    CHECK(current_task && current_task->tx);
    CHECK(active_input->lock->value == 1 && len == sizeof(uint32_t));
    uint32_t first_sample;
    memcpy(&first_sample, data, sizeof(first_sample));
    CHECK(first_sample == source_sample(expected_start)); /* 已选择的首句/live采样不重复或乱序。 */
    unsigned attempt = ++attempts;
    unsigned delay = send_cost_us + (attempt == stall_packet ? stall_us : 0);
    unsigned before_count = active_input->tx_count;
    CHECK(before_count > 0 && before_count <= AUDIO_TX_QUEUE_PACKETS);
    send_started = true;
    task_wait_us(delay);
    sent_work += delay;
    uint32_t still_first;
    memcpy(&still_first, data, sizeof(still_first));
    CHECK(still_first == source_sample(expected_start)); /* 等待期间 head 未被生产者覆盖。 */
    CHECK(active_input->tx_count >= before_count); /* in-flight 槽未提前复用。 */
    if (attempt == fail_packet) return ESP_ERR_TIMEOUT;
    ++packets; expected_start += 320; last_send_us = test_time_us;
    return ESP_OK;
}
void wake_word_confirm_audio_sent(uint32_t sample_end)
{
    CHECK(active_input->lock->value == 1);
    CHECK(sample_end == source_end(expected_start) && active_input->sent_end == sample_end);
    CHECK(sample_end > confirmed_end);
    confirmed_end = sample_end; ++confirm_calls;
}
BaseType_t event_bus_publish(event_type_t type, const void *data, size_t size)
{
    (void)size;
    if (type == EV_AUDIO_UPLOAD_FAILED) {
        ++failed_events;
        CHECK((uintptr_t)data == active_input->connection_generation);
        CHECK(active_input->lock->value == 1);
    }
    if (type == EV_AUDIO_SPEECH_START) ++speech_starts;
    if (type == EV_AUDIO_SPEECH_END) ++speech_ends;
    return pdTRUE;
}
opus_enc_handle_t opus_enc_create(const opus_enc_config_t *cfg)
{
    CHECK(cfg->frame_ms == 20 && cfg->sample_rate == 16000);
    CHECK(cfg->channels == 1 && cfg->bitrate == 24000 && cfg->complexity == 0);
    return (opus_enc_handle_t)1;
}
void opus_enc_destroy(opus_enc_handle_t encoder)
{
    (void)encoder;
    for (unsigned i = 0; i < task_count; ++i) CHECK(tasks[i].finished);
}
int opus_enc_encode(opus_enc_handle_t encoder, const int16_t *pcm, int bytes,
                    uint8_t *output, int capacity)
{
    (void)encoder; CHECK(bytes == 640 && capacity == WS_OPUS_FRAME_MAX_BYTES);
    CHECK(current_task && !current_task->tx);
    uint32_t first = source_sample(encoded);
    for (unsigned i = 0; i < 320; ++i) CHECK(pcm[i] == (int16_t)source_sample(encoded++));
    task_wait_us(encode_cost_us);
    encoded_work += encode_cost_us;
    if (encode_failure && encoded / 320 == encode_failure) return -1;
    memcpy(output, &first, sizeof(first));
    return sizeof(first);
}
int wake_word_get_chunksize(void) { return 512; }
static unsigned written_samples(void)
{
    unsigned available = pre_roll_samples + (unsigned)(test_time_us / 32000) * 512;
    return available > input_limit ? input_limit : available;
}
void wake_word_get_audio_progress(wake_word_audio_progress_t *p)
{ *p = (wake_word_audio_progress_t){source_end(written_samples()), source_end(produced), 24000, 1234}; }
size_t wake_word_read_audio(int16_t *pcm, size_t capacity, uint32_t *sample_end)
{
    CHECK(capacity == 1024);
    unsigned backlog = written_samples() - produced;
    if (backlog > max_backlog) max_backlog = backlog;
    if (active_input->tx_count > peak_queue) peak_queue = active_input->tx_count;
    if (backlog < 512) return 0;
    for (unsigned i = 0; i < 512; ++i) pcm[i] = (int16_t)source_sample(produced++);
    *sample_end = source_end(produced);
    return 1024;
}
static void reset_case(void)
{
    CHECK(!task_count);
    produced = encoded = packets = attempts = failed_events = 0;
    fail_packet = encode_failure = stall_packet = stall_us = 0;
    pre_roll_samples = 0; input_limit = 30720;
    encode_cost_us = send_cost_us = 0;
    expected_start = confirmed_end = confirm_calls = max_backlog = peak_queue = 0;
    handoff_sample = handoff_skip_samples = 0;
    encoded_work = sent_work = 0;
    test_time_us = last_send_us = 0;
    hold_encoder = hold_tx = send_started = false;
    fail_create_at = 0; generation = 7;
    allocation_fails = false;
    active_input = audio_in_create(NULL);
    CHECK(active_input != NULL && active_input->vad_buf == NULL);
    CHECK(audio_in_get_last_voice_ms(active_input) == 0);
}
static void start_case(void)
{
    unsigned allocations = queue_allocations;
    CHECK(audio_in_start_from_ringbuf(active_input, (void *)1) == ESP_OK);
    CHECK(queue_allocations == allocations + 1 && wifi_audio_active);
    CHECK(audio_in_get_last_voice_ms(active_input) == 1234);
}
static void finish_case(void)
{
    CHECK(audio_in_stop(active_input) == ESP_OK);
    CHECK(!wifi_audio_active && !active_input->task_h && !active_input->tx_task_h);
    CHECK(!active_input->tx_packets && !active_input->opus_enc);
    release_tasks();
    audio_in_destroy(active_input);
    active_input = NULL;
}
static void check_upload(bool backlog)
{
    reset_case();
    encode_cost_us = 9000; send_cost_us = 10000;
    input_limit = 81920; /* 5.12s PCM，整除 512/320。 */
    pre_roll_samples = backlog ? 32256 : 0; /* 2016ms 含首句预录音。 */
    start_case();
    unsigned allocations = queue_allocations;
    run_until(backlog ? 4000000 : 5400000);
    CHECK(produced == input_limit && encoded == input_limit);
    CHECK(packets == input_limit / 320 && confirm_calls == packets);
    CHECK(confirmed_end == input_limit && active_input->tx_count == 0);
    CHECK(failed_events == 0 && queue_allocations == allocations);
    audio_in_upload_status_t status;
    audio_in_get_upload_status(active_input, &status);
    CHECK(status.pending_ms == 0 && status.tx_queue_ms == 0);
    if (backlog) {
        /* 确定性双核模型在实时尾部约3.1s追上。串行9+10ms仅包耗时即4.864s。
         * 这只验证生产代码允许重叠，不等同实际RTOS/TLS/PSRAM吞吐测试。 */
        CHECK(last_send_us < 3300000);
        CHECK(max_backlog <= pre_roll_samples + 512);
        CHECK(active_input->encode_work_us == encoded_work);
        CHECK(active_input->send_work_us == sent_work);
    }
    finish_case();
}
static void check_pause_resume(void)
{
    reset_case(); input_limit = 512; start_case();
    hold_tx = true;
    run_until(50000);
    CHECK(active_input->opus_samples == 192 && active_input->tx_count == 1);
    /* 一侧无法确认时不能把旧信号当作双侧暂停成功。 */
    CHECK(audio_in_pause(active_input) == ESP_ERR_TIMEOUT);
    CHECK(active_input->pause_ack_epoch == active_input->pause_epoch);
    CHECK(active_input->tx_pause_ack_epoch != active_input->pause_epoch);
    CHECK(packets == 0);
    hold_tx = false;
    CHECK(audio_in_pause(active_input) == ESP_OK);
    CHECK(active_input->pause_ack_epoch == active_input->pause_epoch);
    CHECK(active_input->tx_pause_ack_epoch == active_input->pause_epoch);
    CHECK(audio_in_pause(active_input) == ESP_OK); /* 连续暂停必须确认新代次。 */
    unsigned epoch = active_input->pause_epoch, calls = wifi_mode_calls;
    CHECK(audio_in_start_from_ringbuf(active_input, (void *)1) == ESP_OK);
    CHECK(active_input->opus_samples == 192 && active_input->tx_count == 1);
    CHECK(wifi_mode_calls == calls);
    CHECK(audio_in_pause(active_input) == ESP_OK && active_input->pause_epoch == epoch + 1);
    CHECK(audio_in_start_from_ringbuf(active_input, (void *)1) == ESP_OK);
    input_limit = 2560;
    run_until(test_time_us + 200000);
    CHECK(encoded == 2560 && packets == 8 && confirmed_end == 2560);
    finish_case();
}
static void check_phrase_to_live_handoff(void)
{
    /* Five AFE boundaries cover every 512/320 alignment, including no half-packet.
     * 2.56 s of old tail is removed by the FIFO; production audio_in must preserve
     * every chosen sample, even when one Opus packet straddles that discontinuity. */
    for (unsigned frames = 1; frames <= 5; ++frames) {
        reset_case();
        handoff_sample = frames * 512;
        handoff_skip_samples = 40960;
        pre_roll_samples = input_limit = 5120;
        encode_cost_us = 9000; send_cost_us = 10000;
        start_case();
        if (frames == 1) {
            hold_tx = true;
            run_until(100000);
            CHECK(produced > handoff_sample && confirmed_end == 0);
            CHECK(active_input->tx_count == AUDIO_TX_QUEUE_PACKETS);
            hold_tx = false; /* Source can change while the first phrase is still queued. */
        }
        run_until(800000);
        CHECK(encoded == input_limit && packets == input_limit / 320);
        CHECK(confirmed_end == input_limit + handoff_skip_samples);
        CHECK(!active_input->opus_samples && !active_input->tx_count && !failed_events);
        audio_in_upload_status_t status;
        audio_in_get_upload_status(active_input, &status);
        CHECK(status.pending_ms == 0 && status.tx_queue_ms == 0);
        finish_case();
    }
}
static void check_pause_during_send(void)
{
    reset_case(); pre_roll_samples = input_limit = 512;
    send_cost_us = 100000; start_case();
    run_until(20000);
    CHECK(send_started && packets == 0 && active_input->tx_count == 1);
    int64_t requested_at = test_time_us;
    CHECK(audio_in_pause(active_input) == ESP_OK);
    CHECK(packets == 1 && confirmed_end == 320);
    CHECK(last_send_us >= requested_at);
    CHECK(active_input->pause_ack_epoch == active_input->pause_epoch);
    CHECK(active_input->tx_pause_ack_epoch == active_input->pause_epoch);
    CHECK(active_input->opus_samples == 192);
    finish_case();
}
static void check_queue_pressure(void)
{
    reset_case(); pre_roll_samples = input_limit = 5120;
    encode_cost_us = 9000; send_cost_us = 10000;
    stall_packet = 1; stall_us = 135000; start_case();
    run_until(100000);
    CHECK(active_input->tx_count == 4 && packets == 0 && failed_events == 0);
    audio_in_upload_status_t status;
    audio_in_get_upload_status(active_input, &status);
    CHECK(status.tx_queue_ms == 80 && status.pending_ms == 320);
    CHECK(confirmed_end == 0); /* 编码/排队不能解除首句保护。 */
    run_until(800000);
    CHECK(encoded == input_limit && packets == 16 && failed_events == 0);
    CHECK(active_input->send_work_us == 16 * send_cost_us + stall_us);
    finish_case();

    reset_case(); pre_roll_samples = input_limit = 5120; start_case(); hold_tx = true;
    run_until(100000);
    CHECK(active_input->tx_count == 4 && failed_events == 0);
    run_until(2200000);
    CHECK(failed_events == 1 && !active_input->running && confirmed_end == 0);
    hold_tx = false;
    finish_case();
}
static void check_failure_and_reconnect(void)
{
    reset_case(); pre_roll_samples = input_limit = 5120;
    encode_cost_us = 9000; send_cost_us = 10000; fail_packet = 5;
    start_case(); run_until(300000);
    CHECK(failed_events == 1 && attempts == 5 && packets == 4);
    CHECK(active_input->sent_end == 1280 && confirmed_end == 1280);
    CHECK(active_input->stats_packets == 4 && active_input->tx_count > 0);
    fail_audio_upload(active_input, ESP_FAIL); CHECK(failed_events == 1);
    CHECK(active_input->encode_work_us == encoded_work && active_input->send_work_us == sent_work);
    finish_case();

    reset_case(); pre_roll_samples = input_limit = 5120; start_case(); hold_tx = true;
    run_until(10000); CHECK(active_input->tx_count > 0);
    ++generation; hold_tx = false; run_until(100000);
    CHECK(failed_events == 1 && attempts == 0 && confirmed_end == 0);
    CHECK(audio_in_stop(active_input) == ESP_OK); release_tasks();
    produced = encoded = expected_start = 0;
    start_case(); run_until(test_time_us + 200000);
    CHECK(packets == 16 && confirmed_end == input_limit);
    CHECK(active_input->connection_generation == generation);
    finish_case();

    reset_case(); pre_roll_samples = input_limit = 5120;
    encode_failure = 2; start_case(); run_until(100000);
    CHECK(failed_events == 1 && encoded == 640 && packets <= 1);
    finish_case();
}
static void check_lifecycle(void)
{
    for (unsigned failure = 1; failure <= 2; ++failure) {
        reset_case(); fail_create_at = failure;
        CHECK(audio_in_start_from_ringbuf(active_input, (void *)1) == ESP_ERR_NO_MEM);
        CHECK(!wifi_audio_active && !active_input->running);
        CHECK(!active_input->task_h && !active_input->tx_task_h && !active_input->tx_packets);
        CHECK(!produced && !attempts);
        release_tasks(); audio_in_destroy(active_input);
    }
    reset_case(); allocation_fails = true;
    CHECK(audio_in_start_from_ringbuf(active_input, (void *)1) == ESP_ERR_NO_MEM);
    CHECK(!task_count && !wifi_audio_active); allocation_fails = false;
    audio_in_destroy(active_input);

    reset_case(); pre_roll_samples = input_limit = 5120;
    send_cost_us = 2500000; start_case(); run_until(10000);
    CHECK(attempts == 1);
    audio_tx_packet_t *storage = active_input->tx_packets;
    CHECK(audio_in_stop(active_input) == ESP_ERR_TIMEOUT);
    CHECK(wifi_audio_active && active_input->tx_packets == storage && active_input->opus_enc);
    CHECK(active_input->task_h == NULL && active_input->tx_task_h != NULL);
    CHECK(audio_in_register_utterance_cb(active_input, NULL, NULL) == ESP_ERR_INVALID_STATE);
    run_until(2600000);
    CHECK(packets == 1); /* 已进入WS的包允许完成，但后续排队包不发送。 */
    finish_case();

    reset_case(); start_case(); hold_encoder = true;
    CHECK(audio_in_stop(active_input) == ESP_ERR_TIMEOUT);
    CHECK(wifi_audio_active && active_input->tx_packets != NULL);
    hold_encoder = false; finish_case();
}
static void utterance_callback(void *arg, const int16_t *pcm, size_t bytes)
{
    CHECK(arg == active_input); CHECK(pcm == active_input->vad_buf);
    CHECK(bytes > 0 && bytes <= active_input->vad_buf_cap); ++utterances;
}
static void feed_utterance(void)
{
    int16_t pcm[512];
    unsigned started = speech_starts, ended = speech_ends;
    for (unsigned f = 0; f < 100; ++f) {
        for (unsigned i = 0; i < 512; ++i) pcm[i] = f < 60 ? 32767 : 0;
        process_vad_frame(active_input, pcm, 512, f * 32);
        CHECK(active_input->energy_sum >= 0); /* 饱和采样的 8 帧均方累加不溢出。 */
    }
    CHECK(speech_starts == started + 1 && speech_ends == ended + 1);
    CHECK(active_input->vad_state == VAD_STATE_IDLE);
}
static void check_optional_utterance(void)
{
    active_input = audio_in_create(NULL);
    CHECK(active_input && !active_input->vad_buf);
    unsigned allocations_before = utterance_allocations;
    feed_utterance(); CHECK(!utterances && utterance_allocations == allocations_before);
    allocation_fails = true;
    CHECK(audio_in_register_utterance_cb(active_input, utterance_callback, active_input) == ESP_ERR_NO_MEM);
    CHECK(!active_input->utt_cb && !active_input->vad_buf);
    allocation_fails = false;
    CHECK(audio_in_register_utterance_cb(active_input, utterance_callback, active_input) == ESP_OK);
    CHECK(active_input->vad_buf_cap == 640000);
    feed_utterance(); CHECK(utterances == 1 && utterance_allocations == allocations_before + 2);
    active_input->running = true;
    CHECK(audio_in_register_utterance_cb(active_input, NULL, NULL) == ESP_ERR_INVALID_STATE);
    active_input->running = false;
    CHECK(audio_in_register_utterance_cb(active_input, NULL, NULL) == ESP_OK);
    CHECK(!active_input->vad_buf && !active_input->vad_buf_cap);
    feed_utterance(); CHECK(utterances == 1);
    audio_in_destroy(active_input);
}
int main(void)
{
    main_fiber = ConvertThreadToFiber(NULL);
    CHECK(main_fiber != NULL);
    check_upload(false);
    check_upload(true);
    check_phrase_to_live_handoff();
    check_pause_resume();
    check_pause_during_send();
    check_queue_pressure();
    check_failure_and_reconnect();
    check_lifecycle();
    check_optional_utterance();
    wifi_mode_failure = true;
    check_upload(false); /* Wi-Fi设置失败只告警，不制造上传失败。 */
    printf("PASS: %u upload checks (production dual tasks, bounded queue, ordered 2s pre-roll catch-up, pause epochs, failures, reconnect, join-before-free)\n", checks);
    return 0;
}
