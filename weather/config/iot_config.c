#include "iot_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WEATHER_CONF_DEFAULT_PATH
#define WEATHER_CONF_DEFAULT_PATH "weather/config/weather_conf.json"
#endif

/* -------------------------------------------------------------------------
 * 极简平面 JSON 取值（不依赖 cJSON）。
 * weather_conf.json 只包含 "key": "string" 与 "key": number 两种形式，
 * 这里用轻量实现解析，避免关闭 IoT / 未链接华为 SDK 时仍产生 cJSON 依赖。
 * ------------------------------------------------------------------------- */

/* 定位 key 的值起点（跳过 "key"、冒号与其后空白），找不到返回 NULL。 */
static const char *json_find_value(const char *json, const char *key)
{
    if (json == NULL || key == NULL) return NULL;
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strchr(p, '"')) != NULL) {
        const char *ks = p + 1;
        const char *ke = strchr(ks, '"');
        if (ke == NULL) return NULL;
        if ((size_t)(ke - ks) == klen && strncmp(ks, key, klen) == 0) {
            const char *v = ke + 1;
            while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
            if (*v != ':') { p = ke + 1; continue; }
            v++;
            while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
            return v;
        }
        p = ke + 1;
    }
    return NULL;
}

/* 取字符串字段（支持 \" \\ \/ 转义）；缺失时用 def。 */
static void json_get_str(const char *json, const char *key,
                         char *out, size_t out_size, const char *def)
{
    const char *v = json_find_value(json, key);
    if (out == NULL || out_size == 0) return;
    if (v == NULL || *v != '"') {
        strncpy(out, def ? def : "", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    v++;
    size_t j = 0;
    while (*v != '\0' && *v != '"' && j + 1 < out_size) {
        if (*v == '\\' && (v[1] == '"' || v[1] == '\\' || v[1] == '/')) {
            out[j++] = v[1];
            v += 2;
        } else {
            out[j++] = *v++;
        }
    }
    out[j] = '\0';
}

/* 取整数字段；缺失/非数字时用 def。 */
static int json_get_int(const char *json, const char *key, int def)
{
    const char *v = json_find_value(json, key);
    if (v == NULL) return def;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) return def;
    return (int)n;
}

/* 从 "ssl://host:port" / "tcp://host:port" 里拆出地址和端口（前缀可省略）。
 * 返回端口，默认 def_port。 */
static int split_host_port(const char *uri, char *addr, size_t addr_size, int def_port)
{
    const char *p = uri;
    if (strncmp(p, "ssl://", 6) == 0)      p += 6;
    else if (strncmp(p, "tcp://", 6) == 0) p += 6;

    int port = def_port;
    const char *colon = strrchr(p, ':');
    if (colon != NULL) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= addr_size) hlen = addr_size - 1;
        memcpy(addr, p, hlen);
        addr[hlen] = '\0';
        port = atoi(colon + 1);
    } else {
        strncpy(addr, p, addr_size - 1);
        addr[addr_size - 1] = '\0';
    }
    return port;
}

