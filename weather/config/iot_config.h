#ifndef WEATHER_IOT_CONFIG_H
#define WEATHER_IOT_CONFIG_H

#include <stdbool.h>

/*
 * 华为云 IoTDA（设备接入）连接配置 + 心知天气 API 配置。
 *
 * 这些字段在程序启动时由 iot_config_load() 从 weather/config/weather_conf.json
 * 读取；环境变量优先级更高，便于联调、避免把密钥写死进仓库。
 *
 * 每一项“从哪里获取”见各字段上方的注释。
 */
typedef struct {
    /* ===== 华为云 IoTDA ===== */

    /* 原始 MQTT 接入地址（含 ssl:// 与 :8883）。
     * 控制台：设备接入 IoTDA -> 总览/接入信息 -> MQTT 接入 -> 设备侧 -> 接入地址。
     * 由它自动解析出 mqtt_addr 与 mqtt_port，一般无需手填。 */
    char server_uri[256];

    /* 去掉 "ssl://" 前缀与 ":8883" 端口后的服务器域名/IP（自动解析得到）。 */
    char mqtt_addr[256];

    /* MQTT over TLS 端口，固定 8883（自动解析得到）。 */
    int  mqtt_port;

    /* 明文 MQTT 中转地址（只有"内置 MQTT 客户端"这条通路会用）。
     * 板子既没有 libssl、也上不了外网时，由主机上的 weather/tools/mqtts_relay.py
     * 终结 TLS：板子 -> 172.20.120.100:1883 -> TLS -> 华为云 8883。
     * 留空表示不用中转，直接连 mqtt_addr（需要板子自己能跑 TLS）。 */
    char relay_uri[256];
    char relay_addr[256];
    int  relay_port;

    /* 设备ID（平台分配的 deviceId / nodeId）。
     * 控制台：设备接入 IoTDA -> 设备 -> 所有设备 -> 具体设备 -> 设备ID。 */
    char device_id[128];

    /* 设备密钥（secret）。
     * 控制台：同一设备详情页 -> “设备密钥”（仅创建 / 重置时可见，需妥善保存）。 */
    char device_secret[128];

    /* 实例ID（仅作参考/核对，MQTT 直连不需要，但排障时常用到）。
     * 控制台：设备接入 IoTDA -> 总览 -> 实例信息 -> 实例ID。 */
    char instance_id[64];

    /* 属性上报周期（毫秒），默认 5000。对应天气监测的刷新频率。 */
    int  report_interval_ms;

    /* ===== 心知天气（第三方天气数据源）===== */

    /* 心知天气私钥（API key，形如 SXxxxx）。
     * 获取：心知天气控制台 -> 产品/密钥管理 -> 私钥。
     * 注意：属于机密信息，建议用环境变量 WEATHER_SENIVERSE_KEY 覆盖。 */
    char seniverse_key[64];

    /* 查询城市（中文或拼音均可，如 "深圳" / "beijing" / "ip"）。
     * 心知天气 API 的 location 参数。 */
    char location[64];
} iot_config_t;

/* 从 JSON 配置文件加载；成功返回 0，失败返回 -1（并打印原因）。
 * path 为 NULL 时使用默认路径 weather/config/weather_conf.json。
 * 环境变量覆盖：IOT_DEVICE_ID / IOT_DEVICE_SECRET / IOT_SERVER_URI /
 *               WEATHER_SENIVERSE_KEY / WEATHER_LOCATION。 */
int iot_config_load(const char *path, iot_config_t *cfg);

/* 打印当前配置（密钥做脱敏处理），用于启动自检日志。 */
void iot_config_print(const iot_config_t *cfg);

#endif /* WEATHER_IOT_CONFIG_H */
