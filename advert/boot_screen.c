/**
 * @file boot_screen.c
 * @brief DeepSeek Harness 风格开机动画（LVGL v8 实现）
 *
 * 移植自开源项目 dsh-startup-screen（https://github.com/6shenhonghong9/dsh-startup-screen）
 * 的 Web 版启动动画。原版依赖浏览器 WebAudio 与 TTS 女声，本版在嵌入式
 * 环境下去掉音频，保留完整视觉序列、交互授权、进度条、环形准星与色散故障。
 *
 * 目标平台：GEC6818（800x480 / LVGL v8 / fbdev + evdev 触摸）。
 *
 * 序列（与 Web 版一致的六幕）：
 *   [1] 黑幕 + 鲸鱼标识 + 深度求索 / DEEPSEEK（扫光掠过）
 *   [2] 过曝洗白（黑 → 灰 → 暖白）
 *   [3] 暖白技术档案底板（左上品牌块 / 右下 POWERED BY / 底部进度条 + 环形准星）
 *   [4] 五步授权序列（第 1、2 步需用户点按钮确认，3~5 步自动推进）
 *   [5] 欢迎揭幕（WELCOME TO / DEEPSEEK HARNESS / 色散故障）
 *   [6] 自动进入登录界面（回调 next）
 */

#include "boot_screen.h"
extern const lv_font_t boot_zh_font;  /* 动画专用中文字库（boot_zh_font.c 提供） */

#include <string.h>

/* DeepSeek 鲸鱼标识位图（whale_logo.c 提供，64x64 / ALPHA_1BIT） */
LV_IMG_DECLARE(whale_logo);

/* ============================ 配置 ==================================== */

/* 主题配色（取自 Web 版 DEFAULTS） */
#define ACCENT_COLOR    lv_color_make(0xFF, 0x75, 0x00)  /* #ff7500 橙 */
#define GLITCH_A        lv_color_make(0xFF, 0x3B, 0x30)  /* #ff3b30 红 */
#define GLITCH_B        lv_color_make(0x29, 0x71, 0xB8)  /* #2971b8 蓝 */
#define WARM_WHITE      lv_color_make(0xF7, 0xF4, 0xEF)  /* 暖白底板 */
#define WARM_WHITE_2    lv_color_make(0xE6, 0xE0, 0xD4)
#define INK_COLOR       lv_color_make(0x1A, 0x1A, 0x1A)  /* 底板上的近黑文字 */
#define GRAY_TEXT       lv_color_make(0x6A, 0x6A, 0x6A)

/* 节奏（毫秒，speed=1） */
#define T_LOGO_HOLD     1000    /* 黑幕鲸鱼标识停留 */
#define T_OVEREXPOSE    700     /* 过曝洗白过渡时长 */
#define T_STEP_AUTO     1500    /* 自动步骤停留 */
#define T_STEP_LAST     1900    /* 最后一步停留 */
#define T_WELCOME_HOLD  2400    /* 欢迎画面停留 */

/* 五步授权脚本（与 Web 版 STEPS 一致，去掉音频字段） */
typedef struct {
    const char *zh;         /* 中文主字 */
    const char *en;         /* 英文副标题 */
    const char *log;        /* 底部日志 */
    const char *badge;      /* 角标 */
    int         interactive;/* 1=需用户点按钮确认，0=自动推进 */
    const char *btn_zh;     /* 确认按钮文案 */
    int         progress;   /* 进度（千分比，0..1000） */
} boot_step_t;

static const boot_step_t steps[] = {
    { "需要访问许可",     "ACCESS PERMISSION REQUIRED",   "等待用户授权…",             "AUTHORIZATION", 1, "授予访问权限", 340 },
    { "身份资料确认",     "IDENTITY VERIFICATION",        "核对身份资料…",             "IDENTITY",      1, "确认身份",     500 },
    { "接收访问需求",     "ACCESS REQUEST RECEIVED",      "接收访问需求，建立会话通道…", "REQUEST",       0, NULL,         660 },
    { "开始读取权限",     "PERMISSION READ STARTED",      "开始读取权限，索引工作区与会话…", "READ",      0, NULL,         800 },
    { "读取完毕 · 核验通过", "READ COMPLETE - VERIFIED",  "读取完毕，核验通过，权限已授予", "VERIFY",        0, NULL,         930 },
};
#define STEP_COUNT  ((int)(sizeof(steps) / sizeof(steps[0])))

