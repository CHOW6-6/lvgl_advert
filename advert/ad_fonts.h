/**
 * @file ad_fonts.h
 * @brief 中文点阵字库声明（由 tools/gen_fonts.sh 用 lv_font_conv 生成）
 *
 * 只保留 4 个字号，避免把 flash 塞爆：
 *   14 —— 角标、说明
 *   16 —— 小标题
 *   20 —— 正文 / 广告标题（全 GB2312，能显示任意上传标题）
 *   24 —— 品牌名、页面大标题（只含界面固定用字）
 */

#ifndef AD_FONTS_H
#define AD_FONTS_H

#include "lvgl/lvgl.h"

LV_FONT_DECLARE(ad_font_14);
LV_FONT_DECLARE(ad_font_16);
LV_FONT_DECLARE(ad_font_20);
LV_FONT_DECLARE(ad_font_24);

#endif /* AD_FONTS_H */
