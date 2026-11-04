#include "weather_data.h"
#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 缓存文件候选路径（主机抓好心知数据后推送到板上）。
 * 板端 /root/lvgl-weather/weather_cache.txt；PC 上调试用相对路径。 */
#ifndef WEATHER_CACHE_DEFAULT_PATH
#define WEATHER_CACHE_DEFAULT_PATH "/root/lvgl-weather/weather_cache.txt"
#endif

#if WEATHER_ENABLE_HTTP
#include "cJSON.h"   /* 仅真实 HTTPS 拉取时需要 JSON 解析 */

/* ---------- 工具：极简 URL 编码（编码非字母数字及中文等多字节字符）---------- */
static void url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char *hex = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_size; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = (char)c;
        } else {
            if (j + 3 >= dst_size) break;
            dst[j++] = '%';
            dst[j++] = hex[(c >> 4) & 0xF];
            dst[j++] = hex[c & 0xF];
        }
    }
    dst[j] = '\0';
}

/* 从 ISO 时间串截取 "MM-DD HH:MM" 便于显示 */
static void fmt_update_time(const char *src, char *dst, size_t n)
{
    /* 心知格式形如 2020-07-07T21:00:00+08:00 */
    if (src == NULL || strlen(src) < 11) { strncpy(dst, "--", n); dst[n-1]='\0'; return; }
    const char *t = src + 5; /* 跳过 "YYYY-" */
    size_t k = 0;
    while (*t && *t != 'T' && k + 1 < n) dst[k++] = *t++;
    if (*t == 'T' && k + 1 < n) dst[k++] = ' ';
    t++;
    while (*t && *t != '+' && *t != '-' && *t != 'Z' && k + 1 < n) dst[k++] = *t++;
    dst[k] = '\0';
}

/* 解析心知 "now" 与 "daily" 两个接口的响应，填充 out。返回 0 成功。 */
static int parse_seniverse(const char *now_json, const char *daily_json, weather_data_t *out)
{
    cJSON *nroot = cJSON_Parse(now_json);
    if (nroot == NULL) return -1;

    cJSON *results = cJSON_GetObjectItem(nroot, "results");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        cJSON_Delete(nroot);
        return -1;
    }
    cJSON *r0 = cJSON_GetArrayItem(results, 0);

    cJSON *loc = cJSON_GetObjectItem(r0, "location");
    cJSON *now = cJSON_GetObjectItem(r0, "now");
    cJSON *upd = cJSON_GetObjectItem(r0, "last_update");
    if (now == NULL) { cJSON_Delete(nroot); return -1; }

    if (loc) {
        cJSON *name = cJSON_GetObjectItem(loc, "name");
        if (cJSON_IsString(name))
            strncpy(out->now.city, name->valuestring, sizeof(out->now.city) - 1);
    }
    cJSON *txt = cJSON_GetObjectItem(now, "text");
    if (cJSON_IsString(txt))
        strncpy(out->now.text, txt->valuestring, sizeof(out->now.text) - 1);
    cJSON *tmp = cJSON_GetObjectItem(now, "temperature");
    if (cJSON_IsString(tmp) || cJSON_IsNumber(tmp))
        out->now.temperature = (float)tmp->valuedouble;
    cJSON *code = cJSON_GetObjectItem(now, "code");
    if (cJSON_IsString(code)) out->now.code = atoi(code->valuestring);

    if (cJSON_IsString(upd))
        fmt_update_time(upd->valuestring, out->now.update_time, sizeof(out->now.update_time));

    cJSON_Delete(nroot);

    /* daily（可选，失败不影响 now） */
    out->forecast_days = 0;
    if (daily_json != NULL) {
        cJSON *droot = cJSON_Parse(daily_json);
        if (droot != NULL) {
            cJSON *dres = cJSON_GetObjectItem(droot, "results");
            if (cJSON_IsArray(dres) && cJSON_GetArraySize(dres) > 0) {
                cJSON *dd = cJSON_GetObjectItem(cJSON_GetArrayItem(dres, 0), "daily");
                if (cJSON_IsArray(dd)) {
                    int days = cJSON_GetArraySize(dd);
                    if (days > WEATHER_FORECAST_DAYS) days = WEATHER_FORECAST_DAYS;
                    for (int i = 0; i < days; i++) {
                        cJSON *d = cJSON_GetArrayItem(dd, i);
                        cJSON *date = cJSON_GetObjectItem(d, "date");
                        cJSON *td   = cJSON_GetObjectItem(d, "text_day");
                        cJSON *hi   = cJSON_GetObjectItem(d, "high");
                        cJSON *lo   = cJSON_GetObjectItem(d, "low");
                        cJSON *hu   = cJSON_GetObjectItem(d, "humidity");
                        cJSON *ws   = cJSON_GetObjectItem(d, "wind_speed");
                        if (cJSON_IsString(date))
                            strncpy(out->forecast[i].date, date->valuestring,
                                    sizeof(out->forecast[i].date) - 1);
                        if (cJSON_IsString(td))
                            strncpy(out->forecast[i].text_day, td->valuestring,
                                    sizeof(out->forecast[i].text_day) - 1);
                        if (hi && (cJSON_IsString(hi) || cJSON_IsNumber(hi)))
                            out->forecast[i].high = (float)hi->valuedouble;
                        if (lo && (cJSON_IsString(lo) || cJSON_IsNumber(lo)))
                            out->forecast[i].low = (float)lo->valuedouble;
                        /* now 接口不带湿度/风速，只能借 daily[0] 补上 */
                        if (i == 0 && hu && (cJSON_IsString(hu) || cJSON_IsNumber(hu)))
                            out->now.humidity = (float)hu->valuedouble;
                        if (i == 0 && ws && (cJSON_IsString(ws) || cJSON_IsNumber(ws)))
                            out->now.wind_speed = (float)ws->valuedouble;
                    }
                    out->forecast_days = days;
                }
            }
            cJSON_Delete(droot);
        }
    }

    out->now.valid = true;
    return 0;
}