/* ============================ 内部状态 ================================ */

typedef enum {
    STAGE_LOGO = 0,     /* 黑幕 + 鲸鱼标识 + 扫光 */
    STAGE_WASH,         /* 过曝洗白 */
    STAGE_STEPS,        /* 五步授权序列（底板在下方） */
    STAGE_WELCOME,      /* 欢迎揭幕 + 色散故障 */
    STAGE_DONE          /* 已回调 next，等待销毁 */
} boot_stage_t;

typedef struct {
    lv_obj_t *screen;           /* 动画根容器 */

    /* 第一幕：黑幕 + Logo */
    lv_obj_t *logo_layer;       /* 黑幕容器 */
    lv_obj_t *logo_img;         /* 鲸鱼标识（位图） */
    lv_obj_t *logo_org_zh;      /* 深度求索 */
    lv_obj_t *logo_org_en;      /* DEEPSEEK */
    lv_obj_t *scan_line;        /* 扫光竖条 */

    /* 第二幕：过曝洗白遮罩 */
    lv_obj_t *wash_layer;

    /* 第三幕：暖白档案底板 */
    lv_obj_t *plate_layer;
    lv_obj_t *plate_progress_fg;/* 进度条前景 */
    lv_obj_t *reticle;          /* 环形准星画布 */

    /* 第四幕：授权序列 */
    lv_obj_t *step_layer;
    lv_obj_t *step_zh;
    lv_obj_t *step_en;
    lv_obj_t *step_log;
    lv_obj_t *step_badge;
    lv_obj_t *step_btn;
    lv_obj_t *step_btn_label;

    /* 终幕：欢迎 */
    lv_obj_t *welcome_layer;
    lv_obj_t *glitch_red;
    lv_obj_t *glitch_blue;

    boot_stage_t stage;
    void (*next)(void);
    int cur_step;
    lv_timer_t *anim_timer;
    uint32_t stage_elapsed;
    int scan_phase;
    int reticle_angle;
    int glitch_tick;
} boot_ctx_t;

static boot_ctx_t g_boot;
static bool g_boot_running = false;

/* ============================ 工具函数 ================================ */

static lv_obj_t *make_rect(lv_obj_t *parent, lv_color_t color, lv_coord_t x, lv_coord_t y,
                           lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                            lv_color_t color, lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, align, 0);
    return l;
}

/* ============================ 环形准星 ================================ */

/* 自转的环形准星：一圈圆环，缺口随角度旋转 */
static void reticle_update(void)
{
    lv_obj_t *c = g_boot.reticle;
    if (c == NULL) return;

    lv_coord_t w = lv_obj_get_width(c);
    lv_coord_t h = lv_obj_get_height(c);

    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_opa = LV_OPA_TRANSP;
    lv_canvas_draw_rect(c, 0, 0, w, h, &bg);

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = ACCENT_COLOR;
    arc.width = 4;
    arc.rounded = 1;

    int angle = g_boot.reticle_angle % 360;
    int gap = 40;                       /* 缺口角度 */
    int start = angle + gap / 2;        /* 缺口随角度一起转 */
    lv_coord_t r = (w < h ? w : h) / 2 - 4;
    lv_point_t center = { w / 2, h / 2 };
    lv_canvas_draw_arc(c, center.x, center.y, r, start, 360 - gap + start, &arc);
}

/* ============================ 各幕构建 ================================ */

