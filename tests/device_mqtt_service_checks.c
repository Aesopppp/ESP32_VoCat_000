/* 运行实际 service 函数；只替换 RTOS、MQTT、NVS 和时钟，不复制生产策略。 */
#include "../main/device_mqtt_service.h"
#include "../main/device_mqtt_storage.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_ERR_NOT_FOUND 5
#define ESP_FAIL (-1)
#define ESP_EVENT_ANY_ID (-1)
#ifndef TEST_NO_CA
#define DEVICE_MQTT_HAS_CA 1
#endif
#ifndef DEVICE_MQTT_FORWARD_TASK_TO_WS
#define DEVICE_MQTT_FORWARD_TASK_TO_WS 1
#endif
#define __has_include(x) 0
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_FOUND 20
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 21
#define ESP_ERR_INVALID_SIZE 22
typedef bool atomic_bool;
typedef unsigned atomic_uint;
#define atomic_load(p) (*(p))
#define atomic_store(p,v) ((*(p))=(v))
#define atomic_fetch_add(p,v) ((*(p)+=(v))-(v))
static bool atomic_exchange(atomic_bool *p, bool v) { bool old=*p; *p=v; return old; }
typedef int BaseType_t;
typedef int SemaphoreHandle_t;
typedef int esp_event_base_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(n) (n)
#define configASSERT(x) CHECK(x)
typedef struct { unsigned size, capacity, count; unsigned char *data; } *QueueHandle_t;
static QueueHandle_t xQueueCreate(unsigned n, unsigned size) {
    QueueHandle_t q = calloc(1, sizeof(*q)); q->size=size; q->capacity=n; q->data=calloc(n,size); return q;
}
static int xQueueSend(QueueHandle_t q, const void *item, int wait) {
    (void)wait; if (q->count==q->capacity) return 0;
    memcpy(q->data+q->count++*q->size,item,q->size); return 1;
}
static int xQueueReceive(QueueHandle_t q, void *item, int wait) {
    (void)wait; if (!q->count) return 0; memcpy(item,q->data,q->size);
    --q->count; memmove(q->data,q->data+q->size,q->count*q->size); return 1;
}
static void xQueueReset(QueueHandle_t q) { q->count=0; }
static void vQueueDelete(QueueHandle_t q) { free(q->data); free(q); }
static int xSemaphoreCreateMutex(void) { return 1; }
static int xSemaphoreTake(int s,int t) { (void)s; (void)t; return 1; }
static void xSemaphoreGive(int s) { (void)s; }
static void vSemaphoreDelete(int s) { (void)s; }
static void *worker_argument;
static int xTaskCreatePinnedToCore(void (*fn)(void *),const char *name,int stack,void *arg,int priority,void *task,int core) {
    (void)fn;(void)name;(void)stack;(void)priority;(void)task;(void)core; worker_argument=arg; return 1;
}
static void vTaskDelay(int ms) { (void)ms; }
static int64_t fake_now = 1000;
enum { WIFI_SETUP_MQTT = 4 };
static void wifi_service_set_setup_active(int owner, bool active) { (void)owner; (void)active; }
static int64_t esp_timer_get_time(void) { return fake_now * 1000; }
static uint32_t esp_random(void) { return 20; }
static bool wifi_service_get_ip_str(char *p,size_t n) { snprintf(p,n,"192.0.2.1"); return true; }
static int wifi_service_get_rssi(void) { return -50; }
typedef struct { const char *version; } mock_description_t;
static const mock_description_t *esp_app_get_description(void) { static mock_description_t d={"test-version"}; return &d; }
typedef unsigned nvs_handle_t;
static bool fail_commit;
static int fail_commit_slot = -1;
static unsigned commits;
static unsigned space_failures;
/* 近似 NVS entry 占用，额外预留整份新 blob 后才能替换旧值。
 * 模拟其他配置占用后的可用预算，不宣称复现真实 Flash 页分配/GC。 */
