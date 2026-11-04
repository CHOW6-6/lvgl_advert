/**
 * @file advert_ui.h
 * @brief 登录成功后的多功能广告机界面
 *
 * 三页：播放 / 广告库 / 信息。整体走「清新简约」：冷白底、薄荷绿点缀、
 * 细描边、大留白，不做重阴影与渐变堆叠，保证 6818 这种没有 GPU 的板子
 * 也能跑得动（软件渲染，每帧都是 CPU 在算）。
 */

#ifndef ADVERT_UI_H
#define ADVERT_UI_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 创建并显示广告机界面（会清空当前屏幕） */
void advert_ui_show(void);

/* 销毁界面与所有定时器（退出登录时调用） */
void advert_ui_close(void);

#ifdef __cplusplus
}
#endif

#endif /* ADVERT_UI_H */