int iot_config_load(const char *path, iot_config_t *cfg)
{
    (void)path;   /* 具体路径由内部候选列表决定（见下） */
    if (cfg == NULL) return -1;
    /* 按顺序尝试多个候选路径：环境变量 > 默认相对路径 > 板子部署路径 > 当前目录。
     * 因为 PC 模拟与板子上启动时的工作目录不同，统一兜底，避免找不到配置。 */
    const char *e = getenv("WEATHER_CONF");
    const char *candidates[8];
    int ci = 0;
    if (e) candidates[ci++] = e;
    candidates[ci++] = WEATHER_CONF_DEFAULT_PATH;
    candidates[ci++] = "/root/lvgl-weather/weather_conf.json";
    candidates[ci++] = "./weather_conf.json";
    candidates[ci++] = "weather/config/weather_conf.json";
    candidates[ci] = NULL;

    FILE *f = NULL;
    const char *used = NULL;
    for (int i = 0; candidates[i] != NULL; i++) {
        f = fopen(candidates[i], "rb");
        if (f != NULL) { used = candidates[i]; break; }
    }
    if (f == NULL) {
        fprintf(stderr, "[iot_config] 未找到配置文件（已尝试默认路径/板子路径，可用 WEATHER_CONF 指定）\n");
        return -1;
    }
    printf("[iot_config] 使用配置文件: %s\n", used);

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return -1; }

    char *buf = (char *)malloc((size_t)len + 1);
    if (buf == NULL) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)len, f);
    buf[got] = '\0';
    fclose(f);

    json_get_str(buf, "serverUri",    cfg->server_uri,    sizeof(cfg->server_uri), "");
    json_get_str(buf, "relayUri",     cfg->relay_uri,     sizeof(cfg->relay_uri), "");
    json_get_str(buf, "deviceId",     cfg->device_id,     sizeof(cfg->device_id), "");
    json_get_str(buf, "secret",       cfg->device_secret, sizeof(cfg->device_secret), "");
    json_get_str(buf, "instanceId",   cfg->instance_id,   sizeof(cfg->instance_id), "");
    json_get_str(buf, "seniverseKey", cfg->seniverse_key, sizeof(cfg->seniverse_key), "");
    json_get_str(buf, "location",     cfg->location,      sizeof(cfg->location), "广州");
    cfg->report_interval_ms = json_get_int(buf, "reportIntervalMs", 5000);

    free(buf);

    /* 从 serverUri（如 ssl://host:port）解析出 mqtt_addr 与 mqtt_port */
    cfg->mqtt_port = split_host_port(cfg->server_uri, cfg->mqtt_addr,
                                     sizeof(cfg->mqtt_addr), 8883);

    /* relayUri：内置 MQTT 客户端走的中转地址（可空） */
    cfg->relay_port = 0;
    if (cfg->relay_uri[0] != '\0')
        cfg->relay_port = split_host_port(cfg->relay_uri, cfg->relay_addr,
                                          sizeof(cfg->relay_addr), 1883);

    /* 环境变量覆盖：联调时不想改文件可直接 export，例如
     *   export IOT_DEVICE_SECRET=xxx
     *   export WEATHER_SENIVERSE_KEY=SXxxxx */
    if ((e = getenv("IOT_DEVICE_ID"))     != NULL) {
        strncpy(cfg->device_id, e, sizeof(cfg->device_id) - 1);
        cfg->device_id[sizeof(cfg->device_id) - 1] = '\0';
    }
    if ((e = getenv("IOT_DEVICE_SECRET")) != NULL) {
        strncpy(cfg->device_secret, e, sizeof(cfg->device_secret) - 1);
        cfg->device_secret[sizeof(cfg->device_secret) - 1] = '\0';
    }
    if ((e = getenv("IOT_SERVER_URI"))    != NULL) {
        strncpy(cfg->server_uri, e, sizeof(cfg->server_uri) - 1);
        cfg->server_uri[sizeof(cfg->server_uri) - 1] = '\0';
        cfg->mqtt_port = split_host_port(cfg->server_uri, cfg->mqtt_addr,
                                         sizeof(cfg->mqtt_addr), 8883);
    }
    if ((e = getenv("IOT_RELAY_URI"))     != NULL) {
        strncpy(cfg->relay_uri, e, sizeof(cfg->relay_uri) - 1);
        cfg->relay_uri[sizeof(cfg->relay_uri) - 1] = '\0';
        cfg->relay_port = split_host_port(cfg->relay_uri, cfg->relay_addr,
                                          sizeof(cfg->relay_addr), 1883);
    }
    if ((e = getenv("WEATHER_SENIVERSE_KEY")) != NULL) {
        strncpy(cfg->seniverse_key, e, sizeof(cfg->seniverse_key) - 1);
        cfg->seniverse_key[sizeof(cfg->seniverse_key) - 1] = '\0';
    }
    if ((e = getenv("WEATHER_LOCATION")) != NULL) {
        strncpy(cfg->location, e, sizeof(cfg->location) - 1);
        cfg->location[sizeof(cfg->location) - 1] = '\0';
    }

    return 0;
}

void iot_config_print(const iot_config_t *cfg)
{
    if (cfg == NULL) return;
    printf("[iot_config] device_id : %s\n", cfg->device_id);
    printf("[iot_config] mqtt_addr : %s:%d\n", cfg->mqtt_addr, cfg->mqtt_port);
    printf("[iot_config] relay     : %s\n",
           cfg->relay_port ? cfg->relay_addr : "<未配置，内置客户端直连 mqtt_addr>");
    printf("[iot_config] instance  : %s\n", cfg->instance_id);
    printf("[iot_config] location  : %s\n", cfg->location);
    printf("[iot_config] report_ms : %d\n", cfg->report_interval_ms);

    size_t n = strlen(cfg->device_secret);
    printf("[iot_config] secret    : %s\n",
           n == 0 ? "<空，请检查配置>" : "<已设置，已脱敏>");
    size_t k = strlen(cfg->seniverse_key);
    printf("[iot_config] seniverse : %s\n",
           k == 0 ? "<空，将使用本地模拟天气>" : "<已设置，将拉取真实天气>");
}