static size_t entry_budget = 10000;
typedef struct { size_t available_entries; } nvs_stats_t;
/* 每个 NVS 键有独立 durable/staged 副本，commit 失败不改变持久化内容。 */
static struct { bool present; size_t length; unsigned char bytes[8192]; } blobs[2];
static unsigned char staged[8192];
static size_t staged_length;
static int staged_slot;
static int blob_slot(const char *key) {
    if(!strcmp(key,"played_v1")) return 0;
    CHECK(!strcmp(key,"pending_v1")); return 1;
}
static size_t blob_entries(size_t n) { return (n+31)/32 + 2*((n+3999)/4000) + 1; }
static size_t used_entries(void) {
    size_t used=0;
    for(unsigned i=0;i<2;++i) if(blobs[i].present) used+=blob_entries(blobs[i].length);
    return used;
}
static int nvs_get_stats(const char *partition,nvs_stats_t *stats) {
    (void)partition; size_t used=used_entries();
    stats->available_entries=used<entry_budget ? entry_budget-used : 0; return ESP_OK;
}
static int nvs_open(const char *name,int mode,nvs_handle_t *h) { (void)mode; CHECK(!strcmp(name,"device_tasks")); *h=1; return ESP_OK; }
static int nvs_get_blob(nvs_handle_t h,const char *key,void *p,size_t *n) {
    (void)h; int slot=blob_slot(key); if(!blobs[slot].present) return ESP_ERR_NVS_NOT_FOUND;
    if(p) { CHECK(*n>=blobs[slot].length); memcpy(p,blobs[slot].bytes,blobs[slot].length); }
    *n=blobs[slot].length; return ESP_OK;
}
static int nvs_set_blob(nvs_handle_t h,const char *key,const void *p,size_t n) {
    (void)h; staged_slot=blob_slot(key); CHECK(n<=sizeof(staged));
    if(used_entries()+blob_entries(n)>entry_budget) { ++space_failures; return ESP_ERR_NVS_NOT_ENOUGH_SPACE; }
    memcpy(staged,p,n); staged_length=n; return ESP_OK;
}
static int nvs_commit(nvs_handle_t h) {
    (void)h; if(fail_commit || staged_slot==fail_commit_slot) return ESP_FAIL;
    memcpy(blobs[staged_slot].bytes,staged,staged_length);
    blobs[staged_slot].length=staged_length; blobs[staged_slot].present=true; ++commits; return ESP_OK;
}
static void nvs_close(nvs_handle_t h) { (void)h; }

enum { MQTT_EVENT_CONNECTED=1, MQTT_EVENT_DISCONNECTED, MQTT_EVENT_PUBLISHED, MQTT_EVENT_SUBSCRIBED,
       MQTT_EVENT_ERROR, MQTT_EVENT_DATA };
enum { MQTT_ERROR_TYPE_CONNECTION_REFUSED=2, MQTT_PROTOCOL_V_3_1_1=4 };
typedef struct { int error_type, connect_return_code, esp_tls_stack_err, esp_tls_cert_verify_flags; } mock_error_t;
typedef struct { int retain,qos,topic_len,total_data_len,current_data_offset,data_len,msg_id;
    char *topic,*data; mock_error_t *error_handle; } mock_event_t, *esp_mqtt_event_handle_t;
typedef void *esp_mqtt_client_handle_t;
typedef struct {
    struct { struct { const char *uri; } address;
             struct { const char *certificate; bool skip_cert_common_name_check; } verification; } broker;
    struct { const char *client_id,*username; struct { const char *password; } authentication; } credentials;
    struct { int protocol_ver,keepalive; bool disable_clean_session; } session;
    struct { bool disable_auto_reconnect; int timeout_ms; } network;
    struct { int priority,stack_size; } task;
    struct { int size,out_size; } buffer;
    struct { int limit; } outbox;
} esp_mqtt_client_config_t;
static unsigned starts, stops, subscriptions, publishes, acks;
static int next_id=10;
static esp_mqtt_client_config_t observed;
static char last_topic[129], last_payload[1600];
static esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config) { observed=*config; return (void *)1; }
static int esp_mqtt_client_register_event(void *client,int id,void (*cb)(void*,esp_event_base_t,int32_t,void*),void *arg) {
    (void)client;(void)id;(void)cb;(void)arg; return ESP_OK;
}
static int esp_mqtt_client_start(void *c) { CHECK(c!=NULL); ++starts; return ESP_OK; }
static int esp_mqtt_client_stop(void *c) { CHECK(c!=NULL); ++stops; return ESP_OK; }
static int esp_mqtt_client_destroy(void *c) { CHECK(c!=NULL); return ESP_OK; }
static int esp_mqtt_client_subscribe(void *c,const char *topic,int qos) {
    CHECK(c!=NULL && qos==1 && !strchr(topic,'#') && !strchr(topic,'+')); ++subscriptions; return next_id++;
}
static int esp_mqtt_client_enqueue(void *c,const char *topic,const char *payload,int len,int qos,int retain,bool store) {
    (void)len; CHECK(c!=NULL && qos==1 && retain==0 && store); ++publishes;
    snprintf(last_topic,sizeof(last_topic),"%s",topic); snprintf(last_payload,sizeof(last_payload),"%s",payload);
    if (strstr(topic,"/ack")) ++acks;
    return next_id++;
}

