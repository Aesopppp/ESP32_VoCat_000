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
#include "../../main/device_handshake_service.h"
#include "../../main/device_preconnect_config.h"
#include "../../main/app_state.h"
#include "../../main/session_policy.h"

static unsigned checks, takes, stops, closes, clears, releases, starts, recoveries, wakes;
static bool conversation_active, session_requested, cleanup_pending;
static bool pre_roll_active, mode_kept_pre_roll, wake_on_return_to_idle;
static unsigned pre_roll_prepares, pre_roll_cancels;
static bool preconnect_owned, preconnect_unsupported, valid_lease, hot_pre_roll;
static bool refresh_due;
static bool preconnect_token_renewed, preconnect_token_blocked;
static unsigned preconnect_epoch, preconnect_failures, http_epoch;
static int64_t preconnect_deadline_ms, preconnect_poll_at_ms;
static unsigned dials, preauth, activations, prepared_tasks;
static device_handshake_response_t configuration;
static device_ws_readiness_t readiness;
static esp_err_t activation_result;
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
static unsigned esp_random(void) { return 17; }
device_ws_readiness_t device_handshake_service_prepare_websocket(const device_handshake_response_t **cfg)
{ *cfg = readiness == DEVICE_WS_READY ? &configuration : NULL; return readiness; }
unsigned device_handshake_service_websocket_epoch(void) { return http_epoch; }
bool device_handshake_service_renew_credentials(void)
{ if (readiness!=DEVICE_WS_READY) return false; ++http_epoch; readiness=DEVICE_WS_WAITING; return true; }
esp_err_t ws_service_set_credentials(const char *url, const char *token)
{ CHECK(ws_state==WS_STATE_DISCONNECTED && !strcmp(url,"wss://example") && !strcmp(token,"test")); return ESP_OK; }
esp_err_t ws_service_prepare_preconnect(const char *id)
{ CHECK(!strcmp(id,"dev_test")); return ESP_OK; }
esp_err_t ws_service_connect(void) { ++dials; ws_state=WS_STATE_CONNECTING; return ESP_OK; }
esp_err_t ws_service_authenticate(void)
{ CHECK(ws_state==WS_STATE_PREAUTHING); ++preauth; return ESP_OK; }
esp_err_t ws_service_prepare_reminder(const char *id,const char *const *tasks,size_t count)
{ CHECK(!strcmp(id,"dev_test") && count==2 && !strcmp(tasks[0],"a") && !strcmp(tasks[1],"b"));
  ++prepared_tasks; return ESP_OK; }
bool ws_service_preconnect_valid(void) { return valid_lease && ws_state==WS_STATE_PRECONNECTED; }
bool ws_service_preconnect_refresh_due(void) { return refresh_due && ws_state==WS_STATE_PRECONNECTED; }
esp_err_t ws_service_activate_preconnection(void)
{ CHECK(ws_state==WS_STATE_PRECONNECTED); ++activations;
  if (activation_result==ESP_OK) ws_state=WS_STATE_ACTIVATING;
  return activation_result; }
void ws_service_get_speech_status(ws_speech_status_t *status) { *status=platform_speech; }
esp_err_t wifi_service_set_audio_active(bool active) { audio_active=active; return ESP_OK; }
app_state_t app_state_get(void) { return current_state; }
app_state_t app_state_set(app_state_t s) { app_state_t old=current_state; current_state=s; return old; }
esp_err_t wake_word_set_mode(wake_word_mode_t mode)
{ mode_kept_pre_roll=pre_roll_active; afe_mode=mode; return afe_result; }
wake_word_mode_t wake_word_get_mode(void) { return afe_mode; }
void wake_word_reset(void) {}
void wake_word_clear_audio_buffer(void) { ++clears; input_voice_ms=0; pre_roll_active=false; }
void wake_word_set_pre_roll(bool enabled)
{ if(!enabled) { ++pre_roll_cancels; pre_roll_active=false; } }
void wake_word_select_hot_pre_roll(void) { hot_pre_roll=pre_roll_active; }
void wake_word_fallback_cold_pre_roll(void) { hot_pre_roll=false; }
bool wake_word_prepare_pre_roll_upload(void)
{ ++pre_roll_prepares; return pre_roll_active; }
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
    CHECK(ws_state==WS_STATE_CONNECTED);
    current_state=APP_STATE_LISTENING; upload.running=true; return ESP_OK;
}
esp_err_t app_state_back_to_wait_wake(void)
{ if(idle_result!=ESP_OK) return idle_result;
  wake_word_clear_audio_buffer();
  current_state=APP_STATE_WAIT_WAKE; afe_mode=WAKE_WORD_MODE_WAKE;
  if(wake_on_return_to_idle) pre_roll_active=true; /* 模拟发布待机后立即检测到新唤醒。 */
  return ESP_OK; }
