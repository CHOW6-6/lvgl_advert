#ifndef WEATHER_SHA256_H
#define WEATHER_SHA256_H

#include <stddef.h>
#include <stdint.h>

/*
 * 最小 SHA-256 + HMAC-SHA256，只为华为云 MQTT 的口令派生服务：
 *
 *     password = HEX( HMAC-SHA256(key = 时间戳, msg = 设备密钥) )
 *
 * 注意 key 和 msg 的位置：华为云是"时间戳当 key、secret 当消息"，
 * 和直觉正好反过来（拿 secret 当 key 是算不出来的，这个坑踩过）。
 *
 * 时间戳 = ClientId 里那串 yyyyMMddHH（UTC）。
 * 例：secret=123456789abcd, ts=2026110208
 *     -> 07d77c033d6d269352be1ec2c8bfda876a89bc61343288955fa9b5652da973f4
 */

/* 输出 64 字节小写 hex + '\0'，out 至少 65 字节 */
void hmac_sha256_hex(const char *key, const char *msg, char out[65]);

#endif /* WEATHER_SHA256_H */