#include "device_mqtt_service.inc"

static mqtt_worker_t *worker;
static mqtt_settings_t scratch;
static device_handshake_response_t config;
static void step(void) { mqtt_worker_step(worker,&scratch); }
static void signal_event(int id, int msg_id, int code) {
    mqtt_notice_t event={.id=id,.msg_id=msg_id,.code=code};
    CHECK(xQueueSend(s_control,&event,0)); step();
}
static void receive_task(const char *id) {
    char payload[400];
    snprintf(payload,sizeof(payload),"{\"task_id\":\"%s\",\"kind\":\"reminder\",\"content\":\"原文播报\",\"fire_at\":\"2020-01-01T01:02:03.000Z\"}",id);
    mock_event_t e={.topic=config.mqtt.subscribe_topic,.topic_len=(int)strlen(config.mqtt.subscribe_topic),
        .qos=1,.data=payload,.data_len=(int)strlen(payload),.total_data_len=(int)strlen(payload)};
    mqtt_event(worker,0,MQTT_EVENT_DATA,&e); step();
}
static void receive_fragmented_task(const char *payload) {
    size_t length=strlen(payload);
    for(size_t offset=0;offset<length;offset+=17) {
        size_t part=length-offset<17 ? length-offset : 17;
        /* 后续片段不带 topic，与 ESP-MQTT 的真实回调一致。 */
        mock_event_t e={.topic=offset ? NULL : config.mqtt.subscribe_topic,
            .topic_len=offset ? 0 : (int)strlen(config.mqtt.subscribe_topic),.qos=0,
            .data=(char *)payload+offset,.data_len=(int)part,.total_data_len=(int)length,
            .current_data_offset=(int)offset};
        mqtt_event(worker,0,MQTT_EVENT_DATA,&e); step();
        if(offset+part<length) CHECK(!worker->active && !worker->pending.count);
    }
}
static void connect_ok(void) {
    signal_event(MQTT_EVENT_CONNECTED,0,0);
    signal_event(MQTT_EVENT_SUBSCRIBED,worker->subscribe_id,1);
    CHECK(worker->subscribed);
    signal_event(MQTT_EVENT_PUBLISHED,worker->status_id,0);
}


