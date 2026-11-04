#include "iot_client.h"

/* 是否启用华为云 IoT 上报。默认启用；Makefile 可用 WEATHER_ENABLE_IOT=0 关闭，
 * 关闭时不链接任何云端代码，天气界面/数据层照常工作（仅无云端上报）。 */
#if !defined(WEATHER_ENABLE_IOT)
#define WEATHER_ENABLE_IOT 1
#endif

/* 用哪套实现连华为云：
 *   1 = 华为 AgentLite SDK（需要 lib/ 下有 ARM 版 libHWMQTT.so 等，官方方案）
 *   0 = 内置 MQTT 客户端（不依赖任何 SDK/OpenSSL，明文 MQTT 走主机中转）
 * Makefile 里 WEATHER_IOT_BACKEND=auto 会按 lib/ 里有没有 ARM 库自动选。 */
#if !defined(WEATHER_IOT_BACKEND_SDK)
#define WEATHER_IOT_BACKEND_SDK 0
#endif

#if WEATHER_ENABLE_IOT

#include "iot_model.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#if WEATHER_IOT_BACKEND_SDK
#include "hw_type.h"          /* 必须放在 iota_*.h 之前，HW_CHAR/HW_INT 等类型在此定义 */
#include "iota_init.h"
#include "iota_login.h"
#include "iota_datatrans.h"
#include "iota_cfg.h"
#include "StringUtil.h"       /* getEventTimeStamp() */
#include "cJSON.h"
#else
#include "mqtt_client.h"
#include "sha256.h"
#endif

static const iot_config_t *g_cfg = NULL;
static int                 g_connected = 0;
static pthread_mutex_t     g_conn_mtx = PTHREAD_MUTEX_INITIALIZER;

/* 由回调 / 连接流程调用，线程安全地更新连接状态 */
void iot_client_set_connected(int v)
{
    pthread_mutex_lock(&g_conn_mtx);
    g_connected = v;
    pthread_mutex_unlock(&g_conn_mtx);
}

int iot_client_is_connected(void)
{
    int v = 0;
    pthread_mutex_lock(&g_conn_mtx);
    v = g_connected;
    pthread_mutex_unlock(&g_conn_mtx);
    return v;
}

int iot_client_enabled(void)
{
    return 1;
}

/* 按 UTF-8 字符边界截断到不超过 max_bytes 字节，避免把汉字截成半个。
 * 平台对 weatherText / city 的 max_length 为 10，这里保证不超限。 */
