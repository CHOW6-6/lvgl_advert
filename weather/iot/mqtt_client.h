#ifndef WEATHER_MQTT_CLIENT_H
#define WEATHER_MQTT_CLIENT_H

/*
 * 极简 MQTT 3.1.1 客户端（CONNECT / SUBSCRIBE / PUBLISH QoS1 / PINGREQ）。
 *
 * 为什么自己写一个：
 *   - 板上没有 ARM 版华为 AgentLite 库（那是闭源的，只有 x86 版）；
 *   - 板上也没有 libssl/libcrypto，跑不了 MQTTS。
 *   而这条链路上真正需要的只有"连上去 + 发一条属性"，
 *   为它去交叉编译 OpenSSL + paho 属于杀鸡用牛刀。
 *
 * 所以：本客户端只说**明文 MQTT**，TLS 由主机侧的 mqtts_relay.py 终结
 *       （板子 -> 172.20.120.100:1883 -> TLS -> 华为云 8883）。
 *       板子真正联网以后，把 relayUri 去掉、换 ssl:// 直连即可（需要 OpenSSL）。
 */

#include <stdint.h>

typedef struct {
    int      fd;
    uint16_t next_id;
    void   (*on_msg)(const char *topic, const char *payload, int len);
} mqtt_t;

/* 连接并完成 CONNACK 校验。成功返回 0；失败返回负值（-1 建链，-2 协议，-3 鉴权被拒）。 */
int  mqtt_open(mqtt_t *m, const char *host, int port,
               const char *client_id, const char *user, const char *pass,
               int keepalive_sec);

/* 订阅（QoS0）。成功 0。 */
int  mqtt_sub(mqtt_t *m, const char *topic);

/* 发布一条消息（QoS1，等 PUBACK）。成功 0，失败负值。 */
int  mqtt_pub(mqtt_t *m, const char *topic, const char *payload, int timeout_ms);

/* 读一批待处理报文（PUBACK/PINGRESP/下行 PUBLISH），最多等 timeout_ms。返回处理条数。 */
int  mqtt_drain(mqtt_t *m, int timeout_ms);

/* 发 PINGREQ（保活用）。 */
int  mqtt_ping(mqtt_t *m);

void mqtt_close(mqtt_t *m);

#endif /* WEATHER_MQTT_CLIENT_H */
