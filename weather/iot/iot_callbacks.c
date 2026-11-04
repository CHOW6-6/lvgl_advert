#include "iot_client.h"

/* 与 iot_client.c 保持一致：关闭 IoT、或者用内置 MQTT 客户端时，
 * 本文件这些华为 SDK 的下行回调整体不参与编译。 */
#if !defined(WEATHER_ENABLE_IOT)
#define WEATHER_ENABLE_IOT 1
#endif
#if !defined(WEATHER_IOT_BACKEND_SDK)
#define WEATHER_IOT_BACKEND_SDK 0
#endif

#if WEATHER_ENABLE_IOT && WEATHER_IOT_BACKEND_SDK

#include "iot_model.h"
#include "hw_type.h"
#include "iota_init.h"
#include "iota_datatrans.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 连接生命周期回调 ---------------- */

void on_connect_success(void *ctx, int msgId, int code, char *message)
{
    (void)ctx; (void)msgId; (void)code; (void)message;
    iot_client_set_connected(1);
    printf("[iot] 连接成功，已接入华为云 IoTDA\n");
}

void on_connect_failure(void *ctx, int msgId, int code, char *message)
{
    (void)ctx; (void)msgId;
    iot_client_set_connected(0);
    printf("[iot] 连接失败, code=%d, msg=%s\n", code, message ? message : "");
    /* 网络恢复后可在外部定时重试 IOTA_Connect() */
}

void on_connection_lost(void *ctx, int msgId, int code, char *message)
{
    (void)ctx; (void)msgId;
    iot_client_set_connected(0);
    printf("[iot] 连接断开, code=%d, msg=%s\n", code, message ? message : "");
}

void on_disconnect_success(void *ctx, int msgId, int code, char *message)
{
    (void)ctx; (void)msgId; (void)code; (void)message;
    iot_client_set_connected(0);
    printf("[iot] 主动断开成功\n");
}

void on_disconnect_failure(void *ctx, int msgId, int code, char *message)
{
    (void)ctx; (void)msgId; (void)code; (void)message;
    printf("[iot] 主动断开失败\n");
}

void on_publish_success(void *ctx, int msgId, int code, char *message)
{
    (void)ctx; (void)msgId; (void)code; (void)message;
    printf("[iot] 发布成功, msgId=%d\n", msgId);
}

void on_publish_failure(void *ctx, int msgId, int code, char *message)
{
    (void)ctx; (void)msgId; (void)code; (void)message;
    printf("[iot] 发布失败, msgId=%d, code=%d\n", msgId, code);
}

void on_subscribe_success(void *ctx, int msgId, int code, char *message)
{
    (void)ctx; (void)msgId; (void)code; (void)message;
    printf("[iot] 订阅成功\n");
}

void on_subscribe_failure(void *ctx, int msgId, int code, char *message)
{
    (void)ctx; (void)msgId; (void)code; (void)message;
    printf("[iot] 订阅失败, code=%d\n", code);
}

/* ---------------- 平台下行消息回调 ---------------- */

void on_message_down(void *ctx, int msgId, int code, char *message)
{
    (void)ctx; (void)msgId; (void)code;
    printf("[iot] 收到平台消息: %s\n", message ? message : "");
}

void on_command_request(void *ctx, int msgId, int code, char *message, char *requestId)
{
    (void)ctx; (void)msgId; (void)code;
    printf("[iot] 收到命令: %s (requestId=%s)\n", message ? message : "", requestId ? requestId : "");

    /* 解析命令名（demo：可在此根据 command_name 触发本地动作） */
    cJSON *root = cJSON_Parse(message ? message : "{}");
    if (root != NULL) {
        cJSON *name = cJSON_GetObjectItem(root, "command_name");
        if (cJSON_IsString(name))
            printf("[iot] command_name = %s\n", name->valuestring);
        cJSON_Delete(root);
    }

    char *resp = "{\"result\":\"success\"}";
    IOTA_CommandResponse((HW_CHAR *)requestId, 0, NULL, (HW_CHAR *)resp);
}

void on_properties_set(void *ctx, int msgId, int code, char *message, char *requestId)
{
    (void)ctx; (void)msgId; (void)code;
    printf("[iot] 平台设置属性: %s (requestId=%s)\n", message ? message : "", requestId ? requestId : "");
    IOTA_PropertiesSetResponse((HW_CHAR *)requestId, 0, (HW_CHAR *)"success");
}

void on_properties_get(void *ctx, int msgId, int code, char *message, char *requestId)
{
    (void)ctx; (void)msgId; (void)code;
    printf("[iot] 平台读取属性: %s (requestId=%s)\n", message ? message : "", requestId ? requestId : "");

    /* 回报当前天气服务的最新值（示例用占位，真实值可在此读取共享数据） */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, PROP_TEMPERATURE, 0);
    cJSON_AddStringToObject(root, PROP_WEATHER_TEXT, "-");
    cJSON_AddStringToObject(root, PROP_CITY, "-");
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    ST_IOTA_SERVICE_DATA_INFO svc;
    memset(&svc, 0, sizeof(svc));
    svc.service_id = (HW_CHAR *)WEATHER_SERVICE_ID;
    svc.event_time = NULL;
    svc.properties = (HW_CHAR *)payload;

    IOTA_PropertiesGetResponse((HW_CHAR *)requestId, &svc, 1);
    free(payload);
}

#endif /* WEATHER_ENABLE_IOT && WEATHER_IOT_BACKEND_SDK */
