#ifndef WEATHER_HTTP_CLIENT_H
#define WEATHER_HTTP_CLIENT_H

#include <stddef.h>

/*
 * 极简 HTTPS GET 客户端（基于 OpenSSL）。
 *
 * 仅依赖 OpenSSL（项目已随华为 AgentLite SDK 附带 libssl.so.1.1 / libcrypto.so.1.1）。
 *
 * 注意：本文件在 WEATHER_ENABLE_HTTP=1 时才会被编译/链接。
 * 若构建环境没有 OpenSSL 开发头文件（openssl/ssl.h），请确保关闭该宏，
 * 此时天气数据将回退为本地模拟（见 weather_data.c）。
 *
 * 成功返回 0，并把完整 HTTP 响应体（含头部）写入 *out（调用者 free）；
 * 失败返回 -1 且 *out 为 NULL。
 */
int https_get(const char *host, int port, const char *path, char **out, size_t *out_len);

#endif /* WEATHER_HTTP_CLIENT_H */
