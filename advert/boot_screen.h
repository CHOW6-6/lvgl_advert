#ifndef __BOOT_SCREEN_H__
#define __BOOT_SCREEN_H__

/**
 * 开机动画（DeepSeek Harness 风格）
 *
 * 六幕连贯：
 *   第一幕  黑幕 + DeepSeek 鲸鱼标识 / 深度求索 / DEEPSEEK（扫光）
 *   过场    过曝洗白（黑 → 灰 → 暖白底板）
 *   第二幕  暖白「技术档案」底板：左上品牌块、右下 POWERED BY、进度条
 *   序列    需要访问许可 → 身份资料确认 → 接收访问需求 → 开始读取权限
 *           → 读取完毕·核验通过；每步中文大字 + 英文等宽副标题
 *   交互    第 1、2 步等待用户确认（点按钮 / 触摸任意处）
 *   终幕    WELCOME TO / DEEPSEEK HARNESS / 鲸鱼标识，色散故障后揭幕
 *
 * 设计还原自开源项目 dsh-startup-screen（https://github.com/6shenhonghong9/dsh-startup-screen）
 * 的原版 Web 动画，用 LVGL v8 在 GEC6818（800x480 fbdev + evdev 触摸）上重实现。
 * 原版依赖浏览器 WebAudio 与 TTS 女声，本版在嵌入式环境下去掉音频，
 * 保留完整的视觉序列、交互授权、进度条、环形准星与色散故障效果。
 */

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 播放开机动画。动画结束后自动调用 next() 进入下一屏（通常是登录界面）。
 *
 * @param next  动画播完后的回调（可为 NULL，表示播完停在欢迎画面）。
 *              建议传 btn_wind 进入登录界面。
 */
void boot_screen_show(void (*next)(void));

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_SCREEN_H__ */
