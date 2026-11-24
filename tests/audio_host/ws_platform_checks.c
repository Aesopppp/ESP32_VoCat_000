/* 直接运行生产 WS 接收/分发函数。transport、队列和播放器为可注入替身；
 * 覆盖无回合事件持续播放、可选通知、鉴权/连接代次、分片与缓存上限。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "../../main/event_bus.h"
#include "../../main/ws_service.h"
#include "../../main/proto_service.h"
#include "../../main/ws_rx_policy.h"
#define atomic_fetch_sub(p,v) ((*(p)-=(v))+(v))

static unsigned checks, events[128], decoded, started, queued;
static uint32_t generation=7, played_frames, rx_drop_count, clock_ms;
static uint32_t rx_audio_packets, rx_audio_last_ms, rx_audio_gap_max_ms, queue_wait_max_ms;
static uint32_t rx_packet_sequence, speech_sequence;
static bool speech_sequence_seen;
static size_t rx_bytes;
static int state=WS_STATE_CONNECTED, decoded_samples=960, opcode;
static bool client_started, stop_flag, inject_disconnect, queue_fail;
static uint8_t *frame;
static size_t frame_size, received;
static char reminder_device_id[65], reminder_task_ids[PROTO_REMINDER_TASK_IDS_MAX][129];
static size_t reminder_task_count;
static char auth_device_id[65];
static bool standby_auth;
static unsigned preconnect_until_ms, preconnect_refresh_ms, random_value;
static bool auth_sent, terminal_notified;
static SemaphoreHandle_t playback_lock=(void*)1;
static QueueHandle_t rx_queue=(void*)2;
/* 单线程替身验证所有快照读写走短临界区，不模拟跨核调度。 */
static unsigned speech_lock, control_lock, critical_depth;
static bool bus_full, delay_control;
static event_t pending_control;
static ws_speech_status_t speech_status;
#define portENTER_CRITICAL(lock) do { CHECK(((lock)==&speech_lock || (lock)==&control_lock) && !critical_depth); ++critical_depth; } while(0)
#define portEXIT_CRITICAL(lock) do { CHECK(((lock)==&speech_lock || (lock)==&control_lock) && critical_depth==1); --critical_depth; } while(0)
typedef const char *esp_event_base_t;
typedef struct {
    int op_code, payload_len, data_len, payload_offset;
    const char *data_ptr;
} esp_websocket_event_data_t;
enum { WEBSOCKET_EVENT_BEFORE_CONNECT, WEBSOCKET_EVENT_CONNECTED,
       WEBSOCKET_EVENT_DISCONNECTED, WEBSOCKET_EVENT_CLOSED,
       WEBSOCKET_EVENT_ERROR, WEBSOCKET_EVENT_DATA };
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
BaseType_t event_bus_publish(event_type_t id,const void *tag,size_t len)
{
    CHECK((uint32_t)(uintptr_t)tag==generation && !len);
    if (bus_full) return pdFALSE;
    ++events[id]; return pdTRUE;
}
#define publish production_publish
unsigned esp_log_timestamp(void) { return clock_ms; }
#include "ws_control.inc"
#undef publish
static void publish(app_event_id_t id,uint32_t gen)
{
    production_publish(id,gen);
    event_t ev;
    if (!delay_control && ws_service_take_pending_event(&ev)) ++events[ev.type];
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t ticks) { (void)s; (void)ticks; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { (void)q; return queued; }
static BaseType_t xQueueSend(QueueHandle_t q, const void *packet, TickType_t ticks);
static void log_server_error(const proto_msg_t *m) { CHECK(m->evt==PROTO_EVT_ERROR); }
static void log_connection_memory(const char *phase) { CHECK(phase!=NULL); }
static void log_transport_error(const char *phase,const void *event) { CHECK(phase && event); }
static bool audio_out_stop_requested(void) { return stop_flag; }
static void audio_out_set_streaming(bool active) { CHECK(active); ++started; }
static int audio_out_play_opus(const uint8_t *data,size_t len)
{
    CHECK(data!=NULL && len!=0);
    if (inject_disconnect) { state=WS_STATE_DISCONNECTED; stop_flag=true; return -1; }
    ++decoded; return decoded_samples;
}
uint32_t esp_random(void) { return random_value; }
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
#include "ws_platform.inc"
static rx_frame_t pending[64];
static unsigned pending_count;
static BaseType_t xQueueSend(QueueHandle_t q,const void *packet,TickType_t ticks)
{
    CHECK(q==rx_queue && ticks==0);
    if (queue_fail) return pdFALSE;
    CHECK(pending_count<64); pending[pending_count++]=*(const rx_frame_t*)packet;
    return pdTRUE;
}
static void drain_queue(void)
{
    for(unsigned i=0;i<pending_count;++i) {
        handle_frame(&pending[i]); release_rx_frame(pending[i].data,pending[i].len);
    }
    pending_count=0;
}
static rx_frame_t packet(const char *text,int op)
{
    rx_frame_t p={.data=(uint8_t*)text,.len=strlen(text),.opcode=op,.generation=generation};
    observe_rx_packet(&p); return p;
}
static void control(const char *text) { rx_frame_t p=packet(text,1); handle_frame(&p); }
static void audio(void) { rx_frame_t p=packet("opus",2); handle_frame(&p); }
static void feed(int op,const char *text,int total,int offset,int length)
{
    esp_websocket_event_data_t event={.op_code=op,.data_ptr=text,.payload_len=total,
                                     .payload_offset=offset,.data_len=length};
    on_ws(NULL,NULL,WEBSOCKET_EVENT_DATA,&event);
}
static void reset(void)
{
    drain_queue(); release_rx_frame(frame,frame_size); frame=NULL; frame_size=received=0;
    memset(events,0,sizeof(events)); decoded=started=queued=0;
    rx_audio_packets=rx_audio_last_ms=rx_audio_gap_max_ms=queue_wait_max_ms=rx_drop_count=0;
    CHECK(rx_bytes==0);
    state=WS_STATE_CONNECTED; stop_flag=client_started=inject_disconnect=queue_fail=false;
    played_frames=0; decoded_samples=960; clock_ms=1000; reminder_task_count=0;
    auth_sent=true; standby_auth=terminal_notified=false; preconnect_until_ms=preconnect_refresh_ms=0;
    random_value=1; auth_device_id[0]=0;
    reset_speech_status();
    memset(reminder_device_id,0,sizeof(reminder_device_id));
    memset(reminder_task_ids,0,sizeof(reminder_task_ids));
}
static void continuous_checks(void)
{
    reset();
    /* 无 speech_stopped/response.created 也必须立即向播放器交付。 */
    for(unsigned i=0;i<50;++i) audio();
    CHECK(decoded==50 && played_frames==50);
    const char *notices[]={
        "{\"type\":\"input_audio_buffer.speech_started\"}",
        "{\"type\":\"input_audio_buffer.speech_stopped\"}",
        "{\"type\":\"response.audio.done\"}",
        "{\"type\":\"response.output_audio.done\"}",
        "{\"type\":\"response.done\",\"response\":{\"status\":\"completed\"}}",
        "{\"type\":\"response.done\",\"response\":{\"status\":\"cancelled\"}}",
        "{\"type\":\"response.done\",\"response\":{\"status\":\"failed\"}}",
        "{\"type\":\"response.cancelled\"}",
        "{\"type\":\"response.created\"}",
        "{\"type\":\"response.output_item.done\",\"item\":{\"type\":\"function_call\"}}",
        "{\"type\":\"response.function_call_arguments.done\"}",
        "{\"type\":\"conversation.transcription\",\"who\":\"user\"}",
        /* 可选元数据不再按旧回合协议强校验。 */
        "{\"type\":\"response.done\",\"response\":\"gateway-specific\",\"status\":null}"
    };
    for(unsigned i=0;i<sizeof(notices)/sizeof(notices[0]);++i) {
        unsigned before=decoded;
        rx_frame_t queued_audio=packet("queued-old-audio",2);
        control(notices[i]); handle_frame(&queued_audio); audio();
        CHECK(decoded==before+2 && state==WS_STATE_CONNECTED && !stop_flag);
    }
    CHECK(!events[EV_WS_RESPONSE_DONE] && !events[EV_WS_RESPONSE_INTERRUPTED]);
    CHECK(!events[EV_WS_SPEECH_STARTED] && !events[EV_WS_SPEECH_STOPPED]);
    CHECK(!events[EV_WS_ERROR] && !events[EV_WS_SERVER_ERROR]);
    clock_ms+=3600000; audio(); CHECK(state==WS_STATE_CONNECTED);
    clock_ms=UINT32_MAX-10; audio(); clock_ms=20; audio();
    CHECK(rx_audio_packets==decoded);
}
static void session_checks(void)
{
    reset(); state=WS_STATE_AUTHING;
    audio(); CHECK(decoded==0);
    control("{\"type\":\"input_audio_buffer.speech_stopped\"}"); audio(); CHECK(decoded==0);
    control("{\"type\":\"__proxy_connected__\"}"); audio();
    CHECK(state==WS_STATE_CONNECTED && decoded==1 && events[EV_WS_AUTH_OK]==1);
    control("{\"type\":\"__proxy_connected__\"}"); CHECK(events[EV_WS_AUTH_OK]==1);
    rx_frame_t old=packet("stale",2); ++generation; handle_frame(&old); CHECK(decoded==1);
    old=packet("{\"type\":\"error\",\"code\":5000}",1); ++generation; handle_frame(&old);
    CHECK(!events[EV_WS_SERVER_ERROR]);
    state=WS_STATE_DISCONNECTED; audio(); CHECK(decoded==1);
    reset(); control("{\"type\":\"error\",\"code\":5000}"); audio();
    CHECK(events[EV_WS_SERVER_ERROR]==1 && state==WS_STATE_ERROR && decoded==0);
    reset(); control("{\"type\":\"error\",\"code\":2000}"); audio();
    CHECK(events[EV_WS_AUTH_FAILED]==1 && state==WS_STATE_ERROR && decoded==0);
    reset(); state=WS_STATE_AUTHING; control("{bad json}");
    CHECK(events[EV_WS_AUTH_FAILED]==1 && state==WS_STATE_ERROR);
    reset(); control("{bad json}"); audio(); CHECK(state==WS_STATE_CONNECTED && decoded==1);
    reset(); decoded_samples=-1; audio(); CHECK(events[EV_WS_SERVER_ERROR]==1);
    reset(); inject_disconnect=true; audio(); CHECK(!events[EV_WS_SERVER_ERROR]);
}
static void speech_checks(void)
{
    const char *start="{\"type\":\"input_audio_buffer.speech_started\"}";
    const char *stop="{\"type\":\"input_audio_buffer.speech_stopped\"}";
    ws_speech_status_t status;
    reset(); ws_service_get_speech_status(&status);
    CHECK(!status.speaking && !status.stopped_ms);
    ws_service_get_speech_status(NULL); CHECK(!critical_depth);
    control(stop); ws_service_get_speech_status(&status);
    CHECK(!status.speaking && status.stopped_ms==clock_ms); /* 首次 stop 也重启检测。 */
    control(start); ws_service_get_speech_status(&status);
    CHECK(status.speaking && status.stopped_ms==clock_ms);
    clock_ms+=45000; audio(); ws_service_get_speech_status(&status);
    CHECK(status.speaking && decoded==1 && !stop_flag); /* 长讲话、全双工播放。 */
    control(start); control("{\"type\":\"conversation.item.input_audio_transcription.completed\"}");
    control("{\"type\":\"response.done\",\"response\":{\"status\":\"completed\"}}");
    ws_service_get_speech_status(&status); CHECK(status.speaking);
    control(stop); ws_service_get_speech_status(&status);
    CHECK(!status.speaking && status.stopped_ms==46000);
    clock_ms+=10000; control(stop); ws_service_get_speech_status(&status);
    CHECK(!status.speaking && status.stopped_ms==46000); /* 重复 stop 幂等。 */
    control("{\"type\":\"conversation.item.input_audio_transcription.started\"}");
    ws_service_get_speech_status(&status); CHECK(!status.speaking && status.stopped_ms==46000);
    rx_frame_t stale=packet(start,1); ++generation; handle_frame(&stale);
    ws_service_get_speech_status(&status); CHECK(!status.speaking);
    CHECK(proto_parse_priority_event(start)==PROTO_EVT_SPEECH_STARTED);
    CHECK(!observe_speech_event(PROTO_EVT_SPEECH_STARTED,&stale)); /* 解析后发生断连，再核对代次。 */
    ws_service_get_speech_status(&status); CHECK(!status.speaking);
    control(start); stale=packet(stop,1); ++generation; handle_frame(&stale);
    ws_service_get_speech_status(&status); CHECK(status.speaking);
    esp_websocket_event_data_t event={0};
    on_ws(NULL,NULL,WEBSOCKET_EVENT_DISCONNECTED,&event);
    ws_service_get_speech_status(&status); CHECK(!status.speaking && !status.stopped_ms);
    handle_frame(&stale); ws_service_get_speech_status(&status);
    CHECK(!status.speaking && !status.stopped_ms);
    state=WS_STATE_AUTHING; control(start); ws_service_get_speech_status(&status);
    CHECK(!status.speaking && !status.stopped_ms); /* 认证前的 VAD 不可信。 */
    control("{\"type\":\"__proxy_connected__\"}"); control(start);
    clock_ms=0; control(stop); ws_service_get_speech_status(&status);
    CHECK(!status.speaking && status.stopped_ms==UINT32_MAX); /* 0 哨兵与时钟回绕。 */
    control(start); state=WS_STATE_CONNECTING; on_ws(NULL,NULL,WEBSOCKET_EVENT_CONNECTED,&event);
    ws_service_get_speech_status(&status); CHECK(!status.speaking && !status.stopped_ms);
    control("{\"type\":\"__proxy_connected__\"}"); control(start);
    on_ws(NULL,NULL,WEBSOCKET_EVENT_ERROR,&event);
    ws_service_get_speech_status(&status); CHECK(!status.speaking && !status.stopped_ms);
    reset(); control(start); control("{\"type\":\"error\",\"code\":5000}");
    ws_service_get_speech_status(&status); CHECK(!status.speaking && !status.stopped_ms);
    CHECK(!critical_depth);
}
static void speech_fast_path_checks(void)
{
    const char *start="{\"type\":\"input_audio_buffer.speech_started\"}";
    const char *stop="{\"type\":\"input_audio_buffer.speech_stopped\"}";
    const char *auth="{\"type\":\"__proxy_connected__\"}";
    ws_speech_status_t status;
    reset(); feed(2,"opus",4,0,4);
    clock_ms=30999;
    feed(1,start,(int)strlen(start),0,(int)strlen(start));
    ws_service_get_speech_status(&status);
    CHECK(status.speaking && pending_count==1 && decoded==0); /* 不等积压音频出队。 */
    clock_ms+=45000;
    feed(1,stop,(int)strlen(stop),0,(int)strlen(stop));
    ws_service_get_speech_status(&status);
    CHECK(!status.speaking && status.stopped_ms==clock_ms && pending_count==1);
    drain_queue(); CHECK(decoded==1 && rx_bytes==0 && !stop_flag);
    reset(); state=WS_STATE_AUTHING;
    feed(1,auth,(int)strlen(auth),0,(int)strlen(auth));
    feed(1,start,(int)strlen(start),0,(int)strlen(start));
    feed(1,stop,(int)strlen(stop),0,(int)strlen(stop));
    ws_service_get_speech_status(&status); CHECK(!status.speaking && pending_count==0);
    rx_frame_t delayed=packet(stop,1);
    feed(1,start,(int)strlen(start),0,(int)strlen(start));
    handle_frame(&delayed);
    ws_service_get_speech_status(&status); CHECK(status.speaking && pending_count==0);
    drain_queue(); ws_service_get_speech_status(&status);
    CHECK(status.speaking && status.stopped_ms==clock_ms && rx_bytes==0);
    reset(); state=WS_STATE_AUTHING;
    feed(1,auth,(int)strlen(auth),0,(int)strlen(auth));
    feed(1,start,(int)strlen(start),0,(int)strlen(start));
    drain_queue(); ws_service_get_speech_status(&status);
    CHECK(status.speaking); /* 无更新事件时，auth 后队列中的 start 仍生效。 */
    reset(); state=WS_STATE_AUTHING;
    feed(1,auth,(int)strlen(auth),0,(int)strlen(auth));
    feed(1,start,(int)strlen(start),0,(int)strlen(start));
    clock_ms+=45000;
    feed(1,stop,(int)strlen(stop),0,(int)strlen(stop));
    drain_queue(); ws_service_get_speech_status(&status);
    CHECK(!status.speaking && status.stopped_ms==clock_ms); /* start未出队，stop仍须续期。 */
    CHECK(proto_parse_priority_event("{\"type\":\"input_audio_buffer.speech_started\",\"type\":\"x\"}")==PROTO_EVT_UNKNOWN);
    CHECK(proto_parse_priority_event("{\"type\":\"conversation.item.input_audio_transcription.started\"}")==PROTO_EVT_UNKNOWN);
    CHECK(proto_parse_priority_event("{\"type\":\"input_audio_buffer.speech_started\"} trailing")==PROTO_EVT_UNKNOWN);
    CHECK(proto_parse_priority_event("{\"type\":\"input_audio_buffer.speech_started\\u0000bad\"}")==PROTO_EVT_UNKNOWN);
    CHECK(proto_parse_priority_event(NULL)==PROTO_EVT_UNKNOWN);
    reset(); rx_packet_sequence=UINT32_MAX-1;
    feed(1,start,(int)strlen(start),0,(int)strlen(start));
    feed(1,stop,(int)strlen(stop),0,(int)strlen(stop));
    ws_service_get_speech_status(&status);
    CHECK(!status.speaking && status.stopped_ms==clock_ms); /* 包序号回绕。 */
}
static void reminder_checks(void)
{
    reset(); state=WS_STATE_DISCONNECTED;
    const char *ids[]={"42","0043"}, *invalid[]={"42","task-43"};
    CHECK(ws_service_prepare_reminder("dev_test",invalid,2)==ESP_ERR_INVALID_ARG);
    CHECK(ws_service_prepare_reminder("dev_test",NULL,1)==ESP_ERR_INVALID_ARG);
    CHECK(ws_service_prepare_reminder("dev_test",ids,2)==ESP_OK);
    CHECK(reminder_task_count==2 && !strcmp(reminder_task_ids[1],"0043"));
    CHECK(ws_service_prepare_reminder("dev_test",ids,2)==ESP_ERR_INVALID_STATE);
    state=WS_STATE_AUTHING; audio(); CHECK(decoded==0);
    control("{\"type\":\"__proxy_connected__\"}"); audio();
    control("{\"type\":\"response.audio.done\"}");
    control("{\"type\":\"input_audio_buffer.speech_started\"}"); audio();
    CHECK(decoded==2 && !stop_flag && state==WS_STATE_CONNECTED);
    CHECK(!events[EV_WS_RESPONSE_DONE] && !events[EV_WS_SERVER_ERROR]);
}
static void preconnect_checks(void)
{
    const char *ready="{\"type\":\"__preconnect_ready__\",\"expires_in\":600}";
    const char *proxy="{\"type\":\"__proxy_connected__\",\"audio_format\":{\"encoding\":\"opus\",\"sample_rate\":16000,\"channels\":1}}";
    const char *pcm="{\"type\":\"__proxy_connected__\",\"audio_format\":{\"encoding\":\"pcm16\",\"sample_rate\":16000,\"channels\":1}}";
    const char *missing="{\"type\":\"__proxy_connected__\"}";
    const char *closed="{\"type\":\"session.closed\",\"reason\":\"server_ended\"}";
    esp_websocket_event_data_t event={0};
    reset(); state=WS_STATE_CONNECTING; strcpy(auth_device_id,"dev-1"); standby_auth=true;
    on_ws(NULL,NULL,WEBSOCKET_EVENT_CONNECTED,&event);
    CHECK(state==WS_STATE_PREAUTHING && events[EV_WS_CONNECTED]==1);
    auth_sent=false; control(ready); audio();
    CHECK(state==WS_STATE_PREAUTHING && !decoded && !events[EV_WS_AUTH_OK]);
    auth_sent=true; clock_ms=UINT32_MAX-100;
    feed(1,ready,strlen(ready),0,strlen(ready)); audio(); control(ready);
    CHECK(state==WS_STATE_PRECONNECTED && events[EV_WS_PRECONNECT_READY]==1 && !decoded);
    clock_ms+=599999; CHECK(ws_service_preconnect_valid()); ++clock_ms;
    CHECK(!ws_service_preconnect_valid());
    CHECK(ws_service_prepare_reminder("dev-1",(const char *const[]){"1"},1)==ESP_ERR_INVALID_STATE);
    --clock_ms;
    CHECK(ws_service_prepare_reminder("dev-other",(const char *const[]){"1"},1)==ESP_ERR_INVALID_STATE);
    CHECK(ws_service_prepare_reminder("dev-1",(const char *const[]){"13"},1)==ESP_OK);
    state=WS_STATE_ACTIVATING; audio(); CHECK(!decoded);
    feed(1,proxy,strlen(proxy),0,strlen(proxy)); audio(); control(proxy);
    CHECK(decoded==1 && state==WS_STATE_CONNECTED && events[EV_WS_AUTH_OK]==1);
    feed(1,closed,strlen(closed),0,strlen(closed));
    on_ws(NULL,NULL,WEBSOCKET_EVENT_DISCONNECTED,&event);
    CHECK(events[EV_WS_SESSION_CLOSED]==1 && !events[EV_WS_DISCONNECTED]);
    reset(); state=WS_STATE_PREAUTHING;
    feed(1,missing,strlen(missing),0,strlen(missing));
    on_ws(NULL,NULL,WEBSOCKET_EVENT_DISCONNECTED,&event); audio();
    CHECK(events[EV_WS_PRECONNECT_UNSUPPORTED]==1 && !events[EV_WS_AUTH_OK] && !events[EV_WS_DISCONNECTED] && !decoded);
    reset(); state=WS_STATE_PREAUTHING; control("{\"type\":\"error\",\"error\":{\"code\":4002}}");
    CHECK(events[EV_WS_PRECONNECT_REJECTED]==1 && !events[EV_WS_AUTH_FAILED]);
    reset(); state=WS_STATE_PREAUTHING; control("{\"type\":\"error\",\"error\":{\"code\":\"token_expired\"}}");
    CHECK(events[EV_WS_AUTH_FAILED]==1);
    reset(); state=WS_STATE_PREAUTHING; control("{\"type\":\"__preconnect_ready__\",\"expires_in\":0.5}");
    CHECK(events[EV_WS_PRECONNECT_REJECTED]==1 && !events[EV_WS_PRECONNECT_READY]);
    reset(); state=WS_STATE_ACTIVATING; control(missing); CHECK(events[EV_WS_FORMAT_ERROR]==1);
    reset(); state=WS_STATE_ACTIVATING; control(pcm); audio(); CHECK(events[EV_WS_FORMAT_ERROR]==1 && !decoded);
    reset(); state=WS_STATE_AUTHING; control(pcm); audio(); CHECK(events[EV_WS_FORMAT_ERROR]==1 && !decoded);
    reset(); state=WS_STATE_AUTHING; strcpy(auth_device_id,"dev-1"); control(missing);
    CHECK(events[EV_WS_FORMAT_ERROR]==1 && !events[EV_WS_AUTH_OK]);
}
static void preconnect_refresh_checks(void)
{
    const uint32_t start = UINT32_MAX - 100u;
    const uint32_t seeds[] = {0, 9999, UINT32_MAX};
    uint32_t first_delay = 0;
    bool staggered = false;
    for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i) {
        reset();
        clock_ms = start;
        state = WS_STATE_PREAUTHING;
        random_value = seeds[i];
        control("{\"type\":\"__preconnect_ready__\",\"expires_in\":600}");
        uint32_t delay = preconnect_refresh_ms - start;
        CHECK(delay >= 500000 && delay <= 540000);
        if (i == 0) first_delay = delay;
        else if (delay != first_delay) staggered = true;
        clock_ms = start + delay - 1;
        CHECK(!ws_service_preconnect_refresh_due() && ws_service_preconnect_valid());
        ++clock_ms;
        CHECK(ws_service_preconnect_refresh_due() && ws_service_preconnect_valid());
        clock_ms = start + 599999;
        CHECK(ws_service_preconnect_valid());
        ++clock_ms;
        CHECK(!ws_service_preconnect_valid());
        state = WS_STATE_ACTIVATING;
        CHECK(!ws_service_preconnect_refresh_due());
    }
    CHECK(staggered);

    reset();
    state = WS_STATE_PREAUTHING;
    uint32_t received_at = clock_ms;
    control("{\"type\":\"__preconnect_ready__\",\"expires_in\":1}");
    CHECK(!ws_service_preconnect_refresh_due());
    uint32_t short_delay = preconnect_refresh_ms - received_at;
    CHECK(short_delay > 0 && short_delay < 1000);
    clock_ms += short_delay;
    CHECK(ws_service_preconnect_refresh_due() && ws_service_preconnect_valid());
}
static void framing_checks(void)
{
    reset(); state=WS_STATE_AUTHING;
    const char *auth="{\"type\":\"__proxy_connected__\"}";
    feed(2,"early",5,0,5); feed(1,auth,(int)strlen(auth),0,5);
    CHECK(pending_count==0 && frame && received==5);
    feed(1,auth+5,(int)strlen(auth),5,(int)strlen(auth)-5);
    feed(2,"op",4,0,2); feed(2,"us",4,2,2);
    CHECK(pending_count==1); drain_queue();
    CHECK(events[EV_WS_AUTH_OK]==1 && decoded==1 && rx_bytes==0);
    feed(2,"first",5,0,2); feed(2,"new",3,0,3); drain_queue();
    CHECK(decoded==2 && rx_bytes==0);
    feed(2,"bad-offset",10,0,2); feed(2,"wrong",10,3,5);
    CHECK(pending_count==0 && received==2);
    feed(2,"ok",2,0,2); drain_queue(); CHECK(decoded==3 && rx_bytes==0);
    unsigned before=decoded;
    feed(2,"x",65537,0,1); feed(2,"x",1,-1,1); feed(2,"x",1,0,-1);
    drain_queue(); CHECK(decoded==before && rx_bytes==0);
    queued=WS_RX_FRAMES-WS_RX_CONTROL_RESERVE;
    feed(2,"full",4,0,4); CHECK(rx_drop_count==1 && pending_count==0 && rx_bytes==0);
    feed(1,auth,(int)strlen(auth),0,(int)strlen(auth)); drain_queue(); CHECK(rx_bytes==0);
    queued=0; queue_fail=true;
    feed(2,"full",4,0,4); CHECK(rx_drop_count==2 && rx_bytes==0);
    feed(1,auth,(int)strlen(auth),0,(int)strlen(auth)); CHECK(events[EV_WS_ERROR]==1 && rx_bytes==0);
    queue_fail=false;
    feed(2,"split",5,0,2); CHECK(rx_bytes==6);
    esp_websocket_event_data_t event={0};
    on_ws(NULL,NULL,WEBSOCKET_EVENT_DISCONNECTED,&event);
    CHECK(state==WS_STATE_DISCONNECTED && rx_bytes==0 && frame==NULL);
    state=WS_STATE_CONNECTED; feed(2,"old",3,0,3); ++generation;
    drain_queue(); CHECK(decoded==before && rx_bytes==0);
}
int main(void)
{
    continuous_checks(); session_checks(); speech_checks(); speech_fast_path_checks();
    reminder_checks(); preconnect_checks(); preconnect_refresh_checks(); framing_checks(); reset();
    event_t ev;
    bus_full=delay_control=true;
    publish(EV_WS_CONNECTED,generation);
    CHECK(ws_service_take_pending_event(&ev) && ev.type==EV_WS_CONNECTED);
    publish(EV_WS_PRECONNECT_READY,generation); publish(EV_WS_DISCONNECTED,generation);
    CHECK(ws_service_take_pending_event(&ev) && ev.type==EV_WS_DISCONNECTED);
    publish(EV_WS_AUTH_OK,generation);
    const char closed[]="{\"type\":\"session.closed\"}";
    feed(1,closed,sizeof(closed)-1,0,sizeof(closed)-1);
    publish(EV_WS_ERROR,generation);
    CHECK(ws_service_take_pending_event(&ev) && ev.type==EV_WS_SESSION_CLOSED);
    CHECK(!ws_service_take_pending_event(&ev));
    publish(EV_WS_CONNECTED,generation); ++generation;
    CHECK(!ws_service_take_pending_event(&ev));
    printf("PASS: %u production WS continuous-duplex checks\n",checks); return 0;
}