static void utf8_trim(char *dst, size_t dst_size, const char *src, size_t max_bytes)
{
    size_t n = 0;
    if (dst_size == 0) return;
    while (src != NULL && src[n] != '\0' && n < max_bytes) {
        unsigned char c = (unsigned char)src[n];
        size_t len;
        if      ((c & 0x80) == 0x00) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else                          len = 1;
        if (n + len > max_bytes) break;
        n += len;
    }
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* 平台要求 DateTime 形如 20261102T081333Z（UTC） */
static void fmt_event_time(char out[20])
{
    time_t t = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    strftime(out, 20, "%Y%m%dT%H%M%SZ", &tmv);
}

/* =========================================================================
 * 后端 A：华为 AgentLite SDK
 * ========================================================================= */
#if WEATHER_IOT_BACKEND_SDK

/* 下行回调声明（实现见 iot_callbacks.c） */
extern void on_connect_success(void *, int, int, char *);
extern void on_connect_failure(void *, int, int, char *);
extern void on_connection_lost(void *, int, int, char *);
extern void on_disconnect_success(void *, int, int, char *);
extern void on_disconnect_failure(void *, int, int, char *);
extern void on_publish_success(void *, int, int, char *);
extern void on_publish_failure(void *, int, int, char *);
extern void on_subscribe_success(void *, int, int, char *);
extern void on_subscribe_failure(void *, int, int, char *);
extern void on_message_down(void *, int, int, char *);
extern void on_command_request(void *, int, int, char *, char *);
extern void on_properties_set(void *, int, int, char *, char *);
extern void on_properties_get(void *, int, int, char *, char *);

/* 极简日志回调，避免引入 LogUtil 依赖 */
static void my_print_log(int level, char *format, va_list args)
{
    (void)level;
    vprintf(format, args);
}

int iot_client_init(const iot_config_t *cfg)
{
    if (cfg == NULL) return -1;
    g_cfg = cfg;

    IOTA_SetPrintLogCallback(my_print_log);

    /* 必须先 IOTA_Init() 再设置连接参数：IOTA_Init 会初始化 SDK 内部状态，
     * 若在 ConfigSetStr 之后再调用 Init，会清掉刚设置的配置（连接报 NULL）。 */
    int ret = IOTA_Init((HW_CHAR *)".");
    if (ret > 0) {
        fprintf(stderr, "[iot] IOTA_Init() 失败, code=%d\n", ret);
        return -1;
    }

    IOTA_ConfigSetStr(EN_IOTA_CFG_MQTT_ADDR,    (HW_CHAR *)cfg->mqtt_addr);
    IOTA_ConfigSetUint(EN_IOTA_CFG_MQTT_PORT,   (HW_UINT)cfg->mqtt_port);
    IOTA_ConfigSetStr(EN_IOTA_CFG_DEVICEID,     (HW_CHAR *)cfg->device_id);
    IOTA_ConfigSetStr(EN_IOTA_CFG_DEVICESECRET, (HW_CHAR *)cfg->device_secret);

    IOTA_SetCallback(EN_IOTA_CALLBACK_CONNECT_SUCCESS,     on_connect_success);
    IOTA_SetCallback(EN_IOTA_CALLBACK_CONNECT_FAILURE,     on_connect_failure);
    IOTA_SetCallback(EN_IOTA_CALLBACK_CONNECTION_LOST,     on_connection_lost);
    IOTA_SetCallback(EN_IOTA_CALLBACK_DISCONNECT_SUCCESS,  on_disconnect_success);
    IOTA_SetCallback(EN_IOTA_CALLBACK_DISCONNECT_FAILURE,  on_disconnect_failure);
    IOTA_SetCallback(EN_IOTA_CALLBACK_SUBSCRIBE_SUCCESS,   on_subscribe_success);
    IOTA_SetCallback(EN_IOTA_CALLBACK_SUBSCRIBE_FAILURE,   on_subscribe_failure);
    IOTA_SetCallback(EN_IOTA_CALLBACK_PUBLISH_SUCCESS,     on_publish_success);
    IOTA_SetCallback(EN_IOTA_CALLBACK_PUBLISH_FAILURE,     on_publish_failure);
    IOTA_SetCallback(EN_IOTA_CALLBACK_MESSAGE_DOWN,        on_message_down);
    IOTA_SetCallbackWithTopic(EN_IOTA_CALLBACK_COMMAND_REQUEST, on_command_request);
    IOTA_SetCallbackWithTopic(EN_IOTA_CALLBACK_PROPERTIES_SET,  on_properties_set);
    IOTA_SetCallbackWithTopic(EN_IOTA_CALLBACK_PROPERTIES_GET,  on_properties_get);

    /* 注意：沿用官方示例流程，不调用 IOTA_Init()（IOTA_Connect 内部已处理初始化）。
     * 若在此处额外调用 IOTA_Init(".")，会清空上面刚设置的连接配置，导致连接报
     * "serverIp/port/username can not be NULL"。 */
    return 0;
}

int iot_client_connect(void)
{
    return IOTA_Connect();
}

int iot_client_report_weather(const weather_data_t *w)
{
    if (w == NULL) return -1;
    /* 未连接时 MQTT 客户端为 NULL，此时上报会触发 SDK 内部空指针解引用。
     * 这里做防御：仅当已连接才上报（后台线程也已在调用处做了同样的判断）。 */
    if (!iot_client_is_connected()) return -2;

    /* 按平台产品模型对齐类型：
     *   temperature  decimal  -> 浮点
     *   humidity     int      -> 取整
     *   windSpeed    long     -> 取整
     *   weatherText  string(10) / city string(10) -> 按 UTF-8 边界截断
     *   updateTime   DateTime -> 必须 yyyyMMdd'T'HHmmss'Z'，用 SDK 的 getEventTimeStamp() */
    char text_trim[16];
    char city_trim[16];
    /* 模拟数据的 city 带 "模拟:" 前缀（ASCII 冒号，共 7 字节），上报前去掉，
     * 避免把 "模拟:深圳" 这类非真实城市名发到平台。 */
    const char *city_src = w->now.city;
    if (strncmp(city_src, "模拟:", 7) == 0) city_src += 7;
    utf8_trim(text_trim, sizeof(text_trim), w->now.text, 10);
    utf8_trim(city_trim, sizeof(city_trim), city_src, 10);

    const char *ts = getEventTimeStamp();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, PROP_TEMPERATURE,  (double)w->now.temperature);
    cJSON_AddNumberToObject(root, PROP_HUMIDITY,     (int)w->now.humidity);
    cJSON_AddNumberToObject(root, PROP_WIND_SPEED,   (long)w->now.wind_speed);
    cJSON_AddStringToObject(root, PROP_WEATHER_TEXT, text_trim);
    cJSON_AddStringToObject(root, PROP_CITY,         city_trim);
    cJSON_AddStringToObject(root, PROP_UPDATE_TIME,  ts ? ts : "");

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) return -1;

    ST_IOTA_SERVICE_DATA_INFO svc;
    memset(&svc, 0, sizeof(svc));
    svc.service_id = (HW_CHAR *)WEATHER_SERVICE_ID;
    svc.event_time = (HW_CHAR *)getEventTimeStamp();  /* 事件时间；SDK 自带实现 */
    svc.properties = (HW_CHAR *)payload;

    int msgId = IOTA_PropertiesReport(&svc, 1);
    free(payload);
    return msgId;
}