static void storage_recovery_checks(void)
{
    mqtt_worker_t *w=calloc(1,sizeof(*w)), *boot=calloc(1,sizeof(*boot));
    w->settings=worker->settings; boot->settings=w->settings;
    w->storage_ready=true;
    memset(blobs,0,sizeof(blobs)); entry_budget=10000;
    pending_init(&w->pending,w->settings.config.username); w->pending.count=2;
    strcpy(w->pending.entries[0].id,"000021"); strcpy(w->pending.entries[1].id,"000022");
    w->pending.entries[1].received_at=1790000000;
    w->pending.entries[0].state=2; w->pending.entries[0].failure_pending=1;
    CHECK(save_blob("pending_v1",&w->pending,sizeof(w->pending))==ESP_OK);
    entry_budget=used_entries()+12;
    CHECK(save_blob("pending_v1",&w->pending,sizeof(w->pending))==ESP_ERR_NVS_NOT_ENOUGH_SPACE);
    CHECK(load_pending(w) && w->pending_dirty && w->pending.count==2);
    CHECK(!w->pending.entries[0].state && !w->pending.entries[0].failure_pending);
    fail_commit=true; persist_state(w); CHECK(w->pending_dirty);
    CHECK(blobs[1].length==sizeof(w->pending)); /* 迁移写失败仍能从旧记录重启。 */
    CHECK(load_pending(boot) && boot->pending.count==2);
    fail_commit=false; fake_now+=5000; persist_state(w);
    CHECK(!w->pending_dirty && !memcmp(blobs[1].bytes,"MQP2",4) && blobs[1].length<100);
    memset(&boot->pending,0,sizeof(boot->pending)); boot->pending_dirty=false;
    CHECK(load_pending(boot) && !boot->pending_dirty && boot->pending.count==2);
    CHECK(!strcmp(boot->pending.entries[0].id,"000021"));

    /* 通知消费保存失败：当前启动不重复交接；掉电可能恢复旧通知，由平台去重。 */
    w->pending.count=0; w->pending_dirty=true; fail_commit=true;
    w->persist_at=0; persist_state(w); CHECK(w->pending_dirty);
    CHECK(load_pending(boot) && boot->pending.count==2);
    fail_commit=false; fake_now+=5000; persist_state(w);
    CHECK(!w->pending_dirty && load_pending(boot) && !boot->pending.count);
    for(unsigned i=0;i<200;++i) {
        w->pending.count=1; snprintf(w->pending.entries[0].id,sizeof(w->pending.entries[0].id),"%u",i);
        w->pending_dirty=true; persist_state(w);
        CHECK(!w->pending_dirty && load_pending(boot) && boot->pending.count==1);
        w->pending.count=0; w->pending_dirty=true; persist_state(w);
        CHECK(!w->pending_dirty && load_pending(boot) && !boot->pending.count);
    }
    blobs[1].length=4; CHECK(!load_pending(boot));
    entry_budget=10000; free(w); free(boot);
}

static device_mqtt_delivery_t *claim(void)
{
    device_mqtt_service_set_dispatch_allowed(true); step();
    device_mqtt_delivery_t *tasks=device_mqtt_service_take_tasks();
    CHECK(tasks!=NULL);
    return tasks;
}

static void release(device_mqtt_delivery_t *tasks,bool handled)
{
    device_mqtt_service_set_dispatch_allowed(false);
    device_mqtt_service_release_tasks(tasks,handled); step();
}

