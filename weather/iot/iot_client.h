#ifndef WEATHER_IOT_CLIENT_H
#define WEATHER_IOT_CLIENT_H

#include "iot_config.h"
#include "weather_data.h"

/*
 * 华为云 IoTDA 设备接入封装（基于 AgentLite SDK）。
 *
 * 流程：iot_client_init() 加载配置并注册回调 -> iot_client_connect() 发起 MQTT 连接
 *      -> iot_client_report_weather() 上报天气属性。连接状态由回调异步更新，
 *      通过 iot_client_is_connected() 线程安全地读取。
 */

/* 初始化 SDK（IOTA_Init）并注册所有下行回调。失败返回 -1。 */
int iot_client_init(const iot_config_t *cfg);

/* 发起 MQTT 连接（IOTA_Connect）。返回 IOTA 结果码。 */
int iot_client_connect(void);

/* 上报一份天气数据（属性上报）。返回 IOTA 消息ID，<0 表示失败。 */
int iot_client_report_weather(const weather_data_t *w);

/* 当前是否已连接（由回调更新，线程安全读取）。 */
int iot_client_is_connected(void);

/* 编译期是否启用了云上报（WEATHER_ENABLE_IOT）。
 * 为 0 时 UI 应显示"云上报未启用"，而不是"未连接" —— 后者会让人误以为是网络问题。 */
int iot_client_enabled(void);

/* 由下行回调调用，线程安全地更新连接状态。 */
void iot_client_set_connected(int v);

/* 释放 SDK 资源（IOTA_Destroy）。 */
void iot_client_destroy(void);

#endif /* WEATHER_IOT_CLIENT_H */