void iot_client_destroy(void)
{
    IOTA_DisConnect();
    IOTA_Destroy();
}

/* =========================================================================
 * 后端 B：内置 MQTT 客户端（不需要 SDK / OpenSSL）
 *
 * 板子上没有 ARM 版 AgentLite 库，也没有 libssl，官方 SDK 那条路走不通。
 * 这里直接按华为云 IoTDA 的公开协议说话：
 *   ClientId = {deviceId}_0_0_{yyyyMMddHH}   （UTC）
 *   Username = {deviceId}
 *   Password = HEX(HMAC-SHA256(key=上面那串时间, msg=secret))
 *   上报主题 = $oc/devices/{deviceId}/sys/properties/report
 * 详见 weather/README.md「两条上云通路」。
 * ========================================================================= */
#else

static mqtt_t g_mq;
static int    g_mq_ok = 0;
static time_t g_last_tx = 0;
static time_t g_last_try = 0;   /* 上次尝试连接的时刻，用于重连退避 */

/* 平台下行（比如属性上报的响应）都从这里出来 */
static void on_mqtt_msg(const char *topic, const char *payload, int len)
{
    int n = len > 160 ? 160 : len;
    printf("[iot] 下行 %s -> %.*s%s\n", topic, n, payload, len > n ? "…" : "");
}

/* 时间戳（UTC，精确到小时）：既是 ClientId 的一部分，也是口令的 HMAC key */
static void mqtt_stamp(char out[16])
{
    time_t t = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    strftime(out, 16, "%Y%m%d%H", &tmv);
}

int iot_client_init(const iot_config_t *cfg)
{
    if (cfg == NULL) return -1;
    g_cfg = cfg;
    memset(&g_mq, 0, sizeof(g_mq));
    g_mq.fd = -1;
    g_mq.on_msg = on_mqtt_msg;
    g_mq_ok = 0;
    printf("[iot] 使用内置 MQTT 客户端（不依赖华为 SDK）\n");
    return 0;
}

int iot_client_connect(void)
{
    if (g_cfg == NULL) return -1;
    if (!g_cfg->device_id[0] || !g_cfg->device_secret[0]) {
        fprintf(stderr, "[iot] 没配 deviceId/secret，跳过连接\n");
        return -1;
    }

    if (g_mq_ok) { mqtt_close(&g_mq); g_mq_ok = 0; }

    char stamp[16];
    mqtt_stamp(stamp);

    char client_id[160];
    snprintf(client_id, sizeof(client_id), "%s_0_0_%s", g_cfg->device_id, stamp);

    char pass[65];
    /* 华为云是"时间戳当 key、secret 当消息"，顺序反了就算不出对的密码 */
    hmac_sha256_hex(stamp, g_cfg->device_secret, pass);

    /* 有 relayUri 就走主机中转（板子说不了 TLS、也上不了外网时的唯一通路） */
    const char *host = g_cfg->relay_port ? g_cfg->relay_addr : g_cfg->mqtt_addr;
    int         port = g_cfg->relay_port ? g_cfg->relay_port : g_cfg->mqtt_port;

    printf("[iot] 连接 %s:%d  clientId=%s\n", host, port, client_id);

    int rc = mqtt_open(&g_mq, host, port, client_id, g_cfg->device_id, pass, 60);
    if (rc != 0) {
        iot_client_set_connected(0);
        fprintf(stderr, "[iot] 连接失败 (rc=%d)\n", rc);
        return rc;
    }

    /* 订阅属性上报的响应，能直接看到平台收没收 */
    char topic[192];
    snprintf(topic, sizeof(topic), "$oc/devices/%s/sys/properties/report/response",
             g_cfg->device_id);
    mqtt_sub(&g_mq, topic);

    g_mq_ok = 1;
    g_last_tx = time(NULL);
    iot_client_set_connected(1);
    printf("[iot] ✓ 已连接华为云 IoTDA（内置客户端）\n");
    return 0;
}