static void recover_session(const char *reason)
{ CHECK(reason!=NULL); ++recoveries; conversation_active=false; }
#include "app_session.inc"

static void reset(void)
{
    preconnect_owned=preconnect_unsupported=hot_pre_roll=false;
    preconnect_token_renewed=preconnect_token_blocked=false;
    preconnect_epoch=preconnect_failures=dials=preauth=activations=prepared_tasks=0;
    preconnect_deadline_ms=preconnect_poll_at_ms=0; http_epoch=1;
    valid_lease=true; refresh_due=false; activation_result=ESP_OK; readiness=DEVICE_WS_READY;
    strcpy(configuration.mqtt.username,"dev_test");
    strcpy(configuration.websocket.url,"wss://example"); strcpy(configuration.websocket.token,"test");
    strcpy(batch.device_id,"dev_test"); strcpy(batch.task_ids[0],"a"); strcpy(batch.task_ids[1],"b");
    conversation_active=session_requested=cleanup_pending=false;
    pre_roll_active=mode_kept_pre_roll=wake_on_return_to_idle=false;
    pre_roll_prepares=pre_roll_cancels=0;
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
static void wake_pre_roll_checks(void)
{
    /* 检测任务在发布 wake 事件前已保护 FIFO；主任务不得在切模式时取消。 */
    reset(); pre_roll_active=true; start_conversation("wake word");
    CHECK(session_requested && !conversation_active && mode_kept_pre_roll);
    CHECK(pre_roll_active && pre_roll_cancels==0 && clears==0);
    start_conversation("wake word");
    CHECK(pre_roll_active && pre_roll_cancels==0 && clears==0);
    clock_ms+=5000; ws_state=WS_STATE_CONNECTED; start_audio_session("wake auth");
    CHECK(conversation_active && upload.running && pre_roll_prepares==1 && clears==0);
    CHECK(pre_roll_active); /* 鉴权不能提前解除 FIFO 的补传保护。 */
    start_audio_session("duplicate auth");
    CHECK(pre_roll_prepares==1 && starts==1 && clears==0);
    end_conversation("touch");
    CHECK(!pre_roll_active && clears==1);
    start_conversation("REC button");
    ws_state=WS_STATE_CONNECTED; start_audio_session("button auth");
    CHECK(conversation_active && !pre_roll_active && clears==2);
    /* 开关关闭/没有检测边界时回退旧清空路径，不能伪造边界。 */
    reset(); start_conversation("wake word");
    ws_state=WS_STATE_CONNECTED; start_audio_session("auth without pre-roll");
    CHECK(conversation_active && clears==1);
    const char *sources[]={"touch", "REC button", "MQTT notification"};
    for(unsigned i=0;i<sizeof(sources)/sizeof(sources[0]);++i) {
        reset(); pre_roll_active=true; start_conversation(sources[i]);
        CHECK(!pre_roll_active && !mode_kept_pre_roll && pre_roll_cancels==1);
        ws_state=WS_STATE_CONNECTED; start_audio_session("non-wake auth");
        CHECK(conversation_active && upload.running && clears==1);
    }
    reset(); pre_roll_active=true; provisioning=true; start_conversation("wake word");
    CHECK(!session_requested && !pre_roll_active && pre_roll_cancels==1);
    reset(); pre_roll_active=true; wifi_state=WIFI_STATE_DISCONNECTED;
    start_conversation("wake word");
    CHECK(!session_requested && !pre_roll_active && pre_roll_cancels==1);
    reset(); pre_roll_active=true; start_conversation("wake word");
    ws_state=WS_STATE_CONNECTED; start_audio_session("auth");
    wake_on_return_to_idle=true; end_conversation("touch then immediate wake");
    CHECK(pre_roll_active && clears==1); /* 旧会话收尾不能抹掉新边界。 */
    start_conversation("wake word"); ws_state=WS_STATE_CONNECTED;
    start_audio_session("next auth");
    CHECK(conversation_active && pre_roll_active && clears==1);
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
/* Production handlers with an idle, preauthenticated WebSocket. */
static void standby_transport(void)
{
    poll_preconnection(clock_ms,false);
    CHECK(preconnect_owned && dials==1 && !session_requested && !conversation_active);
    CHECK(current_state==APP_STATE_WAIT_WAKE && afe_mode==WAKE_WORD_MODE_WAKE && !audio_active);
    ws_state=WS_STATE_PREAUTHING; CHECK(handle_preconnection_event(EV_WS_CONNECTED));
    CHECK(preauth==1 && !activations && !starts);
}
static void standby_ready(void)
{
    standby_transport();
    ws_state=WS_STATE_PRECONNECTED;
    CHECK(handle_preconnection_event(EV_WS_PRECONNECT_READY));
    CHECK(!starts && !activations && preconnect_owned && !preconnect_deadline_ms);
}
static void standby_refresh_checks(void)
{
    reset();
    standby_ready();
    clock_ms+=500;
    poll_preconnection(clock_ms,false);
    CHECK(dials==1 && preconnect_owned);

    refresh_due=true;
    clock_ms+=500;
    poll_preconnection(clock_ms,false);
    CHECK(dials==2 && preconnect_owned && ws_state==WS_STATE_CONNECTING);
    CHECK(!preconnect_failures && !retry_at_ms && http_epoch==1);
    CHECK(!starts && !stops && !clears && !session_requested && !audio_active);
    CHECK(current_state==APP_STATE_WAIT_WAKE && afe_mode==WAKE_WORD_MODE_WAKE);

    /* The old standby socket is already closed. Preserve the first phrase on the new one. */
    pre_roll_active=true;
    start_conversation("wake word");
    CHECK(pre_roll_active && !hot_pre_roll && !activations && !starts);
    ws_state=WS_STATE_PREAUTHING;
    CHECK(handle_preconnection_event(EV_WS_CONNECTED));
    refresh_due=false;
    ws_state=WS_STATE_PRECONNECTED;
    CHECK(handle_preconnection_event(EV_WS_PRECONNECT_READY));
    CHECK(activations==1 && dials==2 && !starts && !clears);
    ws_state=WS_STATE_CONNECTED;
    CHECK(!handle_preconnection_event(EV_WS_AUTH_OK));
    start_audio_session("wake during standby refresh");
    CHECK(upload.running && pre_roll_prepares==1 && !clears);

    /* If activation is requested before the old socket is closed, use its remaining lease. */
    reset();
    standby_ready();
    refresh_due=pre_roll_active=true;
    start_conversation("wake word");
    CHECK(activations==1 && dials==1 && hot_pre_roll && !starts && !preconnect_failures);
    ws_state=WS_STATE_CONNECTED;
    CHECK(!handle_preconnection_event(EV_WS_AUTH_OK));
    start_audio_session("wake at refresh window");
    clock_ms+=600000;
    poll_preconnection(clock_ms,false);
    CHECK(conversation_active && upload.running && dials==1 && !stops);

    reset();
    standby_ready();
    refresh_due=true;
    available=&batch;
    poll_reminder();
    CHECK(activations==1 && dials==1 && prepared_tasks==1 && reminder_task==&batch);
    CHECK(!starts && !releases && !preconnect_failures);

    reset();
    standby_ready();
    refresh_due=true;
    clock_ms+=500;
    poll_preconnection(clock_ms,false);
    available=&batch;
    poll_reminder();
    CHECK(reminder_task==&batch && !prepared_tasks && !activations && !releases);
    ws_state=WS_STATE_PREAUTHING;
    CHECK(handle_preconnection_event(EV_WS_CONNECTED));
    refresh_due=false;
    ws_state=WS_STATE_PRECONNECTED;
    CHECK(handle_preconnection_event(EV_WS_PRECONNECT_READY));
    CHECK(prepared_tasks==1 && activations==1 && dials==2 && reminder_task==&batch);
}
static void standby_checks(void)
{
    CHECK(tagged_ws_event(EV_WS_PRECONNECT_READY) && tagged_ws_event(EV_WS_SESSION_CLOSED));
    CHECK(tagged_ws_event(EV_WS_PRECONNECT_UNSUPPORTED) && !tagged_ws_event(EV_AUDIO_WAKE_DETECTED));
    reset(); readiness=DEVICE_WS_WAITING; poll_preconnection(clock_ms,false);
    CHECK(!dials && !preconnect_owned);
    readiness=DEVICE_WS_DISABLED; clock_ms+=500; poll_preconnection(clock_ms,false); CHECK(!dials);
    reset(); standby_ready(); pre_roll_active=true;
    start_conversation("wake word"); CHECK(pre_roll_active && hot_pre_roll && activations==1 && !starts);
    CHECK(!retry_at_ms && current_state==APP_STATE_WS_CONNECTING_WAKE && dials==1);
    CHECK(handle_preconnection_event(EV_WS_PRECONNECT_READY));
    CHECK(preconnect_deadline_ms && activations==1 && !starts);
    start_conversation("wake word"); CHECK(activations==1);
    ws_state=WS_STATE_CONNECTED;
    CHECK(!handle_preconnection_event(EV_WS_AUTH_OK) && !preconnect_owned);
    start_audio_session("warm auth"); CHECK(starts==1 && conversation_active && upload.running);
    poll_preconnection(clock_ms+900000,false); CHECK(dials==1);
    reset(); standby_transport(); pre_roll_active=true; start_conversation("wake word");
    CHECK(session_requested && pre_roll_active && !hot_pre_roll && !activations && !retry_at_ms);
    ws_state=WS_STATE_PRECONNECTED; CHECK(handle_preconnection_event(EV_WS_PRECONNECT_READY));
    CHECK(activations==1 && !starts && dials==1);
    reset(); standby_transport(); CHECK(handle_preconnection_event(EV_WS_PRECONNECT_UNSUPPORTED));
    CHECK(preconnect_unsupported && !preconnect_owned && !session_requested && !starts);
    poll_preconnection(clock_ms+600000,false); CHECK(dials==1);
    start_conversation("wake word"); CHECK(retry_at_ms && session_requested && !activations);
    reset(); standby_transport(); CHECK(handle_preconnection_event(EV_WS_PRECONNECT_REJECTED));
    CHECK(!preconnect_owned && !session_requested && !retry_at_ms);
    poll_preconnection(clock_ms+4900,false); CHECK(dials==1);
    clock_ms+=7000; poll_preconnection(clock_ms,false); CHECK(dials==2 && preconnect_owned);
    reset(); standby_transport(); CHECK(handle_preconnection_event(EV_WS_AUTH_FAILED));
    CHECK(preconnect_token_renewed && !preconnect_token_blocked && http_epoch==2 && !starts);
    readiness=DEVICE_WS_READY; clock_ms+=7000; poll_preconnection(clock_ms,false);
    CHECK(preconnect_owned && dials==2); CHECK(handle_preconnection_event(EV_WS_AUTH_FAILED));
    CHECK(preconnect_token_blocked && http_epoch==2); clock_ms+=100000;
    poll_preconnection(clock_ms,false); CHECK(!preconnect_owned && dials==2);
    ++http_epoch; clock_ms+=501; poll_preconnection(clock_ms,false); CHECK(dials==3 && preconnect_owned);
    reset(); standby_ready(); ++http_epoch; pre_roll_active=true; start_conversation("wake word");
    CHECK(!preconnect_owned && retry_at_ms && !activations && pre_roll_active && !hot_pre_roll);
    reset(); standby_ready(); readiness=DEVICE_WS_WAITING; clock_ms+=501;
    poll_preconnection(clock_ms,false); CHECK(!preconnect_owned && !session_requested);
    reset(); standby_ready(); valid_lease=false; start_conversation("touch");
    CHECK(!preconnect_owned && session_requested && retry_at_ms && !activations);
    reset(); standby_ready(); activation_result=ESP_FAIL; pre_roll_active=true; start_conversation("wake word");
    CHECK(!preconnect_owned && retry_at_ms && !starts && pre_roll_active && !hot_pre_roll);
    reset(); standby_ready(); available=&batch; poll_reminder();
    CHECK(reminder_task==&batch && activations==1 && prepared_tasks==1 && !starts && !hot_pre_roll);
    ws_state=WS_STATE_CONNECTED; CHECK(!handle_preconnection_event(EV_WS_AUTH_OK));
    start_audio_session("reminder"); CHECK(releases==1 && last_handled && starts==1);
    reset(); standby_transport(); start_conversation("touch"); clock_ms+=1500;
    poll_preconnection(clock_ms,false); CHECK(!preconnect_owned && retry_at_ms && !starts);
    reset(); standby_ready(); start_conversation("touch"); handle_talk_action("cancel activation");
    CHECK(!preconnect_owned && !session_requested && !retry_at_ms && !starts);
    reset(); standby_ready(); start_conversation("touch");
    CHECK(!handle_preconnection_event(EV_WS_SESSION_CLOSED));
    end_conversation("platform closed"); CHECK(!retry_at_ms && !preconnect_owned);
}
int main(void)
{
    silence_checks(); conversation_checks(); wake_pre_roll_checks(); task_checks(); touch_checks();
    if (DEVICE_WS_PRECONNECT_ENABLED) {
        standby_checks();
        standby_refresh_checks();
    }
    else {
        reset(); poll_preconnection(clock_ms, true);
        CHECK(!dials && !preconnect_owned && !session_requested);
        start_conversation("wake word");
        CHECK(!activations && session_requested && retry_at_ms == clock_ms);
    }
    printf("PASS: %u unified app session checks\n",checks); return 0;
}