static void build_logo_stage(void)
{
    lv_obj_t *layer = make_rect(g_boot.screen, lv_color_black(), 0, 0, 800, 480);
    g_boot.logo_layer = layer;

    /* 鲸鱼标识（白色 1-bit 位图，居中偏上） */
    g_boot.logo_img = lv_img_create(layer);
    lv_img_set_src(g_boot.logo_img, &whale_logo);
    lv_obj_set_style_img_recolor(g_boot.logo_img, lv_color_white(), 0);
    lv_obj_set_style_img_recolor_opa(g_boot.logo_img, LV_OPA_COVER, 0);
    lv_obj_align(g_boot.logo_img, LV_ALIGN_CENTER, 0, -50);

    /* 深度求索 */
    g_boot.logo_org_zh = make_label(layer, "深度求索", &boot_zh_font,
                                    lv_color_white(), LV_TEXT_ALIGN_CENTER);
    lv_obj_align(g_boot.logo_org_zh, LV_ALIGN_CENTER, 0, 90);

    /* DEEPSEEK（字距放宽） */
    g_boot.logo_org_en = make_label(layer, "D E E P S E E K", &lv_font_montserrat_24,
                                    lv_color_white(), LV_TEXT_ALIGN_CENTER);
    lv_obj_align(g_boot.logo_org_en, LV_ALIGN_CENTER, 0, 130);

    /* 扫光竖条（初始在屏幕外） */
    g_boot.scan_line = make_rect(layer, lv_color_white(), -40, 0, 3, 480);
    lv_obj_set_style_opa(g_boot.scan_line, LV_OPA_30, 0);

    /* 洗白层：先透明覆盖，STAGE_WASH 时逐步变白 */
    g_boot.wash_layer = make_rect(g_boot.screen, lv_color_white(), 0, 0, 800, 480);
    lv_obj_set_style_opa(g_boot.wash_layer, LV_OPA_TRANSP, 0);
}