/* JSON 字符串转义（城市/天气现象理论上没有引号，防一手） */
static void json_escape(char *dst, size_t n, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 2 < n; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = c; }
        else if ((unsigned char)c < 0x20) { continue; }
        else dst[j++] = c;
    }
    dst[j] = '\0';
}

int iot_client_report_weather(const weather_data_t *w)
{
    if (w == NULL) return -1;

    /* 断线自动重连：顺带刷新按小时变化的口令。
     * 20 秒退避 —— 中转没开时别每 5 秒砸一次门，把日志刷成瀑布。 */
    if (!g_mq_ok || !iot_client_is_connected()) {
        time_t now = time(NULL);
        if (now - g_last_try < 20) return -2;
        g_last_try = now;
        if (iot_client_connect() != 0) return -2;
    }

    char text_trim[16], city_trim[16], text_esc[32], city_esc[32];
    const char *city_src = w->now.city;
    if (strncmp(city_src, "模拟:", 7) == 0) city_src += 7;
    utf8_trim(text_trim, sizeof(text_trim), w->now.text, 10);
    utf8_trim(city_trim, sizeof(city_trim), city_src, 10);
    json_escape(text_esc, sizeof(text_esc), text_trim);
    json_escape(city_esc, sizeof(city_esc), city_trim);

    char ts[20];
    fmt_event_time(ts);

    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"services\":[{\"service_id\":\"%s\",\"event_time\":\"%s\",\"properties\":{"
             "\"temperature\":%.1f,\"humidity\":%d,\"windSpeed\":%d,"
             "\"weatherText\":\"%s\",\"city\":\"%s\",\"updateTime\":\"%s\"}}]}",
             WEATHER_SERVICE_ID, ts,
             (double)w->now.temperature, (int)w->now.humidity, (int)w->now.wind_speed,
             text_esc, city_esc, ts);

    char topic[192];
    snprintf(topic, sizeof(topic), "$oc/devices/%s/sys/properties/report",
             g_cfg->device_id);

    int rc = mqtt_pub(&g_mq, topic, payload, 5000);
    g_last_tx = time(NULL);

    if (rc != 0) {
        fprintf(stderr, "[iot] 上报失败，标记断线，下轮重连\n");
        iot_client_set_connected(0);
        g_mq_ok = 0;
        mqtt_close(&g_mq);
        return -1;
    }

    /* 把平台响应读出来（会打印 result_code），顺便回收下行报文 */
    mqtt_drain(&g_mq, 800);
    printf("[iot] 属性已上报（PUBACK）\n");
    return 0;
}

void iot_client_destroy(void)
{
    mqtt_close(&g_mq);
    g_mq_ok = 0;
    iot_client_set_connected(0);
}

#endif /* WEATHER_IOT_BACKEND_SDK */

#else  /* !WEATHER_ENABLE_IOT：未启用 IoT，提供空实现，保证上层可正常编译运行 */
#include <stdio.h>

int  iot_client_init(const iot_config_t *cfg)
{
    (void)cfg;
    printf("[iot] 未启用云上报（编译期 WEATHER_ENABLE_IOT=0）：界面与天气数据照常工作，"
           "数据只在本机显示，不会发到华为云。\n");
    printf("[iot] 想开启：make weather WEATHER_ENABLE_IOT=1，详见 weather/README.md\n");
    return 0;
}
int  iot_client_connect(void)                            { return 0; }
int  iot_client_is_connected(void)                       { return 0; }
int  iot_client_enabled(void)                            { return 0; }
void iot_client_set_connected(int v)                     { (void)v; }
int  iot_client_report_weather(const weather_data_t *w)  { (void)w; return -1; }
void iot_client_destroy(void)                            { }

#endif /* WEATHER_ENABLE_IOT */