/* 设备自己直连心知（需要 OpenSSL 头文件 + 板子真的能上外网）。
 * 注意：GEC6818 默认没有默认路由，这条路在板端基本走不通，
 * 留给 PC 模拟器或已经配好 NAT 的环境使用。 */
static int fetch_via_https(const char *key, const char *location, weather_data_t *out)
{
    char enc_loc[256];
    url_encode(location, enc_loc, sizeof(enc_loc));

    char now_path[512];
    snprintf(now_path, sizeof(now_path),
             "/v3/weather/now.json?key=%s&location=%s&language=zh-Hans&unit=c",
             key, enc_loc);

    char daily_path[512];
    snprintf(daily_path, sizeof(daily_path),
             "/v3/weather/daily.json?key=%s&location=%s&language=zh-Hans&unit=c&start=0&days=%d",
             key, enc_loc, WEATHER_FORECAST_DAYS);

    char *now_body = NULL, *daily_body = NULL;
    size_t nlen = 0, dlen = 0;
    int rc = https_get("api.seniverse.com", 443, now_path, &now_body, &nlen);
    if (rc != 0 || now_body == NULL) {
        free(now_body);
        free(daily_body);
        return -1;
    }
    https_get("api.seniverse.com", 443, daily_path, &daily_body, &dlen);

    int prc = parse_seniverse(now_body, daily_body, out);
    free(now_body);
    free(daily_body);
    return prc;
}
#endif /* WEATHER_ENABLE_HTTP */

/* -------------------------------------------------------------------------
 * 缓存文件：主机跑 weather/tools/fetch_weather.py 抓心知，写成 key=value 文本，
 * 再用 ssh 管道丢到板子上。板子不联网也能显示真实天气。
 *
 * 格式（一行一项，'#' 开头为注释）：
 *   ts=1793347829
 *   city=深圳
 *   text=晴
 *   code=0
 *   temp=30
 *   humidity=83
 *   wind=8.4
 *   updated=11-02 15:04
 *   days=3
 *   d0_date=11-03  d0_text=晴  d0_high=29  d0_low=24
 * ------------------------------------------------------------------------- */

static void copy_str(char *dst, size_t n, const char *src)
{
    if (dst == NULL || n == 0) return;
    strncpy(dst, src ? src : "", n - 1);
    dst[n - 1] = '\0';
}

/* 定位缓存文件：环境变量 > 板端部署路径 > 当前目录 > 源码目录。 */
static FILE *cache_open(const char **used)
{
    const char *e = getenv("WEATHER_CACHE");
    const char *cand[5];
    int ci = 0;
    if (e != NULL && e[0] != '\0') cand[ci++] = e;
    cand[ci++] = WEATHER_CACHE_DEFAULT_PATH;
    cand[ci++] = "./weather_cache.txt";
    cand[ci++] = "weather/data/weather_cache.txt";
    cand[ci] = NULL;

    for (int i = 0; i < ci; i++) {
        FILE *f = fopen(cand[i], "rb");
        if (f != NULL) {
            if (used) *used = cand[i];
            return f;
        }
    }
    return NULL;
}

