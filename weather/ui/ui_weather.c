#include "ui_weather.h"
#include "weather_app.h"
#include "iot_client.h"
#include "ad_fonts.h"           /* advert/ad_fonts.h：GB2312 全覆盖的 14/16/20px 中文字库 */
#include "lvgl/lvgl.h"
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * 字体约定（这就是"中文字全变成方块"的根因）
 *
 * lv_conf.h 里的 LV_FONT_DEFAULT 是 lv_font_montserrat_14 —— 它只有 ASCII，
 * 没有任何汉字字形。以前这里所有标签都靠默认字体，于是 LVGL 对每个汉字都报
 *   lv_draw_letter: glyph dsc. not found for U+5929
 * 屏幕上只剩一排空心方块。
 *
 * 项目里早就有烤好的 GB2312 全集中文字库（advert/fonts/，广告界面在用），
 * 这里直接按字号取用，一个字都不用重新生成：
 *   ad_font_20 —— 标题、城市名
 *   ad_font_16 —— 正文、数值、预报
 *   ad_font_14 —— 卡片小标题
 *   lv_font_montserrat_48 —— 大号温度数字（只有数字和 °C，不需要汉字）
 * ------------------------------------------------------------------------- */
#define F_TITLE (&ad_font_20)
#define F_BODY  (&ad_font_16)
#define F_SMALL (&ad_font_14)
#define F_TEMP  (&lv_font_montserrat_48)

/* 配色 */
#define C_BG        lv_color_make(0x0f, 0x16, 0x22)
#define C_CARD      lv_color_make(0x1f, 0x29, 0x37)
#define C_MAIN      lv_color_make(0x16, 0x3a, 0x5f)
#define C_INK       lv_color_make(0xff, 0xff, 0xff)
#define C_INK2      lv_color_make(0x9f, 0xb3, 0xc8)
#define C_ACCENT    lv_color_make(0xbf, 0xd7, 0xff)
#define C_OK        lv_color_make(0x2e, 0xc1, 0x9d)
#define C_WARN      lv_color_make(0xff, 0xb0, 0x4a)
#define C_MUTED     lv_color_make(0x8a, 0x9a, 0xad)

/* UI 控件句柄（文件作用域，供刷新定时器更新） */
static lv_obj_t *g_lbl_city;
static lv_obj_t *g_lbl_temp;
static lv_obj_t *g_lbl_text;
static lv_obj_t *g_lbl_hum;
static lv_obj_t *g_lbl_wind;
static lv_obj_t *g_lbl_update;
static lv_obj_t *g_lbl_source;
static lv_obj_t *g_lbl_status;
static lv_obj_t *g_lbl_fc_title;
static lv_obj_t *g_lbl_forecast;

/* 定时器：从全局 g_weather 拷贝最新值并更新界面（在 LVGL 主线程执行，安全） */
static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    weather_data_t w;
    pthread_mutex_lock(&g_weather_mtx);
    w = g_weather;
    pthread_mutex_unlock(&g_weather_mtx);

    char buf[64];

    lv_label_set_text(g_lbl_city, w.now.city[0] ? w.now.city : "--");

    snprintf(buf, sizeof(buf), "%.0f°C", (double)w.now.temperature);
    lv_label_set_text(g_lbl_temp, buf);

    lv_label_set_text(g_lbl_text, w.now.text[0] ? w.now.text : "--");

    snprintf(buf, sizeof(buf), "%.0f%%", (double)w.now.humidity);
    lv_label_set_text(g_lbl_hum, buf);

    snprintf(buf, sizeof(buf), "%.0f km/h", (double)w.now.wind_speed);
    lv_label_set_text(g_lbl_wind, buf);

    lv_label_set_text(g_lbl_update, w.now.update_time[0] ? w.now.update_time : "--");

    /* 数据源：模拟数据必须一眼可见，别让人以为屏幕上跳的是真天气 */
    lv_label_set_text(g_lbl_source, weather_src_name(w.src));
    lv_obj_set_style_text_color(g_lbl_source,
        w.src == WEATHER_SRC_SIM ? C_WARN : (w.src == WEATHER_SRC_NONE ? C_MUTED : C_OK), 0);

    /* 未来几天预报 */
    if (w.forecast_days > 0) {
        snprintf(buf, sizeof(buf), "近 %d 天预报", w.forecast_days);
        lv_label_set_text(g_lbl_fc_title, buf);

        char fbuf[192];
        int off = 0;
        for (int i = 0; i < w.forecast_days; i++) {
            off += snprintf(fbuf + off, sizeof(fbuf) - (size_t)off,
                            "%s  %s  %.0f/%.0f°%s",
                            w.forecast[i].date, w.forecast[i].text_day,
                            (double)w.forecast[i].high, (double)w.forecast[i].low,
                            (i + 1 < w.forecast_days) ? "\n" : "");
            if (off >= (int)sizeof(fbuf)) break;
        }
        lv_label_set_text(g_lbl_forecast, fbuf);
    } else {
        lv_label_set_text(g_lbl_fc_title, "未来预报");
        lv_label_set_text(g_lbl_forecast, "--");
    }

    /* 连接状态：编译期就没开上报时不要谎报"未连接"，那不是网络问题 */
    if (!iot_client_enabled()) {
        lv_label_set_text(g_lbl_status, "○ 云上报未启用");
        lv_obj_set_style_text_color(g_lbl_status, C_MUTED, 0);
    } else if (iot_client_is_connected()) {
        lv_label_set_text(g_lbl_status, "● 已连接华为云");
        lv_obj_set_style_text_color(g_lbl_status, C_OK, 0);
    } else {
        lv_label_set_text(g_lbl_status, "○ 未连接华为云");
        lv_obj_set_style_text_color(g_lbl_status, C_WARN, 0);
    }
}