int main(void)
{
#ifdef TEST_NO_CA
    CHECK(device_mqtt_service_init()==ESP_ERR_NOT_FOUND);
    device_mqtt_service_poll(true,1,&config);
    CHECK(!device_mqtt_service_voice_enabled() && !device_mqtt_service_take_tasks() && !starts);
    printf("Device MQTT missing-CA guard: %u checks passed\n",checks); return 0;
#endif
    strcpy(config.mqtt.endpoint,"mqtts://broker.example:8883");
    strcpy(config.mqtt.username,"dev-1");
    strcpy(config.mqtt.client_id,"issued-id");
    strcpy(config.mqtt.password,"test-only");
    strcpy(config.mqtt.publish_topic,"devices/dev-1/status");
    strcpy(config.mqtt.subscribe_topic,"devices/dev-1/task");
    CHECK(device_mqtt_service_init()==ESP_OK); worker=worker_argument;
    device_mqtt_service_poll(true,1,&config); step();
    CHECK(starts==1 && worker->storage_ready);
    CHECK(observed.session.protocol_ver==MQTT_PROTOCOL_V_3_1_1 && observed.session.keepalive==60);
    CHECK(!observed.session.disable_clean_session && observed.network.disable_auto_reconnect);
    CHECK(observed.broker.verification.certificate && !observed.broker.verification.skip_cert_common_name_check);
    CHECK(!strcmp(observed.credentials.client_id,"issued-id"));
    connect_ok(); CHECK(publishes==1 && subscriptions==1);
    CHECK(!strcmp(last_topic,config.mqtt.publish_topic));
    cJSON *status=cJSON_Parse(last_payload);
    CHECK(status && cJSON_GetArraySize(status)==4);
    CHECK(!strcmp(cJSON_GetObjectItem(status,"firmware_version")->valuestring,"test-version"));
    CHECK(!strcmp(cJSON_GetObjectItem(status,"board")->valuestring,"esp-vocat-v1.2"));
    CHECK(!strcmp(cJSON_GetObjectItem(status,"ip")->valuestring,"192.0.2.1"));
    CHECK(cJSON_GetObjectItem(status,"rssi")->valueint==-50);
    CHECK(!cJSON_GetObjectItem(status,"ts")); cJSON_Delete(status);
    fake_now+=59999; step(); CHECK(publishes==1);
    fake_now+=1; step(); CHECK(publishes==2);
    step(); CHECK(publishes==2);
    signal_event(MQTT_EVENT_PUBLISHED,worker->status_id,0);
    device_mqtt_service_poll(true,1,NULL); step(); CHECK(stops==0 && starts==1);

    const char *fragmented=" {\"task_id\":\"10\",\"kind\":\"reminder\","
        "\"content\":\"请按原文播报：\\\"午饭\\\"\\n时间到了\",\"fire_at\":\"2020-01-01T01:02:03.000Z\"} ";
    receive_fragmented_task(fragmented);
    CHECK(worker->pending.count==1 && !worker->active && blobs[1].present && acks==0);
    CHECK(!strcmp(worker->pending.entries[0].id,"10") && worker->pending.entries[0].received_at>0);
    receive_task("11"); receive_task("10");
    CHECK(worker->pending.count==3 && !worker->active); /* 重复业务交由平台处理。 */
    receive_task("task-12"); CHECK(worker->pending.count==3);
    mqtt_worker_t *reboot=calloc(1,sizeof(*reboot)); reboot->settings=worker->settings;
    CHECK(load_pending(reboot) && reboot->pending.count==3);
    CHECK(reboot->pending.entries[0].received_at==worker->pending.entries[0].received_at); free(reboot);
#if !DEVICE_MQTT_FORWARD_TASK_TO_WS
    device_mqtt_service_set_dispatch_allowed(true); step();
    CHECK(!device_mqtt_service_voice_enabled() && !device_mqtt_service_take_tasks() && acks==0);
    printf("Device MQTT disabled-voice guard: %u checks passed\n",checks); return 0;
#endif
    /* 已有会话可以立即领取；不需要等播放结束，服务不生成语音请求或播放ACK。 */
    device_mqtt_delivery_t *tasks=claim();
    CHECK(tasks->count==3 && !strcmp(tasks->task_ids[0],"10") && !strcmp(tasks->task_ids[2],"10"));
    CHECK(!device_mqtt_service_take_tasks());
    device_mqtt_service_set_dispatch_allowed(false);
    receive_task("10"); receive_task("12"); CHECK(worker->pending.count==5);
    fail_commit=true; release(tasks,true);
    CHECK(worker->pending_dirty && worker->pending.count==2 && !worker->active && acks==0);
    device_mqtt_service_set_dispatch_allowed(true); step(); CHECK(!worker->active);
    device_mqtt_service_set_dispatch_allowed(false);
    fail_commit=false; fake_now+=5000; step(); CHECK(!worker->pending_dirty);
    tasks=claim(); CHECK(tasks->count==2 && !strcmp(tasks->task_ids[0],"10") && !strcmp(tasks->task_ids[1],"12"));
    release(tasks,true); CHECK(!worker->pending.count && acks==0);
    /* 没有成功历史：同编号再次投递，仍通知主任务。 */
    receive_task("10"); tasks=claim(); CHECK(tasks->count==1); release(tasks,true);

    /* 失败保留通知，不能按时间自动无限唤醒；真实用户唤醒重新允许。 */
    receive_task("13"); tasks=claim(); release(tasks,false);
    CHECK(worker->pending.count==1 && worker->dispatch_paused);
    device_mqtt_service_set_dispatch_allowed(true); fake_now+=1000; step();
    CHECK(!worker->active && !device_mqtt_service_take_tasks());
    device_mqtt_service_on_wake(); tasks=claim(); release(tasks,false);
    device_mqtt_service_poll(false,2,NULL); step(); CHECK(!worker->client);
    device_mqtt_service_poll(true,3,&config); step(); connect_ok();
    CHECK(!worker->dispatch_paused);
    tasks=claim(); release(tasks,false);
    receive_task("14"); tasks=claim(); CHECK(tasks->count==2); release(tasks,true);

    /* 用户取消使用 handled=true，只消费通知，仍无播放结果上报。 */
    receive_task("15"); tasks=claim(); release(tasks,true); CHECK(!worker->pending.count && acks==0);
    unsigned old_stops=stops;
    mock_event_t e={.msg_id=worker->subscribe_id,.data="\200",.data_len=1};
    mqtt_event(worker,0,MQTT_EVENT_SUBSCRIBED,&e); step(); CHECK(worker->subscription_rejected && stops==old_stops);
    e.data="\0"; mqtt_event(worker,0,MQTT_EVENT_SUBSCRIBED,&e); step(); CHECK(worker->subscribed);
    char *payload="{\"task_id\":\"99\",\"kind\":\"reminder\",\"content\":\"x\",\"fire_at\":\"2020-01-01T00:00:00Z\"}";
    e=(mock_event_t){.topic=config.mqtt.subscribe_topic,.topic_len=(int)strlen(config.mqtt.subscribe_topic),
        .data=payload,.data_len=(int)strlen(payload),.total_data_len=(int)strlen(payload),.retain=1,.qos=1};
    mqtt_event(worker,0,MQTT_EVENT_DATA,&e); step(); CHECK(!worker->pending.count);
    e.retain=0; e.topic="devices/other/task"; e.topic_len=(int)strlen(e.topic);
    mqtt_event(worker,0,MQTT_EVENT_DATA,&e); step(); CHECK(!worker->pending.count);

    fail_commit=true; receive_task("20"); CHECK(worker->pending_dirty && !worker->active);
    device_mqtt_service_set_dispatch_allowed(true); step(); CHECK(!worker->active);
    device_mqtt_service_set_dispatch_allowed(false); fail_commit=false; fake_now+=5000; step();
    for(unsigned i=21;i<29;++i) { char id[20]; snprintf(id,sizeof(id),"%u",i); receive_task(id); }
    CHECK(worker->pending.count==8); receive_task("21"); CHECK(worker->pending.count==8);
    tasks=claim(); CHECK(tasks->count==8 && !strcmp(tasks->task_ids[7],"27"));
    release(tasks,true); CHECK(!worker->pending.count);

    /* 连接期间门禁关闭，只回收未领取快照；连接完成后一次取所有新通知。 */
    device_mqtt_service_set_dispatch_allowed(true); receive_task("90"); CHECK(worker->active);
    device_mqtt_service_set_dispatch_allowed(false); receive_task("91");
    CHECK(!worker->active && worker->pending.count==2 && !device_mqtt_service_take_tasks());
    tasks=claim(); CHECK(tasks->count==2); device_mqtt_service_set_dispatch_allowed(false); step();
    CHECK(worker->active==tasks); release(tasks,true);

    /* 身份改变 A -> B -> A 后，旧借用对象不能删去当前身份重新加载的同 ID 通知。 */
    receive_task("92"); tasks=claim(); device_mqtt_service_set_dispatch_allowed(false);
    unsigned old_serial=worker->identity_serial;
    strcpy(config.mqtt.username,"dev-2"); device_mqtt_service_poll(true,4,&config); step();
    strcpy(config.mqtt.username,"dev-1"); device_mqtt_service_poll(true,5,&config); step();
    CHECK(worker->identity_serial==old_serial+2 && worker->pending.count==1);
    release(tasks,true); CHECK(worker->pending.count==1);
    tasks=claim(); release(tasks,true);
    connect_ok();
    for(unsigned i=1;i<=5;++i) {
        signal_event(MQTT_EVENT_ERROR,0,5); CHECK(worker->retry.auth_failures==i && worker->retry.blocked==(i==5));
        signal_event(MQTT_EVENT_DISCONNECTED,0,0); CHECK(worker->retry.auth_failures==i);
        fake_now=worker->retry.retry_at_ms; step();
    }
    CHECK(!worker->client);
    device_mqtt_service_poll(false,6,NULL); step(); device_mqtt_service_poll(true,7,&config); step();
    CHECK(!worker->client); device_mqtt_service_on_wake(); step(); CHECK(worker->client && !worker->retry.blocked);
    CHECK(acks==0);
    storage_recovery_checks();
    printf("Device MQTT service: %u checks passed\n",checks); return 0;
}
