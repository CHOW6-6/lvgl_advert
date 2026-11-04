#include "weather_app.h"
#include "iot_config.h"
#include "iot_client.h"
#include "weather_data.h"
#include "ui_weather.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

weather_data_t    g_weather;
pthread_mutex_t   g_weather_mtx = PTHREAD_MUTEX_INITIALIZER;

static iot_config_t   g_cfg;
static volatile int   g_stop = 0;
static pthread_t      g_worker;

/* 后台线程：周期性拉取天气（真实或模拟），写入 g_weather；
 * 若已连上华为云，则同步上报属性。 */
static void *weather_worker(void *arg)
{
    (void)arg;
    while (!g_stop) {
        weather_data_t w;
        memset(&w, 0, sizeof(w));

        weather_fetch(g_cfg.seniverse_key, g_cfg.location, &w);

        pthread_mutex_lock(&g_weather_mtx);
        g_weather = w;
        pthread_mutex_unlock(&g_weather_mtx);

        char log[192];
        weather_to_string(&w, log, sizeof(log));
        printf("%s\n", log);

        /* 未连接时也要调一次上报：内置 MQTT 后端会在这里自己重连
         * （它内部有 20 秒退避），否则"启动时正好连不上"就永远不再试了。
         * 华为 SDK 后端在未连接时直接返回 -2，没有任何副作用。 */
        int id = iot_client_report_weather(&w);
        if (id < 0 && id != -2)
            printf("[weather] 上报失败, msgId=%d\n", id);

        /* 首次立即拉取，之后按周期 */
        usleep((useconds_t)g_cfg.report_interval_ms * 1000);
    }
    return NULL;
}

int weather_app_init(void)
{
    /* 板端日志重定向到文件时，stdout 默认是 4KB 全缓冲 —— 程序刚起来那几十行
     * 一条都看不到，排查时很容易误判成"根本没执行到这儿"。改成行缓冲。 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* 先把默认值写好：iot_config_load() 失败时不会碰 g_cfg，
     * 而 g_cfg 是 static（全零）—— report_interval_ms=0 会让下面的 usleep(0)
     * 变成"零延迟死循环"，把 CPU 直接打满。这个坑必须先堵上。 */
    memset(&g_cfg, 0, sizeof(g_cfg));
    snprintf(g_cfg.location, sizeof(g_cfg.location), "%s", "广州");
    g_cfg.report_interval_ms = 5000;

    if (iot_config_load(NULL, &g_cfg) != 0) {
        fprintf(stderr, "[weather] 加载配置失败，沿用默认值（广州 / 5s）\n");
    }
    if (g_cfg.report_interval_ms < 1000) {
        fprintf(stderr, "[weather] reportIntervalMs=%d 过小，回退为 5000\n",
                g_cfg.report_interval_ms);
        g_cfg.report_interval_ms = 5000;
    }
    iot_config_print(&g_cfg);

    printf("[weather] 云上报: %s\n", iot_client_enabled() ? "已启用" : "未启用（编译期关闭）");
    printf("[weather] 天气数据: %s\n",
           g_cfg.seniverse_key[0] ? "优先缓存/直连心知，失败回退模拟"
                                  : "无密钥，使用缓存或本地模拟");

    /* 初始化 IoT（连接失败不致命：UI 仍可显示天气） */
    if (iot_client_init(&g_cfg) == 0) {
        int r = iot_client_connect();
        printf("[weather] IOTA_Connect() -> %d\n", r);
    } else {
        printf("[weather] IoT 初始化失败，仅本地显示天气\n");
    }

    /* 启动后台拉取线程 */
    if (pthread_create(&g_worker, NULL, weather_worker, NULL) != 0) {
        fprintf(stderr, "[weather] 创建后台线程失败\n");
        return -1;
    }
    return 0;
}

void weather_app_show_ui(void)
{
    ui_weather_create();
}

/* 供 main 退出时调用（可选） */
void weather_app_stop(void)
{
    g_stop = 1;
    pthread_join(g_worker, NULL);
    iot_client_destroy();
}