/* 透明无边框的容器（用来做纯布局） */
static lv_obj_t *make_box(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_flex_flow_t flow)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* 带标题的小卡片：返回数值标签，便于后续更新 */
static lv_obj_t *make_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 186, 78);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, C_CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, F_SMALL, 0);
    lv_obj_set_style_text_color(t, C_INK2, 0);

    lv_obj_t *v = lv_label_create(card);
    lv_obj_set_style_text_font(v, F_BODY, 0);
    lv_obj_set_style_text_color(v, C_INK, 0);
    return v;
}

void ui_weather_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 10, 0);
    lv_obj_set_style_pad_row(scr, 10, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);   /* 800x480 一屏放得下，别出滚动条 */
    lv_scr_load(scr);

    /* 顶部：标题 + 连接状态 */
    lv_obj_t *top = make_box(scr, 780, 36, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, "天气监测中心");
    lv_obj_set_style_text_font(title, F_TITLE, 0);
    lv_obj_set_style_text_color(title, C_INK, 0);

    g_lbl_status = lv_label_create(top);
    lv_label_set_text(g_lbl_status, "○ 未连接华为云");
    lv_obj_set_style_text_font(g_lbl_status, F_BODY, 0);
    lv_obj_set_style_text_color(g_lbl_status, C_MUTED, 0);

    /* 主卡片：城市 + 大温度 + 天气现象 */
    lv_obj_t *main_card = lv_obj_create(scr);
    lv_obj_set_size(main_card, 780, 170);
    lv_obj_set_flex_flow(main_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(main_card, 16, 0);
    lv_obj_set_style_bg_color(main_card, C_MAIN, 0);
    lv_obj_set_style_border_width(main_card, 0, 0);
    lv_obj_set_style_pad_all(main_card, 8, 0);
    lv_obj_set_style_pad_row(main_card, 2, 0);
    lv_obj_clear_flag(main_card, LV_OBJ_FLAG_SCROLLABLE);

    g_lbl_city = lv_label_create(main_card);
    lv_label_set_text(g_lbl_city, "--");
    lv_obj_set_style_text_font(g_lbl_city, F_TITLE, 0);
    lv_obj_set_style_text_color(g_lbl_city, C_ACCENT, 0);

    g_lbl_temp = lv_label_create(main_card);
    lv_label_set_text(g_lbl_temp, "--°C");
    lv_obj_set_style_text_font(g_lbl_temp, F_TEMP, 0);
    lv_obj_set_style_text_color(g_lbl_temp, C_INK, 0);

    g_lbl_text = lv_label_create(main_card);
    lv_label_set_text(g_lbl_text, "--");
    lv_obj_set_style_text_font(g_lbl_text, F_BODY, 0);
    lv_obj_set_style_text_color(g_lbl_text, C_ACCENT, 0);

    /* 四个小卡片：湿度 / 风速 / 更新时间 / 数据源 */
    lv_obj_t *row = make_box(scr, 780, 78, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_lbl_hum    = make_card(row, "湿度");
    g_lbl_wind   = make_card(row, "风速");
    g_lbl_update = make_card(row, "更新时间");
    g_lbl_source = make_card(row, "数据源");

    /* 预报块 */
    lv_obj_t *fc_card = lv_obj_create(scr);
    lv_obj_set_size(fc_card, 780, 130);
    lv_obj_set_style_radius(fc_card, 12, 0);
    lv_obj_set_style_bg_color(fc_card, C_CARD, 0);
    lv_obj_set_style_border_width(fc_card, 0, 0);
    lv_obj_set_style_pad_all(fc_card, 10, 0);
    lv_obj_set_style_pad_row(fc_card, 4, 0);
    lv_obj_set_flex_flow(fc_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(fc_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(fc_card, LV_OBJ_FLAG_SCROLLABLE);

    g_lbl_fc_title = lv_label_create(fc_card);
    lv_label_set_text(g_lbl_fc_title, "未来预报");
    lv_obj_set_style_text_font(g_lbl_fc_title, F_SMALL, 0);
    lv_obj_set_style_text_color(g_lbl_fc_title, C_INK2, 0);

    g_lbl_forecast = lv_label_create(fc_card);
    lv_label_set_text(g_lbl_forecast, "--");
    lv_obj_set_style_text_font(g_lbl_forecast, F_BODY, 0);
    lv_obj_set_style_text_color(g_lbl_forecast, C_INK, 0);
    lv_label_set_long_mode(g_lbl_forecast, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_lbl_forecast, 760);

    /* 立即刷新一次，并启动定时器（每 1s 同步全局天气） */
    refresh_cb(NULL);
    lv_timer_create(refresh_cb, 1000, NULL);
}
