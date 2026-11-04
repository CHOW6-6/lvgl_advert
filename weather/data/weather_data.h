#ifndef WEATHER_DATA_H
#define WEATHER_DATA_H

#include <stdbool.h>
#include <stddef.h>   /* size_t */

/* 数据来源。UI 上必须能区分"实况"和"模拟"，
 * 否则屏幕上数字跳得挺欢、其实全是随机数 —— 这种事得让人一眼看出来。 */
typedef enum {
    WEATHER_SRC_NONE = 0,  /* 还没有任何数据 */
    WEATHER_SRC_SIM,       /* 本地随机模拟 */
    WEATHER_SRC_CACHE,     /* 主机抓取心知后推到板上的缓存文件 */
    WEATHER_SRC_HTTP       /* 设备自己通过 HTTPS 直接拉心知（需 WEATHER_ENABLE_HTTP=1） */
} weather_src_t;

/* 单个城市的实时天气 + 短期预报 */
typedef struct {
    char   city[64];        /* 城市名 */
    char   text[32];        /* 天气现象，如 "多云" */
    int    code;            /* 心知天气现象代码 */
    float  temperature;     /* 当前温度 ℃ */
    float  humidity;        /* 相对湿度 % */
    float  wind_speed;      /* 风速 km/h */
    char   update_time[32]; /* 数据更新时间（UTC+8 字符串） */
    bool   valid;           /* 数据是否有效（拉取且解析成功） */
} weather_now_t;

typedef struct {
    char  date[16];         /* 日期 YYYY-MM-DD */
    char  text_day[32];     /* 白天天气现象 */
    float high;             /* 最高温 ℃ */
    float low;              /* 最低温 ℃ */
} weather_forecast_t;

#define WEATHER_FORECAST_DAYS 3

typedef struct {
    weather_now_t       now;
    weather_forecast_t  forecast[WEATHER_FORECAST_DAYS];
    int                 forecast_days;   /* 实际拿到的预报天数 */
    weather_src_t       src;             /* 数据来源，UI 据此显示"实况/缓存/模拟" */
    long                ts;              /* 数据生成时间（unix 秒），0 = 未知 */
} weather_data_t;

/*
 * 拉取最新天气。按优先级依次尝试，任一成功即返回：
 *   1. WEATHER_ENABLE_HTTP=1 且配置了心知私钥 -> 设备直连心知（HTTPS）
 *   2. 缓存文件（主机用 weather/tools/fetch_weather.py 抓好后推上来）
 *   3. weather_simulate() 本地模拟
 * 永远返回 0（至少能给出模拟数据），out->src 标明真实来源。
 */
int weather_fetch(const char *key, const char *location, weather_data_t *out);

/* 生成一份本地模拟天气（无网络 / 未配置密钥 / 没有缓存时用于 UI 联调）。 */
void weather_simulate(weather_data_t *out);

/* 把天气数据格式化为一行可读摘要（用于日志）。 */
void weather_to_string(const weather_data_t *w, char *buf, size_t buflen);

/* 数据来源的短名称："模拟" / "缓存" / "实况" / "--"，供 UI 直接显示。 */
const char *weather_src_name(weather_src_t src);

#endif /* WEATHER_DATA_H */