/* 读缓存文件；解析到可用数据返回 0，否则 -1（保持 out 原样，由调用者回退）。 */
static int weather_cache_load(weather_data_t *out)
{
    const char *used = NULL;
    FILE *f = cache_open(&used);
    if (f == NULL) return -1;

    weather_data_t w;
    memset(&w, 0, sizeof(w));
    w.forecast_days = 0;

    int has_city = 0, has_temp = 0;
    char line[256];

    while (fgets(line, sizeof(line), f) != NULL) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (eq == NULL) continue;
        *eq = '\0';
        const char *k = line;
        const char *v = eq + 1;

        if      (strcmp(k, "city") == 0)  { copy_str(w.now.city, sizeof(w.now.city), v); has_city = 1; }
        else if (strcmp(k, "text") == 0)  { copy_str(w.now.text, sizeof(w.now.text), v); }
        else if (strcmp(k, "code") == 0)  { w.now.code = atoi(v); }
        else if (strcmp(k, "temp") == 0)  { w.now.temperature = (float)atof(v); has_temp = 1; }
        else if (strcmp(k, "humidity") == 0) { w.now.humidity = (float)atof(v); }
        else if (strcmp(k, "wind") == 0)  { w.now.wind_speed = (float)atof(v); }
        else if (strcmp(k, "updated") == 0) { copy_str(w.now.update_time, sizeof(w.now.update_time), v); }
        else if (strcmp(k, "ts") == 0)    { w.ts = atol(v); }
        else if (strcmp(k, "days") == 0)  { w.forecast_days = atoi(v); }
        else if (k[0] == 'd' && k[1] >= '0' && k[1] <= '9') {
            int idx = k[1] - '0';
            if (idx < 0 || idx >= WEATHER_FORECAST_DAYS) continue;
            weather_forecast_t *fc = &w.forecast[idx];
            const char *field = k + 3;   /* 跳过 "dN_" */
            if      (strcmp(field, "date") == 0) copy_str(fc->date, sizeof(fc->date), v);
            else if (strcmp(field, "text") == 0) copy_str(fc->text_day, sizeof(fc->text_day), v);
            else if (strcmp(field, "high") == 0) fc->high = (float)atof(v);
            else if (strcmp(field, "low")  == 0) fc->low  = (float)atof(v);
        }
    }
    fclose(f);

    if (!has_city || !has_temp) {
        fprintf(stderr, "[weather] 缓存文件 %s 内容不完整，忽略\n", used ? used : "?");
        return -1;
    }
    if (w.forecast_days > WEATHER_FORECAST_DAYS) w.forecast_days = WEATHER_FORECAST_DAYS;

    w.now.valid = true;
    *out = w;
    printf("[weather] 已加载缓存天气: %s\n", used ? used : "?");
    return 0;
}

int weather_fetch(const char *key, const char *location, weather_data_t *out)
{
    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));

#if WEATHER_ENABLE_HTTP
    if (key != NULL && key[0] != '\0' && location != NULL && location[0] != '\0') {
        if (fetch_via_https(key, location, out) == 0) {
            out->src = WEATHER_SRC_HTTP;
            out->ts  = (long)time(NULL);
            return 0;
        }
        fprintf(stderr, "[weather] 直连心知失败，回退缓存/模拟\n");
    }
#else
    (void)key;
    (void)location;
#endif

    /* 板子没外网也要能显示真实天气 —— 主机抓、推上来、这里读。 */
    if (weather_cache_load(out) == 0) {
        out->src = WEATHER_SRC_CACHE;
        return 0;
    }

    weather_simulate(out);
    return 0;
}

void weather_simulate(weather_data_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));

    static int seeded = 0;
    if (!seeded) { srand((unsigned)time(NULL)); seeded = 1; }

    out->now.valid = true;
    /* 城市名不再带 "模拟:" 前缀（字库按 GB2312 收录，前缀只会在界面上碍眼），
     * 是否模拟由 out->src 单独表达，UI 有专门的"数据源"卡片。 */
    strncpy(out->now.city, "深圳", sizeof(out->now.city) - 1);
    const char *samples[] = {"晴", "多云", "阴", "小雨", "雷阵雨"};
    strncpy(out->now.text, samples[rand() % 5], sizeof(out->now.text) - 1);
    out->now.code = rand() % 30;
    out->now.temperature = (float)(15 + rand() % 20);
    out->now.humidity    = (float)(40 + rand() % 55);
    out->now.wind_speed  = (float)(rand() % 30);

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm)
        strftime(out->now.update_time, sizeof(out->now.update_time),
                 "%m-%d %H:%M", tm);
    else
        strncpy(out->now.update_time, "--", sizeof(out->now.update_time) - 1);

    out->forecast_days = WEATHER_FORECAST_DAYS;
    const char *fc[] = {"晴", "多云", "小雨"};
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        time_t ft = t + (i + 1) * 86400;
        struct tm *ftm = localtime(&ft);
        if (ftm)
            strftime(out->forecast[i].date, sizeof(out->forecast[i].date),
                     "%m-%d", ftm);
        strncpy(out->forecast[i].text_day, fc[i % 3],
                sizeof(out->forecast[i].text_day) - 1);
        out->forecast[i].high = (float)(20 + rand() % 12);
        out->forecast[i].low  = (float)(8 + rand() % 10);
    }

    out->src = WEATHER_SRC_SIM;
    out->ts  = (long)t;
}

const char *weather_src_name(weather_src_t src)
{
    switch (src) {
    case WEATHER_SRC_HTTP:  return "实况";
    case WEATHER_SRC_CACHE: return "缓存";
    case WEATHER_SRC_SIM:   return "模拟";
    default:                return "--";
    }
}

void weather_to_string(const weather_data_t *w, char *buf, size_t buflen)
{
    if (w == NULL || buf == NULL) return;
    snprintf(buf, buflen,
             "[weather] %s %s %.1fC 湿度%.0f%% 风%.0fkm/h 更新%s 来源=%s",
             w->now.city, w->now.text, (double)w->now.temperature,
             (double)w->now.humidity, (double)w->now.wind_speed,
             w->now.update_time, weather_src_name(w->src));
}
