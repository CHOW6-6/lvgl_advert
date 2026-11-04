#ifndef WEATHER_APP_H
#define WEATHER_APP_H

#include "weather_data.h"
#include <pthread.h>

/*
 * 天气应用顶层胶水层。
 *  - 持有“最新天气”全局副本 g_weather（后台线程写入，UI 线程定时读取，互斥保护）；
 *  - weather_app_init() 负责加载配置、启动后台拉取线程、初始化并连接华为云 IoT；
 *  - weather_app_show_ui() 创建“天气监测中心”界面并启动刷新定时器。
 */

/* 全局最新天气（后台线程更新，UI 线程只读并加锁拷贝） */
extern weather_data_t g_weather;
extern pthread_mutex_t g_weather_mtx;

/* 初始化天气应用：加载配置、启动后台天气拉取线程、初始化并连接 IoT（连接失败不致命）。 */
int weather_app_init(void);

/* 创建“天气监测中心”界面（LVGL 屏幕）。 */
void weather_app_show_ui(void);

/* 停止后台线程并释放 IoT 资源（退出程序前可选调用）。 */
void weather_app_stop(void);

#endif /* WEATHER_APP_H */
