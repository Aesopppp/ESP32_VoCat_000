/* 直接编译主任务会话函数；音频/网络依赖用桩替换，不复制业务流程。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "../../main/audio_in.h"
#include "../../main/ws_service.h"
#include "../../main/wifi_service.h"
#include "../../main/wake_word.h"
#include "../../main/device_mqtt_service.h"
#include "../../main/app_state.h"
#include "../../main/session_policy.h"

static unsigned checks, takes, stops, closes, clears, releases, starts, recoveries, wakes;
static bool conversation_active, session_requested, cleanup_pending;
static bool s_pre_roll_used;
static uint32_t s_wake_boundary_sample;
static int64_t cleanup_at_ms;
static bool voice_auth_refresh_used, awaiting_voice_auth, waiting_for_credentials;
static bool provisioning, voice_enabled, dispatch_allowed, output_speaking, audio_active;
static int64_t clock_ms, retry_at_ms, auth_deadline_ms, resume_deadline_ms;
static uint32_t session_started_ms, input_voice_ms, output_voice_ms;
static ws_speech_status_t platform_speech;
static device_mqtt_delivery_t batch, *reminder_task, *available;
static bool last_handled;
static app_state_t current_state;
static wifi_state_t wifi_state;
static ws_conn_state_t ws_state;
static wake_word_mode_t afe_mode;
static esp_err_t afe_result, upload_result, idle_result;
static audio_in_upload_status_t upload;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static int64_t now_ms(void) { return clock_ms; }
bool provisioning_service_is_active(void) { return provisioning; }
wifi_state_t wifi_service_get_state(void) { return wifi_state; }
ws_conn_state_t ws_service_get_state(void) { return ws_state; }
void ws_service_get_speech_status(ws_speech_status_t *status) { *status=platform_speech; }
esp_err_t wifi_service_set_audio_active(bool active) { audio_active=active; return ESP_OK; }
app_state_t app_state_get(void) { return current_state; }
app_state_t app_state_set(app_state_t s) { app_state_t old=current_state; current_state=s; return old; }
esp_err_t wake_word_set_mode(wake_word_mode_t mode) { afe_mode=mode; return afe_result; }
wake_word_mode_t wake_word_get_mode(void) { return afe_mode; }
void wake_word_reset(void) {}
void wake_word_clear_audio_buffer(void) { ++clears; input_voice_ms=0; }
void wake_word_set_pre_roll(bool enabled) { (void)enabled; }
void wake_word_mark_wake_boundary(void) { s_wake_boundary_sample=1; }
void wake_word_discard_audio_before(uint32_t sample_end) { (void)sample_end; }
audio_in_handle_t audio_in_get_handle(void) { return (void*)1; }
esp_err_t audio_in_stop(audio_in_handle_t h)
{ CHECK(h==(void*)1); ++stops; upload.running=audio_active=false; return ESP_OK; }
void audio_in_get_upload_status(audio_in_handle_t h,audio_in_upload_status_t *s)
{ CHECK(h==(void*)1); *s=upload; }
uint32_t audio_in_get_last_voice_ms(audio_in_handle_t h)
{ CHECK(h==(void*)1); return input_voice_ms; }
uint32_t audio_out_get_last_voice_ms(void) { return output_voice_ms; }
bool audio_out_is_streaming(void) { return output_speaking; }
esp_err_t ws_service_disconnect(void)
{ ++closes; ws_state=WS_STATE_DISCONNECTED; memset(&platform_speech,0,sizeof(platform_speech)); return ESP_OK; }
void device_mqtt_service_set_dispatch_allowed(bool allowed) { dispatch_allowed=allowed; }
void device_mqtt_service_on_wake(void) { ++wakes; }
bool device_mqtt_service_voice_enabled(void) { return voice_enabled; }
device_mqtt_delivery_t *device_mqtt_service_take_tasks(void)
{ ++takes; device_mqtt_delivery_t *p=available; available=NULL; return p; }
void device_mqtt_service_release_tasks(device_mqtt_delivery_t *p,bool handled)
{ CHECK(p==&batch); ++releases; last_handled=handled; }
static void led_set_waiting_wake(void) {}
static void led_set_wifi_err(void) {}
static void led_set_listening(void) {}
static void led_set_speaking(void) {}
static void led_set_connecting(void) {}
esp_err_t app_state_start_listening(void)
{
    ++starts;
    if(upload_result!=ESP_OK) return upload_result;
    CHECK(ws_state==WS_STATE_CONNECTED || ws_state==WS_STATE_PRECONNECTED);
    current_state=APP_STATE_LISTENING; upload.running=true; return ESP_OK;
}
esp_err_t app_state_back_to_wait_wake(void)
{ if(idle_result!=ESP_OK) return idle_result;
  current_state=APP_STATE_WAIT_WAKE; afe_mode=WAKE_WORD_MODE_WAKE; return ESP_OK; }
static void recover_session(const char *reason)
{ CHECK(reason!=NULL); ++recoveries; conversation_active=false; }
#include "app_session.inc"

static void reset(void)
{
    conversation_active=session_requested=cleanup_pending=false;
    cleanup_at_ms=0;
    voice_auth_refresh_used=awaiting_voice_auth=waiting_for_credentials=false;
    provisioning=output_speaking=audio_active=false;
    voice_enabled=dispatch_allowed=true;
    reminder_task=available=NULL; batch.count=2;
    takes=stops=closes=clears=releases=starts=recoveries=wakes=0;
    current_state=APP_STATE_WAIT_WAKE; wifi_state=WIFI_STATE_GOT_IP;
    ws_state=WS_STATE_DISCONNECTED;
    afe_mode=WAKE_WORD_MODE_WAKE; afe_result=upload_result=idle_result=ESP_OK;
    clock_ms=1000; retry_at_ms=auth_deadline_ms=resume_deadline_ms=0;
    session_started_ms=input_voice_ms=output_voice_ms=0;
    memset(&platform_speech,0,sizeof(platform_speech));
    memset(&upload,0,sizeof(upload));
}
static void silence_checks(void)
{
    CHECK(!session_silence_expired(30999,1000,false,0,0));
    CHECK(session_silence_expired(31000,1000,false,0,0));
    CHECK(!session_silence_expired(61000,1000,true,0,0)); /* 长讲话不能误挂。 */
    CHECK(!session_silence_expired(3600000,1000,true,0,0)); /* 漏 stop 不猜测讲话结束。 */
    CHECK(!session_silence_expired(89999,1000,false,60000,50000));
    CHECK(session_silence_expired(90000,1000,false,60000,50000));
    CHECK(!session_silence_expired(90000,1000,false,60000,89999)); /* 尾音重新续期。 */
    CHECK(!session_silence_expired(30999,1000,false,900,800)); /* 前一会话活动无效。 */
    CHECK(session_silence_expired(31000,1000,false,900,800));
    uint32_t begin=UINT32_MAX-15000u;
    CHECK(!session_silence_expired(begin+29999u,begin,false,0,0));
    CHECK(session_silence_expired(begin+30000u,begin,false,0,0));
    CHECK(!session_silence_expired(begin+30000u,begin,false,begin+20000u,0));
    CHECK(!session_silence_expired(begin+30000u,begin,false,0,begin+20000u));
}
static void conversation_checks(void)
{
    reset(); start_conversation("user");
    CHECK(session_requested && !conversation_active && starts==0);
    CHECK(current_state==APP_STATE_WS_CONNECTING_WAKE && retry_at_ms==1000 && audio_active);
    ws_state=WS_STATE_CONNECTED; start_audio_session("auth");
    CHECK(conversation_active && upload.running && starts==1 && session_started_ms==1000);
    input_voice_ms=5000; start_audio_session("duplicate auth");
    CHECK(starts==1 && input_voice_ms==5000); /* 不能重复清空上传 FIFO。 */
    output_speaking=true; clock_ms=60000; output_voice_ms=59999;
    poll_conversation();
    CHECK(current_state==APP_STATE_PLAYING && upload.running && stops==0 && closes==0);
    output_speaking=false; clock_ms=119000; platform_speech.speaking=true;
    poll_conversation();
    CHECK(current_state==APP_STATE_LISTENING && upload.running && stops==0);
    clock_ms+=45000; poll_conversation(); CHECK(conversation_active && stops==0);
    platform_speech.speaking=false; platform_speech.stopped_ms=(uint32_t)clock_ms;
    clock_ms+=29999; poll_conversation(); CHECK(conversation_active);
    for(unsigned i=0;i<120;++i) { clock_ms+=1000; output_voice_ms=(uint32_t)clock_ms; poll_conversation(); }
    CHECK(conversation_active && stops==0); /* 长回复/电话只靠下行也能保持。 */
    clock_ms+=29999; poll_conversation(); CHECK(conversation_active);
    output_speaking=true; ++clock_ms; poll_conversation();
    CHECK(!session_requested && !conversation_active && stops==1 && closes==1);
    CHECK(current_state==APP_STATE_WAIT_WAKE); /* 单靠播放状态不能绕过静默。 */
    CHECK(!retry_at_ms && !resume_deadline_ms); /* 正常待机不能自动重连。 */
    reset(); ws_state=WS_STATE_CONNECTED; start_conversation("no speech");
    output_speaking=true; /* 平台连续静音包，UI 可以一直处于播放状态。 */
    for(unsigned i=0;i<29;++i) {
        clock_ms+=1000; input_voice_ms=(uint32_t)clock_ms; poll_conversation();
        CHECK(conversation_active); /* 本地噪声/VAD不参与待机判定。 */
    }
    clock_ms+=1000; input_voice_ms=(uint32_t)clock_ms; poll_conversation();
    CHECK(!conversation_active && closes==1 && stops==1);
    ws_state=WS_STATE_CONNECTED; output_voice_ms=1000;
    start_conversation("wake again");
    clock_ms+=29999; poll_conversation(); CHECK(conversation_active);
    ++clock_ms; poll_conversation(); CHECK(!conversation_active && closes==2);
    reset(); ws_state=WS_STATE_CONNECTED; start_conversation("start near expiry");
    clock_ms=30999; platform_speech.speaking=true; poll_conversation();
    clock_ms=90000; poll_conversation(); CHECK(conversation_active);
    platform_speech.speaking=false; platform_speech.stopped_ms=(uint32_t)clock_ms;
    clock_ms+=29999; poll_conversation(); CHECK(conversation_active);
    output_voice_ms=(uint32_t)++clock_ms; poll_conversation(); CHECK(conversation_active);
    clock_ms+=30000; poll_conversation(); CHECK(!conversation_active);
    reset(); conversation_active=session_requested=true; ws_state=WS_STATE_CONNECTED;
    upload.running=false; poll_conversation(); CHECK(recoveries==1);
}
static void task_checks(void)
{
    reset(); available=&batch; poll_reminder();
    CHECK(reminder_task==&batch && session_requested && !conversation_active && starts==0);
    CHECK(wakes==0 && releases==0); /* 后台提醒不能解除鉴权失败限制。 */
    poll_reminder(); CHECK(takes==1);
    ws_state=WS_STATE_CONNECTED; start_audio_session("MQTT wake");
    CHECK(upload.running && conversation_active && starts==1 && releases==1 && last_handled);
    CHECK(reminder_task==NULL && closes==0); /* 不等待EOF，不回待机。 */
    available=&batch; output_speaking=true; current_state=APP_STATE_PLAYING;
    poll_reminder();
    CHECK(releases==2 && last_handled && starts==1 && closes==0 && upload.running);
    CHECK(current_state==APP_STATE_PLAYING);
    available=&batch; ws_state=WS_STATE_DISCONNECTED; poll_reminder(); CHECK(releases==2);
    reset(); available=&batch; session_requested=true; poll_reminder(); CHECK(takes==0);
    reset(); available=&batch; provisioning=true; poll_reminder(); CHECK(takes==0);
    provisioning=false; wifi_state=WIFI_STATE_DISCONNECTED; poll_reminder(); CHECK(takes==0);
    wifi_state=WIFI_STATE_GOT_IP; voice_enabled=false; poll_reminder(); CHECK(takes==0);
    reset(); available=&batch; poll_reminder(); end_conversation("connection failed");
    CHECK(releases==1 && !last_handled && !session_requested);
    reset(); available=&batch; poll_reminder(); ws_state=WS_STATE_CONNECTED;
    upload_result=ESP_FAIL; start_audio_session("auth"); CHECK(releases==0 && reminder_task==&batch);
    CHECK(recoveries==1 && !conversation_active);
}
static void touch_checks(void)
{
    reset(); handle_talk_action("touch"); CHECK(wakes==1 && session_requested);
    handle_talk_action("touch during connect"); CHECK(!session_requested && closes==1);
    reset(); session_requested=conversation_active=true; ws_state=WS_STATE_CONNECTED; upload.running=true;
    current_state=APP_STATE_PLAYING; handle_talk_action("touch during playback");
    CHECK(!session_requested && closes==1 && stops==1);
    reset(); available=&batch; poll_reminder(); handle_talk_action("cancel reminder wake");
    CHECK(releases==1 && last_handled && !session_requested);
    reset(); session_requested=true; idle_result=ESP_FAIL;
    end_conversation("stop timeout");
    CHECK(cleanup_pending && clears==0 && current_state==APP_STATE_ERROR);
    handle_talk_action("touch during cleanup");
    CHECK(!session_requested && starts==0 && clears==0);
    idle_result=ESP_OK; stop_session_audio();
    CHECK(!cleanup_pending && clears==1 && current_state==APP_STATE_WAIT_WAKE);
    reset(); provisioning=true; handle_talk_action("menu");
    CHECK(!session_requested && starts==0 && closes==0 && wakes==0);
}
int main(void)
{
    silence_checks(); conversation_checks(); task_checks(); touch_checks();
    printf("PASS: %u unified app session checks\n",checks); return 0;
}