static void build_plate_stage(void)
{
    lv_obj_t *layer = make_rect(g_boot.screen, WARM_WHITE, 0, 0, 800, 480);
    g_boot.plate_layer = layer;
    lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);

    /* 左上品牌块 */
    lv_obj_t *brand = make_rect(layer, INK_COLOR, 24, 24, 250, 64);
    lv_obj_t *b1 = make_label(brand, "DEEPSEEK", &lv_font_montserrat_20,
                              lv_color_white(), LV_TEXT_ALIGN_LEFT);
    lv_obj_align(b1, LV_ALIGN_TOP_LEFT, 16, 8);
    lv_obj_t *b2 = make_label(brand, "SYNTHESIZE INTELLIGENCE", &lv_font_montserrat_12,
                              WARM_WHITE_2, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(b2, LV_ALIGN_BOTTOM_LEFT, 16, -8);

    /* 右下 POWERED BY */
    lv_obj_t *powered = make_label(layer, "POWERED BY DEEPSEEK", &lv_font_montserrat_14,
                                   GRAY_TEXT, LV_TEXT_ALIGN_RIGHT);
    lv_obj_align(powered, LV_ALIGN_BOTTOM_RIGHT, -24, -24);

    /* 进度条 */
    lv_obj_t *pbg = make_rect(layer, WARM_WHITE_2, 80, 400, 640, 6);
    lv_obj_set_style_radius(pbg, 3, 0);
    g_boot.plate_progress_fg = make_rect(layer, ACCENT_COLOR, 80, 400, 0, 6);
    lv_obj_set_style_radius(g_boot.plate_progress_fg, 3, 0);

    /* 环形准星画布（底板中央偏上，缓慢自转） */
    static lv_color_t reticle_buf[80 * 80];
    lv_obj_t *reticle = lv_canvas_create(layer);
    lv_canvas_set_buffer(reticle, reticle_buf, 80, 80, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(reticle, LV_ALIGN_CENTER, 0, -40);
    g_boot.reticle = reticle;
    g_boot.reticle_angle = 0;
    reticle_update();
}

static void build_steps_stage(void)
{
    lv_obj_t *layer = make_rect(g_boot.screen, WARM_WHITE, 0, 0, 800, 480);
    g_boot.step_layer = layer;
    lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);

    g_boot.step_badge = make_label(layer, "AUTHORIZATION", &lv_font_montserrat_14,
                                   ACCENT_COLOR, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(g_boot.step_badge, LV_ALIGN_TOP_LEFT, 40, 48);

    g_boot.step_zh = make_label(layer, "", &boot_zh_font,
                                INK_COLOR, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(g_boot.step_zh, LV_ALIGN_CENTER, 0, -60);

    g_boot.step_en = make_label(layer, "", &lv_font_montserrat_16, GRAY_TEXT, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(g_boot.step_en, LV_ALIGN_CENTER, 0, -20);

    g_boot.step_log = make_label(layer, "", &boot_zh_font, GRAY_TEXT, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(g_boot.step_log, LV_ALIGN_BOTTOM_MID, 0, -70);

    g_boot.step_btn = lv_btn_create(layer);
    lv_obj_set_size(g_boot.step_btn, 200, 52);
    lv_obj_align(g_boot.step_btn, LV_ALIGN_BOTTOM_MID, 0, -110);
    lv_obj_set_style_bg_color(g_boot.step_btn, INK_COLOR, 0);
    lv_obj_set_style_bg_color(g_boot.step_btn, INK_COLOR, LV_STATE_PRESSED);
    g_boot.step_btn_label = lv_label_create(g_boot.step_btn);
    lv_label_set_text(g_boot.step_btn_label, "授予访问权限");
    lv_obj_set_style_text_font(g_boot.step_btn_label, &boot_zh_font, 0);
    lv_obj_set_style_text_color(g_boot.step_btn_label, lv_color_white(), 0);
    lv_obj_center(g_boot.step_btn_label);
    lv_obj_add_flag(g_boot.step_btn, LV_OBJ_FLAG_HIDDEN);
}

static void build_welcome_stage(void)
{
    lv_obj_t *layer = make_rect(g_boot.screen, lv_color_black(), 0, 0, 800, 480);
    g_boot.welcome_layer = layer;
    lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *wt = make_label(layer, "WELCOME TO", &lv_font_montserrat_18,
                              lv_color_white(), LV_TEXT_ALIGN_CENTER);
    lv_obj_align(wt, LV_ALIGN_CENTER, 0, -70);

    lv_obj_t *box = make_label(layer, "[ DEEPSEEK HARNESS ]", &lv_font_montserrat_28,
                               ACCENT_COLOR, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *wc = lv_img_create(layer);
    lv_img_set_src(wc, &whale_logo);
    lv_obj_set_style_img_recolor(wc, lv_color_white(), 0);
    lv_obj_set_style_img_recolor_opa(wc, LV_OPA_COVER, 0);
    lv_obj_align(wc, LV_ALIGN_CENTER, 0, 80);

    /* 色散故障层（覆盖整屏的透明红/蓝错位层） */
    g_boot.glitch_red = make_rect(g_boot.screen, GLITCH_A, 0, 0, 800, 480);
    g_boot.glitch_blue = make_rect(g_boot.screen, GLITCH_B, 0, 0, 800, 480);
    lv_obj_set_style_opa(g_boot.glitch_red, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(g_boot.glitch_blue, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(g_boot.glitch_red, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_boot.glitch_blue, LV_OBJ_FLAG_HIDDEN);
}

/* ============================ 事件回调 ================================ */

static void advance_step(void);
static void show_cur_step(void);

static void step_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    g_boot.stage_elapsed = 0;
    advance_step();   /* 推进到下一步并更新显示 */
}

/* ============================ 主状态机 ================================ */

static void show_stage(boot_stage_t s);

static void show_cur_step(void)
{
    const boot_step_t *st = &steps[g_boot.cur_step];
    lv_label_set_text(g_boot.step_zh, st->zh);
    lv_label_set_text(g_boot.step_en, st->en);
    lv_label_set_text(g_boot.step_log, st->log);
    lv_label_set_text(g_boot.step_badge, st->badge);

    if (st->interactive) {
        lv_label_set_text(g_boot.step_btn_label, st->btn_zh);
        lv_obj_clear_flag(g_boot.step_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_boot.step_btn, LV_OBJ_FLAG_HIDDEN);
    }

    lv_coord_t pw = (lv_coord_t)(st->progress * 640 / 1000);
    lv_obj_set_size(g_boot.plate_progress_fg, pw, 6);
}

static void advance_step(void)
{
    g_boot.cur_step++;
    if (g_boot.cur_step >= STEP_COUNT) {
        show_stage(STAGE_WELCOME);
        return;
    }
    show_cur_step();
}

static void show_stage(boot_stage_t s)
{
    g_boot.stage = s;
    g_boot.stage_elapsed = 0;

    lv_obj_add_flag(g_boot.logo_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_boot.plate_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_boot.step_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_boot.welcome_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(g_boot.wash_layer, LV_OPA_TRANSP, 0);

    switch (s) {
    case STAGE_LOGO:
        lv_obj_clear_flag(g_boot.logo_layer, LV_OBJ_FLAG_HIDDEN);
        break;
    case STAGE_WASH:
        lv_obj_clear_flag(g_boot.logo_layer, LV_OBJ_FLAG_HIDDEN);
        break;
    case STAGE_STEPS:
        lv_obj_clear_flag(g_boot.plate_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_boot.step_layer, LV_OBJ_FLAG_HIDDEN);
        g_boot.cur_step = 0;
        show_cur_step();
        break;
    case STAGE_WELCOME:
        lv_obj_clear_flag(g_boot.welcome_layer, LV_OBJ_FLAG_HIDDEN);
        break;
    default:
        break;
    }
}

static void anim_tick(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    uint32_t dt = 30;
    g_boot.stage_elapsed += dt;

    switch (g_boot.stage) {

    case STAGE_LOGO: {
        g_boot.scan_phase += 40;
        lv_coord_t x = -40 + (g_boot.scan_phase % 900);
        lv_obj_set_pos(g_boot.scan_line, x, 0);

        if (g_boot.stage_elapsed >= T_LOGO_HOLD) {
            show_stage(STAGE_WASH);
        }
        break;
    }

    case STAGE_WASH: {
        uint32_t e = g_boot.stage_elapsed;
        uint32_t dur = T_OVEREXPOSE;
        if (e >= dur) {
            lv_obj_set_style_opa(g_boot.wash_layer, LV_OPA_COVER, 0);
            show_stage(STAGE_STEPS);
        } else {
            int opa = (int)((uint32_t)LV_OPA_COVER * e / dur);
            lv_obj_set_style_opa(g_boot.wash_layer, opa, 0);
        }
        break;
    }

    case STAGE_STEPS: {
        g_boot.reticle_angle = (g_boot.reticle_angle + 3) % 360;
        reticle_update();

        const boot_step_t *st = &steps[g_boot.cur_step];
        uint32_t hold = (g_boot.cur_step == STEP_COUNT - 1) ? T_STEP_LAST : T_STEP_AUTO;
        if (!st->interactive && g_boot.stage_elapsed >= hold) {
            advance_step();
        }
        break;
    }

    case STAGE_WELCOME: {
        g_boot.glitch_tick++;
        if (g_boot.glitch_tick < 10) {
            if (g_boot.glitch_tick % 2 == 0) {
                lv_obj_clear_flag(g_boot.glitch_red, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(g_boot.glitch_blue, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_opa(g_boot.glitch_red, LV_OPA_20, 0);
                lv_obj_set_style_opa(g_boot.glitch_blue, LV_OPA_20, 0);
                lv_obj_set_pos(g_boot.glitch_red, -4, 0);
                lv_obj_set_pos(g_boot.glitch_blue, 4, 0);
            } else {
                lv_obj_add_flag(g_boot.glitch_red, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(g_boot.glitch_blue, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(g_boot.glitch_red, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_boot.glitch_blue, LV_OBJ_FLAG_HIDDEN);
        }

        if (g_boot.stage_elapsed >= T_WELCOME_HOLD) {
            g_boot.stage = STAGE_DONE;
            if (g_boot.anim_timer) {
                lv_timer_del(g_boot.anim_timer);
                g_boot.anim_timer = NULL;
            }
            if (g_boot.screen) {
                lv_obj_del(g_boot.screen);
                g_boot.screen = NULL;
            }
            g_boot_running = false;
            if (g_boot.next) g_boot.next();
        }
        break;
    }

    default:
        break;
    }
}

/* ============================ 对外接口 ================================ */

void boot_screen_show(void (*next)(void))
{
    if (g_boot_running) return;
    g_boot_running = true;

    memset(&g_boot, 0, sizeof(g_boot));
    g_boot.next = next;
    g_boot.stage = STAGE_LOGO;
    g_boot.cur_step = 0;

    g_boot.screen = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(g_boot.screen);
    lv_obj_set_size(g_boot.screen, 800, 480);
    lv_obj_set_pos(g_boot.screen, 0, 0);
    lv_obj_clear_flag(g_boot.screen, LV_OBJ_FLAG_SCROLLABLE);

    build_logo_stage();
    build_plate_stage();
    build_steps_stage();
    build_welcome_stage();

    lv_obj_add_event_cb(g_boot.step_btn, step_btn_cb, LV_EVENT_CLICKED, NULL);

    g_boot.anim_timer = lv_timer_create(anim_tick, 30, NULL);
}
