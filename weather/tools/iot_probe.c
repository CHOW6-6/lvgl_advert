/*
 * iot_probe.c —— 在电脑上验证「华为云 IoTDA 能不能连上、属性能不能报上去」。
 *
 * 为什么需要它：开发板上要么缺 ARM 版 SDK 库、要么没有外网，一上板就是黑盒，
 * 分不清是配置错、密码错、产品模型错还是网络不通。
 * 这个探针直接复用项目里真实的 iot_client.c / weather_data.c，
 * 用的是同一份 weather_conf.json，只是跑在 x86 上（lib/ 里那套 x86 版 SDK 库）。
 *
 * 用法（在项目根目录跑，因为它要读 ./conf/rootcert.pem 和 weather_cache.txt）：
 *     make iot-probe
 *     ./build/bin/iot-probe
 *
 * 退出码：0 = 连上并且上报成功；1 = 没连上；2 = 配置读不到。
 */

#include "iot_config.h"
#include "iot_client.h"
#include "weather_data.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static iot_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.location, sizeof(cfg.location), "%s", "深圳");
    cfg.report_interval_ms = 5000;

    if (iot_config_load(NULL, &cfg) != 0) {
        fprintf(stderr, "[probe] 读不到 weather_conf.json（在项目根目录跑）\n");
        return 2;
    }
    printf("================ 华为云连通性探针 ================\n");
    iot_config_print(&cfg);

    if (iot_client_init(&cfg) != 0) {
        fprintf(stderr, "[probe] iot_client_init 失败\n");
        return 1;
    }

    int rc = iot_client_connect();
    printf("[probe] IOTA_Connect() -> %d，等待连接建立…\n", rc);

    int connected = 0;
    for (int i = 0; i < 30; i++) {          /* 最多等 15 秒 */
        if (iot_client_is_connected()) { connected = 1; break; }
        usleep(500 * 1000);
    }
    if (!connected) {
        fprintf(stderr, "[probe] ✗ 15 秒内没连上。常见原因：\n"
                        "        1) deviceId/secret 不匹配（控制台重置过密钥）\n"
                        "        2) 本机到 8883 不通 / 被防火墙拦\n"
                        "        3) 设备在平台上被禁用\n");
        iot_client_destroy();
        return 1;
    }
    printf("[probe] ✓ 已连接华为云 IoTDA\n");

    /* 取一份天气（优先用主机抓好的缓存，没有就模拟），然后上报 */
    weather_data_t w;
    weather_fetch(cfg.seniverse_key, cfg.location, &w);
    char line[192];
    weather_to_string(&w, line, sizeof(line));
    printf("[probe] %s\n", line);

    int msgId = iot_client_report_weather(&w);
    printf("[probe] IOTA_PropertiesReport() -> msgId=%d %s\n",
           msgId, msgId >= 0 ? "（已提交，看上面日志里的\"发布成功\"）" : "（失败）");

    sleep(3);
    iot_client_destroy();
    printf("================ 探针结束 ================\n");
    return msgId >= 0 ? 0 : 1;
}
