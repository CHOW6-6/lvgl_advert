/**
 * @file advert_ui.c
 * @brief 多功能广告机界面（LVGL v8 / 800x480 / GEC6818）
 *
 * 设计取向「清新简约」：
 *   · 冷白底 #F4F7F9 + 纯白卡片，靠 1px 细描边分隔，不用重阴影
 *   · 唯一强调色是薄荷绿 #3EBFB0，只出现在激活态、进度条、角标上
 *   · 中文只有四级字号，英文副标用 Montserrat，层级靠颜色不靠描边
 *   · 大留白，卡片圆角统一 14px
 *
 * 为什么这么克制：6818 是纯软件渲染（CPU 逐像素往 framebuffer 里写），
 * 阴影、大面积渐变、全屏遮罩都是逐像素成本。界面上一旦有大面积动效，
 * 帧率立刻掉下来，所以动效只保留"换广告时的一次白闪溶解"。
 *
 * 时序注意：所有"删除自己正在处理的对象"的操作（切页、重建、退出登录）
 * 都走 defer_call() 推迟到下一次 lv_timer_handler，绝不在事件回调里
 * 直接把自己所在的容器 clean 掉 —— 那是经典的 use-after-free。
 */

#include "advert_ui.h"
#include "ad_store.h"
#include "ad_fonts.h"
#include "login.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

/* ======================= 配色 ======================= */

#define C_BG         lv_color_hex(0xF4F7F9)
#define C_SURFACE    lv_color_hex(0xFFFFFF)
#define C_INK        lv_color_hex(0x1F2A33)
#define C_INK2       lv_color_hex(0x5A6B7A)
#define C_MUTED      lv_color_hex(0x93A3B1)
#define C_LINE       lv_color_hex(0xE7EEF3)
#define C_MINT       lv_color_hex(0x3EBFB0)
#define C_MINT_DEEP  lv_color_hex(0x2A9D90)
#define C_MINT_SOFT  lv_color_hex(0xE4F5F2)
#define C_AMBER      lv_color_hex(0xF2B25C)

/* ======================= 版面尺寸 ======================= */

#define SCR_W        800
#define SCR_H        480
#define TOP_H        60
#define TICKER_H     30          /* 最底下那条滚动公告 */

/* 播放页卡片 = 客户端裁好的素材尺寸，一一对应，板上不做二次缩放 */
#define CARD_X       48
#define CARD_Y       6           /* content 坐标系（= 屏幕 y 66） */
#define CARD_W       704
#define CARD_H       312

/* 内容区可用高度（顶栏与底部滚动条之间） */
#define CONTENT_H    (SCR_H - TOP_H - TICKER_H)

/* 广告库 */
#define THUMB_W      232
#define THUMB_H      130
#define LIB_CARD_H   178

/* ======================= 状态 ======================= */

typedef enum {
    PAGE_PLAY = 0,
    PAGE_LIB  = 1,
    PAGE_INFO = 2
} page_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *content;

    lv_obj_t *tab_btn[3];
    lv_obj_t *tab_lbl[3];

    lv_obj_t *clock_lbl;
    lv_obj_t *date_lbl;

    /* 底部滚动公告 */
    lv_obj_t *ticker;
    lv_obj_t *ticker_lbl;
    uint32_t  ticker_gen;

    /* 全屏播放层（点广告后创建，销毁即退出） */
    lv_obj_t *fs_layer;
    lv_obj_t *fs_img;
    lv_obj_t *fs_hint;

    page_t page;

    /* 播放页 */
    lv_obj_t *media;      /* lv_img */
    lv_obj_t *flash;      /* 换片白闪遮罩 */
    lv_obj_t *prog;       /* 底部进度条 */
    lv_obj_t *title_lbl;
    lv_obj_t *sub_lbl;
    lv_obj_t *counter_lbl;
    lv_obj_t *dots;
    lv_obj_t *hold_lbl;

    int cur;              /* 当前广告序号 */
    int frame;            /* 当前帧号 */
    int elapsed_ms;       /* 图片广告已停留时间 */

    lv_obj_t *grid;       /* 广告库容器 */

    lv_timer_t *play_timer;
    lv_timer_t *clock_timer;
    lv_timer_t *poll_timer;

    int  hold_sec;                     /* 图片广告停留秒数 */
    char cur_name[AD_NAME_LEN];        /* 重扫后靠它找回同一条广告 */
} ui_ctx_t;

static ui_ctx_t g_ui;

/* 全屏解码缓冲（定义见全屏播放一节） */
static lv_color_t  *s_fs_buf;
static lv_img_dsc_t s_fs_dsc;

static void build_page(page_t page);
static bool fs_decode_frame(const char *path, int w, int h);

/* ======================= 延迟执行 ======================= */

enum { DEFER_NONE = 0, DEFER_REBUILD, DEFER_LOGOUT, DEFER_FS_EXIT };

static int s_defer_kind = DEFER_NONE;

static void play_exit_fullscreen(void);   /* 前置声明：defer_apply 里要用 */

static void defer_apply(void)
{
    int kind = s_defer_kind;
    s_defer_kind = DEFER_NONE;

    switch(kind) {
        case DEFER_REBUILD:
            build_page(g_ui.page);
            break;
        case DEFER_LOGOUT:
            advert_ui_close();
            btn_wind();
            break;
        case DEFER_FS_EXIT:
            play_exit_fullscreen();
            break;
        default:
            break;
    }
}

static void defer_timer_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    defer_apply();
}

static void defer_call(int kind)
{
    s_defer_kind = kind;
    lv_timer_t *t = lv_timer_create(defer_timer_cb, 20, NULL);
    if(t != NULL) lv_timer_set_repeat_count(t, 1);
}

/* ======================= 小工具 ======================= */

/* 建一个"裸"对象：清掉主题样式，完全由调用方控制。
 *
 * 注意这里连 CLICKABLE 一起清掉：
 *   lv_obj_create 出来的对象默认是"可点击"的，而界面里有好几个铺满整块的
 *   透明容器（白闪遮罩 flash、进度条 prog）。它们压在图片上面，会先把点击吃掉，
 *   父控件的 CLICKED 永远收不到 —— "点广告进全屏"失效就是这么来的。
 *   真正需要点的地方（页签、按钮、箭头、卡片、全屏层）都在建完之后自己
 *   lv_obj_add_flag(CLICKABLE)，所以默认不可点才是安全的。
 *   （lv_img / lv_label 的构造函数本来就会清掉这个标志，只有裸 lv_obj 不会。） */
static lv_obj_t *mk_obj(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                          lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

/* 白卡片：圆角 + 1px 细描边，无阴影（阴影在软件渲染下太贵） */
static lv_obj_t *mk_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *c = mk_obj(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, C_SURFACE, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 14, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, C_LINE, 0);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    return c;
}

/* 小按钮：primary=薄荷绿实心，否则白底细描边 */
static lv_obj_t *mk_button(lv_obj_t *parent, int x, int y, int w, int h,
                           const char *text, lv_event_cb_t cb, void *ud, bool primary)
{
    lv_obj_t *b = mk_obj(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);

    if(primary) {
        lv_obj_set_style_bg_color(b, C_MINT, 0);
        lv_obj_set_style_bg_color(b, C_MINT_DEEP, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, 0, 0);
    }
    else {
        lv_obj_set_style_bg_color(b, C_SURFACE, 0);
        lv_obj_set_style_bg_color(b, C_MINT_SOFT, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, C_LINE, 0);
    }

    if(cb != NULL) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);

    lv_obj_t *l = mk_label(b, text, &ad_font_16, primary ? C_SURFACE : C_INK2);
    lv_obj_center(l);
    return b;
}

/* 取网卡 IPv4，失败返回 false */
static bool if_ip(const char *name, char *out, size_t cap)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0) return false;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);

    bool ok = false;
    if(ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        snprintf(out, cap, "%s", inet_ntoa(sin->sin_addr));
        ok = true;
    }
    close(fd);
    return ok;
}

static void get_board_ip(char *out, size_t cap)
{
    static const char *names[] = { "eth0", "eth1", "wlan0", "usb0" };
    for(size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if(if_ip(names[i], out, cap)) return;
    }
    snprintf(out, cap, "未连接");
}

/* ======================= 顶部栏 ======================= */

static void style_tab(int i, bool active)
{
    lv_obj_set_style_bg_color(g_ui.tab_btn[i], active ? C_MINT_SOFT : C_SURFACE, 0);
    lv_obj_set_style_bg_opa(g_ui.tab_btn[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(g_ui.tab_lbl[i], active ? C_MINT_DEEP : C_INK2, 0);
}

static void tab_event_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if(idx < 0 || idx > 2) return;
    if((page_t)idx == g_ui.page) return;

    /* 记住当前广告名，换页回来还能接着播同一条 */
    const ad_item_t *it = ad_store_get(g_ui.cur);
    if(it != NULL) snprintf(g_ui.cur_name, sizeof(g_ui.cur_name), "%s", it->name);

    g_ui.page = (page_t)idx;
    for(int i = 0; i < 3; i++) style_tab(i, i == idx);

    defer_call(DEFER_REBUILD);
}

static void clock_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if(g_ui.clock_lbl == NULL) return;

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    /* 板上没有 RTC 电池，没校时之前系统时间是 1970/2015 这种鬼值。
       与其显示一个明显错误的时间，不如明说"还没校时"，用户才知道要跑一次客户端。 */
    if(tmv.tm_year + 1900 < 2020) {
        lv_label_set_text(g_ui.clock_lbl, "--:--");
        lv_label_set_text(g_ui.date_lbl, "等待校时");
        return;
    }

    lv_label_set_text_fmt(g_ui.clock_lbl, "%02d:%02d", tmv.tm_hour, tmv.tm_min);

    static const char *wd[] = { "日", "一", "二", "三", "四", "五", "六" };
    int w = tmv.tm_wday;
    if(w < 0 || w > 6) w = 0;
    lv_label_set_text_fmt(g_ui.date_lbl, "%d月%d日 周%s",
                          tmv.tm_mon + 1, tmv.tm_mday, wd[w]);
}

static void build_topbar(void)
{
    lv_obj_t *bar = mk_obj(g_ui.root);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, SCR_W, TOP_H);
    lv_obj_set_style_bg_color(bar, C_SURFACE, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, C_LINE, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);

    /* 左上角只留 DEEPSEEK 字标：不要 logo 图形，纯字 + 拉开字距就够了 */
    lv_obj_t *brand = mk_label(bar, "DEEPSEEK", &lv_font_montserrat_26, C_INK);
    lv_obj_set_style_text_letter_space(brand, 3, 0);
    lv_obj_set_pos(brand, 24, 16);

    /* 三个页签 */
    static const char *tab_text[3] = { "播放", "广告库", "信息" };
    static const int   tab_x[3]    = { 286, 384, 482 };

    for(int i = 0; i < 3; i++) {
        lv_obj_t *b = mk_obj(bar);
        lv_obj_set_pos(b, tab_x[i], 12);
        lv_obj_set_size(b, 88, 36);
        lv_obj_set_style_radius(b, 18, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, tab_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *l = mk_label(b, tab_text[i], &ad_font_16, C_INK2);
        lv_obj_center(l);

        g_ui.tab_btn[i] = b;
        g_ui.tab_lbl[i] = l;
    }

    /* 右侧时钟 */
    g_ui.clock_lbl = mk_label(bar, "00:00", &lv_font_montserrat_22, C_INK);
    lv_obj_align(g_ui.clock_lbl, LV_ALIGN_TOP_RIGHT, -20, 4);

    g_ui.date_lbl = mk_label(bar, "", &ad_font_14, C_MUTED);
    lv_obj_align(g_ui.date_lbl, LV_ALIGN_TOP_RIGHT, -20, 34);
}

/* ======================= 底部滚动公告 ======================= */

/**
 * 屏幕最底下一条 30px 的公告带：左边「公益」角标，右边文字循环左移。
 *
 * 用 lv_label 的 LV_LABEL_LONG_SCROLL_CIRCULAR，LVGL 自己会把文字接到自己尾巴后面
 * 无限滚，不用算偏移、不用手写定时器。滚动的重绘区域就是这条 700x21 的带子，
 * 一帧一万多个像素，对帧率几乎没影响。
 *
 * 文案来自 <素材根目录>/ticker.conf，没有就用内置默认值；
 * 客户端 `adclient.py ticker --text "..."` 可以随时改，板上 1 秒内跟着变。
 */
/* 设置滚动文案。
 * LVGL 的 LV_LABEL_LONG_SCROLL_CIRCULAR 只在"文字比控件宽"的时候才滚 ——
 * 文案短的时候它就静止了，而"轮播"的意思就是一直在动。
 * 所以把文案重复一遍、中间塞几个全角空格，保证总宽一定超过可视区 712px，
 * 滚起来之后两遍之间的空白正好当分隔符用。 */
static void set_ticker_text(const char *text)
{
    if(g_ui.ticker_lbl == NULL || text == NULL) return;

    char buf[1280];
    if(strlen(text) < 600) {
        snprintf(buf, sizeof(buf), "%s　　　　%s", text, text);
    }
    else {
        snprintf(buf, sizeof(buf), "%s", text);
    }
    lv_label_set_text(g_ui.ticker_lbl, buf);
}

static void build_ticker(void)
{
    lv_obj_t *t = mk_obj(g_ui.root);
    lv_obj_set_pos(t, 0, SCR_H - TICKER_H);
    lv_obj_set_size(t, SCR_W, TICKER_H);
    lv_obj_set_style_bg_color(t, C_SURFACE, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, C_LINE, 0);
    lv_obj_set_style_border_side(t, LV_BORDER_SIDE_TOP, 0);
    g_ui.ticker = t;

    lv_obj_t *tag = mk_obj(t);
    lv_obj_set_pos(tag, 16, 5);
    lv_obj_set_size(tag, 44, 20);
    lv_obj_set_style_radius(tag, 5, 0);
    lv_obj_set_style_bg_color(tag, C_MINT, 0);
    lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);

    lv_obj_t *tag_l = mk_label(tag, "公益", &ad_font_14, C_SURFACE);
    lv_obj_center(tag_l);

    g_ui.ticker_lbl = mk_label(t, "", &ad_font_16, C_INK2);
    lv_obj_set_pos(g_ui.ticker_lbl, 72, 5);
    lv_obj_set_width(g_ui.ticker_lbl, 712);
    lv_label_set_long_mode(g_ui.ticker_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_speed(g_ui.ticker_lbl, 30, 0);   /* 30 像素/秒 */
    lv_obj_set_style_text_align(g_ui.ticker_lbl, LV_TEXT_ALIGN_LEFT, 0);
    set_ticker_text(ad_store_ticker());

    g_ui.ticker_gen = ad_store_ticker_gen();
}

/* ======================= 播放引擎 ======================= */

/* 换广告时的一次白闪溶解：先压到接近全白，再淡出，把"切换"藏进光里 */
static void flash_opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void flash_play(void)
{
    if(g_ui.flash == NULL) return;

    lv_anim_del(g_ui.flash, flash_opa_cb);
    lv_obj_set_style_bg_opa(g_ui.flash, 220, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_ui.flash);
    lv_anim_set_values(&a, 220, 0);
    lv_anim_set_time(&a, 260);
    lv_anim_set_exec_cb(&a, flash_opa_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void refresh_dots(void)
{
    if(g_ui.dots == NULL) return;

    lv_obj_clean(g_ui.dots);

    int n = ad_store_count();
    if(n <= 1 || n > 10) return;

    for(int i = 0; i < n; i++) {
        bool on = (i == g_ui.cur);
        lv_obj_t *d = mk_obj(g_ui.dots);
        lv_obj_set_size(d, on ? 16 : 6, 6);
        lv_obj_set_style_radius(d, 3, 0);
        lv_obj_set_style_bg_color(d, on ? C_MINT : C_LINE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    }
}

static void refresh_meta_text(void)
{
    const ad_item_t *it = ad_store_get(g_ui.cur);
    if(it == NULL) return;

    if(g_ui.title_lbl != NULL) lv_label_set_text(g_ui.title_lbl, it->title);
    if(g_ui.sub_lbl != NULL)   lv_label_set_text(g_ui.sub_lbl, it->subtitle);
    if(g_ui.counter_lbl != NULL) {
        lv_label_set_text_fmt(g_ui.counter_lbl, "%02d / %02d",
                              g_ui.cur + 1, ad_store_count());
    }
    refresh_dots();
}

/* 给某个 lv_img 换图。路径前缀 A: 是 LVGL 文件系统盘符（LV_USE_FS_STDIO） */
static void set_img_src(lv_obj_t *img, const char *path)
{
    if(img == NULL) return;

    char src[AD_PATH_LEN + 4];
    snprintf(src, sizeof(src), "A:%s", path);
    lv_img_set_src(img, src);
}

/* 换帧：全屏时只喂全屏那一层，普通播放区被盖住了不用再刷（省一次整帧解码） */
static void set_media_src(const char *path)
{
    if(g_ui.fs_img != NULL) {
        if(fs_decode_frame(path, CARD_W, CARD_H)) {
            lv_img_set_src(g_ui.fs_img, &s_fs_dsc);
            lv_obj_invalidate(g_ui.fs_layer);
        }
        return;
    }

    set_img_src(g_ui.media, path);
}

static void set_progress(int done, int total)
{
    if(g_ui.prog == NULL) return;
    if(total <= 0) total = 1;
    if(done > total) done = total;

    int w = (int)((long)CARD_W * done / total);
    if(w < 2) w = 2;
    lv_obj_set_width(g_ui.prog, w);
}

static void play_enter(int idx)
{
    int n = ad_store_count();
    if(n <= 0) return;
    if(idx < 0) idx = 0;
    if(idx >= n) idx = 0;

    const ad_item_t *it = ad_store_get(idx);
    if(it == NULL) return;

    g_ui.cur        = idx;
    g_ui.frame      = 0;
    g_ui.elapsed_ms = 0;
    snprintf(g_ui.cur_name, sizeof(g_ui.cur_name), "%s", it->name);

    if(it->type == AD_TYPE_VIDEO) {
        char p[AD_PATH_LEN];
        if(ad_store_frame_path(idx, 0, p, sizeof(p))) set_media_src(p);
        set_progress(1, it->frame_count);
        if(g_ui.play_timer != NULL) {
            int period = 1000 / (it->fps > 0 ? it->fps : 12);
            if(period < 20) period = 20;
            lv_timer_set_period(g_ui.play_timer, (uint32_t)period);
        }
    }
    else {
        set_media_src(it->cover);
        set_progress(0, 100);
        if(g_ui.play_timer != NULL) lv_timer_set_period(g_ui.play_timer, 100);
    }

    refresh_meta_text();
    flash_play();
}

static void play_next(void)
{
    int n = ad_store_count();
    if(n <= 0) return;
    play_enter((g_ui.cur + 1) % n);
}

static void play_prev(void)
{
    int n = ad_store_count();
    if(n <= 0) return;
    play_enter((g_ui.cur - 1 + n) % n);
}

/* ---------------- 左右滑动切广告 ---------------- */

/*
 * 不用 LVGL 的 LV_EVENT_GESTURE，自己从 PRESSED/PRESSING/RELEASED 累加位移。
 *
 * 原因：LVGL 的手势要同时满足"没有可滚动祖先"和"每次采样的位移大于
 * gesture_min_velocity"两个条件（indev_gesture 开头就把 scroll_obj 判了死刑）。
 * 而这块板子每帧解码一张 JPEG 就要 ~80ms，触摸采样被拖到 100ms 一次，
 * 中间的分支互相打架，实测带探针能看到 vect=(-133,0)，GESTURE 却始终不发。
 *
 * 自己算反而更稳、更可控：松手时看横向总位移够不够，够就切下一条。
 * 而且 RELEASED 一定早于 CLICKED，在这里打个时间戳就能把随后的误触吃掉。
 */
static lv_coord_t s_drag_dx;
static lv_coord_t s_drag_dy;
static uint32_t   s_gesture_tick = 0;

#define SWIPE_MIN_DX            70      /* 横向至少划这么多像素才算滑动 */
#define GESTURE_CLICK_GUARD_MS  400     /* 划过之后多久内的 CLICKED 视为误触 */

static void swipe_press_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_drag_dx = 0;
    s_drag_dy = 0;
}

static void swipe_move_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    lv_indev_t *indev = lv_indev_get_act();
    if(indev == NULL) return;

    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    s_drag_dx += v.x;
    s_drag_dy += v.y;
}

static void swipe_release_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    /* 位移不够，或者更像是上下划 —— 都不算切广告 */
    if(LV_ABS(s_drag_dx) < SWIPE_MIN_DX) return;
    if(LV_ABS(s_drag_dx) <= LV_ABS(s_drag_dy)) return;

    s_gesture_tick = lv_tick_get();   /* 让紧跟着的那次 CLICKED 闭嘴 */

    if(getenv("ADVERT_DEBUG") != NULL) {
        printf("[swipe] dx=%d dy=%d -> %s\n", (int)s_drag_dx, (int)s_drag_dy,
               s_drag_dx < 0 ? "下一条" : "上一条");
        fflush(stdout);
    }

    if(s_drag_dx < 0) play_next();    /* 手指往左划 = 看下一条 */
    else              play_prev();    /* 往右划 = 回上一条 */
}

/* 给一个对象挂上"滑动切广告"的三个事件 */
static void attach_swipe(lv_obj_t *obj)
{
    lv_obj_add_event_cb(obj, swipe_press_cb,   LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(obj, swipe_move_cb,    LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(obj, swipe_release_cb, LV_EVENT_RELEASED, NULL);
}

/* 刚划过手势就认为这次"点击"是误触 */
static bool gesture_just_happened(void)
{
    return s_gesture_tick != 0 && lv_tick_elaps(s_gesture_tick) < GESTURE_CLICK_GUARD_MS;
}

/* 设 ADVERT_DEBUG=1 时每秒打一次实际播放帧率。
   用来判断板子的 JPEG 解码能力够不够：目标帧率写在 meta 里，
   实测低于目标就说明解码跟不上，得降帧率或者缩分辨率。 */
static void dbg_frame_tick(void)
{
    static int      state = -1;   /* -1 未初始化，0 关，1 开 */
    static uint32_t last  = 0;
    static int      cnt   = 0;

    if(state < 0) {
        state = (getenv("ADVERT_DEBUG") != NULL) ? 1 : 0;
    }
    if(state == 0) return;

    cnt++;
    uint32_t now = lv_tick_get();
    if(last == 0) { last = now; return; }

    if(now - last >= 1000) {
        const ad_item_t *it = ad_store_get(g_ui.cur);
        printf("[advert] %d 帧/秒（目标 %d）\n", cnt, it != NULL ? it->fps : 0);
        fflush(stdout);
        cnt  = 0;
        last = now;
    }
}

static void play_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);

    if(ad_store_count() <= 0) return;

    const ad_item_t *it = ad_store_get(g_ui.cur);
    if(it == NULL) { play_enter(0); return; }

    if(it->type == AD_TYPE_VIDEO) {
        g_ui.frame++;
        if(g_ui.frame >= it->frame_count) { play_next(); return; }

        char p[AD_PATH_LEN];
        if(ad_store_frame_path(g_ui.cur, g_ui.frame, p, sizeof(p))) {
            set_media_src(p);
        }
        set_progress(g_ui.frame + 1, it->frame_count);
        dbg_frame_tick();
    }
    else {
        g_ui.elapsed_ms += 100;
        int hold = g_ui.hold_sec * 1000;
        if(g_ui.elapsed_ms >= hold) { play_next(); return; }
        set_progress(g_ui.elapsed_ms, hold);
    }
}

static void nav_event_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);   /* +1 下一则 / -1 上一则 */

    int n = ad_store_count();
    if(n <= 0) return;

    play_enter(((g_ui.cur + dir) % n + n) % n);
}

/* 小箭头：两条线拼一个「>」「<」，比位图省事、也更锐利 */
static void mk_chevron(lv_obj_t *parent, bool right, lv_color_t color)
{
    static lv_point_t pr[3] = { {0, 0}, {7, 9}, {0, 18} };
    static lv_point_t pl[3] = { {7, 0}, {0, 9}, {7, 18} };

    lv_obj_t *ln = lv_line_create(parent);
    lv_line_set_points(ln, right ? pr : pl, 3);
    lv_obj_set_pos(ln, right ? 14 : 12, 8);

    lv_obj_set_style_line_width(ln, 2, 0);
    lv_obj_set_style_line_color(ln, color, 0);
    lv_obj_set_style_line_opa(ln, LV_OPA_COVER, 0);
    lv_obj_set_style_line_rounded(ln, true, 0);
}

static lv_obj_t *mk_arrow_btn(lv_obj_t *parent, int x, int y, bool right)
{
    lv_obj_t *b = mk_obj(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, 34, 34);
    lv_obj_set_style_radius(b, 17, 0);
    lv_obj_set_style_bg_color(b, C_SURFACE, 0);
    lv_obj_set_style_bg_opa(b, 190, 0);
    lv_obj_set_style_bg_opa(b, 250, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    mk_chevron(b, right, C_INK2);
    return b;
}

/* ---------------- 全屏用的解码缓冲区 ---------------- */

/*
 * 为什么全屏要自己解码一遍：
 *   LVGL 的 lv_img 想缩放，必须拿到"整幅解压后的像素"（dec_dsc.img_data）。
 *   而文件源的 JPEG 走的是 tjpgd 的逐行解码器，img_data 恒为 NULL，
 *   lv_draw_img 于是落到"逐行读取"分支 —— 那条分支直接无视 zoom，
 *   结果就是设了缩放也没用（更糟的时候整张画到屏幕外，全黑）。
 *
 * 所以这里用 lv_img_decoder_read_line 把一帧解到自己的缓冲区，
 * 再包装成 LV_IMG_CF_TRUE_COLOR 的"变量图像"交给 lv_img。
 * 变量图像的数据是完整可见的，缩放/旋转就都能正常工作。
 *
 * 缓冲区 704*312*4 ≈ 878KB，用 malloc 而不是 LVGL 内存池，不占 LV_MEM。
 */
static void fs_buf_free(void)
{
    if(s_fs_buf != NULL) {
        free(s_fs_buf);
        s_fs_buf = NULL;
    }
}

/* 把 path 指向的 JPEG 解到 s_fs_buf，并把 s_fs_dsc 指向它。成功返回 true */
static bool fs_decode_frame(const char *path, int w, int h)
{
    char src[AD_PATH_LEN + 4];
    snprintf(src, sizeof(src), "A:%s", path);

    lv_img_decoder_dsc_t d;
    if(lv_img_decoder_open(&d, src, lv_color_black(), 0) != LV_RES_OK) return false;

    bool ok = false;
    if(d.header.w == w && d.header.h == h) {
        if(d.img_data != NULL) {
            memcpy(s_fs_buf, d.img_data, (size_t)w * (size_t)h * sizeof(lv_color_t));
            ok = true;
        }
        else {
            ok = true;
            for(int y = 0; y < h; y++) {
                if(lv_img_decoder_read_line(&d, 0, y, w,
                                            (uint8_t *)(s_fs_buf + (size_t)y * (size_t)w)) != LV_RES_OK) {
                    ok = false;
                    break;
                }
            }
        }
    }

    lv_img_decoder_close(&d);

    if(ok) {
        s_fs_dsc.header.cf     = LV_IMG_CF_TRUE_COLOR;
        s_fs_dsc.header.w      = (uint32_t)w;
        s_fs_dsc.header.h      = (uint32_t)h;
        s_fs_dsc.header.always_zero = 0;
        s_fs_dsc.data_size     = (uint32_t)w * (uint32_t)h * sizeof(lv_color_t);
        s_fs_dsc.data          = (const uint8_t *)s_fs_buf;
    }
    return ok;
}

/* ======================= 全屏播放 ======================= */

/* 进全屏时那句提示，2.2 秒后自己隐掉（只改透明度，不删对象，避免野指针） */
static void fs_hint_timer_cb(lv_timer_t *t)
{
    lv_timer_del(t);

    if(g_ui.fs_hint == NULL) return;          /* 已经退出全屏了 */
    lv_obj_set_style_bg_opa(g_ui.fs_hint, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_opa(g_ui.fs_hint, LV_OPA_TRANSP, 0);
}

static void play_exit_fullscreen(void)
{
    if(g_ui.fs_layer == NULL) return;

    g_ui.fs_hint  = NULL;
    g_ui.fs_img   = NULL;
    lv_obj_del(g_ui.fs_layer);
    g_ui.fs_layer = NULL;

    fs_buf_free();

    if(g_ui.media != NULL) {
        lv_obj_clear_flag(g_ui.media, LV_OBJ_FLAG_HIDDEN);

        /* 全屏期间普通播放区是停更的，退出时补一次当前帧 */
        const ad_item_t *it = ad_store_get(g_ui.cur);
        if(it != NULL) {
            if(it->type == AD_TYPE_VIDEO) {
                char p[AD_PATH_LEN];
                if(ad_store_frame_path(g_ui.cur, g_ui.frame, p, sizeof(p))) {
                    set_img_src(g_ui.media, p);
                }
            }
            else {
                set_img_src(g_ui.media, it->cover);
            }
        }
    }
}

static void fs_exit_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    /* 全屏里左右划是切广告，松手那一下的 CLICKED 不能当成"退出全屏" */
    if(gesture_just_happened()) return;

    /* 不能在"自己这一层的点击回调"里直接把自己删掉，交给下一轮事件循环 */
    defer_call(DEFER_FS_EXIT);
}

/**
 * 点广告进全屏：铺一张纯黑底，把当前帧等比放大到刚好盖住 800x480。
 *
 * 这里刻意用 lv_img_set_zoom 实时放大，而不是另外再存一套 800x480 的帧：
 *   · 放大只增加"绘制"开销，JPEG 解码量不变（还是 704x312 那张）
 *   · 真存一套大图，解码像素多 1.66 倍，反而更慢
 * 实测 704x312 解满能到 16fps，放大后仍有余量，12fps 的片子在板上不会拖慢。
 */
static void play_enter_fullscreen(void)
{
    if(g_ui.fs_layer != NULL) return;
    if(ad_store_count() <= 0) return;

    const ad_item_t *it = ad_store_get(g_ui.cur);
    if(it == NULL) return;

    lv_obj_t *layer = mk_obj(g_ui.root);
    lv_obj_set_pos(layer, 0, 0);
    lv_obj_set_size(layer, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(layer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, 0);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(layer, fs_exit_cb, LV_EVENT_CLICKED, NULL);
    attach_swipe(layer);
    g_ui.fs_layer = layer;

    lv_obj_t *img = lv_img_create(layer);
    g_ui.fs_img = img;

    /* 解码缓冲只分配一次；分配失败就退回普通模式，别把界面搞成黑屏 */
    if(s_fs_buf == NULL) {
        s_fs_buf = (lv_color_t *)malloc((size_t)CARD_W * (size_t)CARD_H * sizeof(lv_color_t));
        if(s_fs_buf == NULL) {
            lv_obj_del(layer);
            g_ui.fs_layer = NULL;
            g_ui.fs_img   = NULL;
            return;
        }
    }

    /* 把当前这一帧解到缓冲，再以"变量图像"喂给 lv_img —— 缩放才有数据可用 */
    char path[AD_PATH_LEN];
    bool got = false;
    if(it->type == AD_TYPE_VIDEO) {
        if(ad_store_frame_path(g_ui.cur, g_ui.frame, path, sizeof(path))) got = true;
    }
    else {
        snprintf(path, sizeof(path), "%s", it->cover);
        got = true;
    }
    if(got && fs_decode_frame(path, CARD_W, CARD_H)) {
        lv_img_set_src(img, &s_fs_dsc);
    }

    /* 卡片上那张图被全屏层完全盖住，藏起来可以省掉每帧一次整帧解码 */
    if(g_ui.media != NULL) lv_obj_add_flag(g_ui.media, LV_OBJ_FLAG_HIDDEN);

    lv_obj_update_layout(layer);

    int iw = lv_obj_get_width(img);
    int ih = lv_obj_get_height(img);
    if(iw <= 0 || ih <= 0) { iw = CARD_W; ih = CARD_H; }

    /* 缩放比取宽、高两个方向的**较小者** = contain：
       整幅广告完整放进 800x480，短边留黑边，一点内容都不切。
       （之前取的是较大者 = cover，虽然铺满了，但上下/左右会被裁掉，
         客户看到的就是"人脸被切了一半"。广告是给人看内容的，不是给屏看满的。）
       256 是 1.0 倍，pivot 默认在图片中心，lv_obj_center 之后放大仍然居中。 */
    uint32_t zw = (uint32_t)((SCR_W * 256 + iw - 1) / iw);
    uint32_t zh = (uint32_t)((SCR_H * 256 + ih - 1) / ih);
    uint32_t zoom = (zw < zh) ? zw : zh;

    if(getenv("ADVERT_FS_ZOOM") != NULL) zoom = (uint32_t)atoi(getenv("ADVERT_FS_ZOOM"));

    lv_img_set_zoom(img, (uint16_t)zoom);
    lv_obj_center(img);

    if(getenv("ADVERT_DEBUG") != NULL) {
        printf("[advert] 全屏：素材 %dx%d 缩放 %u/256\n", iw, ih, (unsigned)zoom);
        fflush(stdout);
    }

    g_ui.fs_img = img;

    g_ui.fs_hint = mk_label(layer, "点击任意处退出全屏", &ad_font_16, C_SURFACE);
    lv_obj_set_style_bg_color(g_ui.fs_hint, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_ui.fs_hint, 150, 0);
    lv_obj_set_style_radius(g_ui.fs_hint, 14, 0);
    lv_obj_set_style_pad_hor(g_ui.fs_hint, 14, 0);
    lv_obj_set_style_pad_ver(g_ui.fs_hint, 5, 0);
    lv_obj_align(g_ui.fs_hint, LV_ALIGN_BOTTOM_MID, 0, -16);

    lv_timer_t *t = lv_timer_create(fs_hint_timer_cb, 2200, NULL);
    if(t != NULL) lv_timer_set_repeat_count(t, 1);
}

static void card_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    /* 划过之后紧跟着来的这次 CLICKED 是误触，不要进全屏 */
    if(gesture_just_happened()) return;

    play_enter_fullscreen();
}

/* ADVERT_FULLSCREEN=1 时的一次性定时器 */
static void fs_auto_timer_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    play_enter_fullscreen();
}

/* ======================= 播放页 ======================= */

static void build_play_page(void)
{
    lv_obj_t *card = mk_card(g_ui.content, CARD_X, CARD_Y, CARD_W, CARD_H);

    /* 点画面任意处（除了左右两个箭头）进全屏；左右划动切上/下一条 */
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED, NULL);
    attach_swipe(card);

    if(ad_store_count() > 0) {
        g_ui.media = lv_img_create(card);
        lv_obj_set_pos(g_ui.media, 0, 0);

        /* 白闪遮罩：必须在图片之后创建，才能盖在图片上面 */
        g_ui.flash = mk_obj(card);
        lv_obj_set_pos(g_ui.flash, 0, 0);
        lv_obj_set_size(g_ui.flash, CARD_W, CARD_H);
        lv_obj_set_style_bg_color(g_ui.flash, C_SURFACE, 0);
        lv_obj_set_style_bg_opa(g_ui.flash, 0, 0);

        lv_obj_t *prev = mk_arrow_btn(card, 10, (CARD_H - 34) / 2, false);
        lv_obj_add_event_cb(prev, nav_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);

        lv_obj_t *next = mk_arrow_btn(card, CARD_W - 44, (CARD_H - 34) / 2, true);
        lv_obj_add_event_cb(next, nav_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);

        g_ui.prog = mk_obj(card);
        lv_obj_set_pos(g_ui.prog, 0, CARD_H - 3);
        lv_obj_set_size(g_ui.prog, 2, 3);
        lv_obj_set_style_bg_color(g_ui.prog, C_MINT, 0);
        lv_obj_set_style_bg_opa(g_ui.prog, LV_OPA_COVER, 0);

        /* 右上角「全屏」小标签。注意要清掉 CLICKABLE：
           lv_obj 默认是可点的，留着它就会把点击吞掉，进不了全屏。 */
        lv_obj_t *chip = mk_obj(card);
        lv_obj_set_size(chip, 52, 22);
        lv_obj_set_pos(chip, CARD_W - 62, 10);
        lv_obj_set_style_radius(chip, 6, 0);
        lv_obj_set_style_bg_color(chip, C_SURFACE, 0);
        lv_obj_set_style_bg_opa(chip, 205, 0);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *chip_l = mk_label(chip, "全屏", &ad_font_14, C_INK2);
        lv_obj_center(chip_l);

        g_ui.play_timer = lv_timer_create(play_timer_cb, 100, NULL);
    }
    else {
        lv_obj_t *empty = mk_label(card, "暂无广告", &ad_font_20, C_INK2);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, -18);
        lv_obj_t *tip = mk_label(card, "请用 adclient.py 从电脑上传素材", &ad_font_14, C_MUTED);
        lv_obj_align(tip, LV_ALIGN_CENTER, 0, 14);
    }

    /* —— 信息行 —— */
    g_ui.title_lbl = mk_label(g_ui.content, "—", &ad_font_20, C_INK);
    lv_obj_set_pos(g_ui.title_lbl, CARD_X, 326);
    lv_obj_set_width(g_ui.title_lbl, 470);
    lv_label_set_long_mode(g_ui.title_lbl, LV_LABEL_LONG_DOT);

    g_ui.sub_lbl = mk_label(g_ui.content, "", &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_pos(g_ui.sub_lbl, CARD_X, 354);

    g_ui.counter_lbl = mk_label(g_ui.content, "00 / 00", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(g_ui.counter_lbl, LV_ALIGN_TOP_RIGHT, -CARD_X, 330);

    g_ui.dots = mk_obj(g_ui.content);
    lv_obj_set_size(g_ui.dots, 260, 12);
    lv_obj_align(g_ui.dots, LV_ALIGN_TOP_RIGHT, -CARD_X, 356);
    lv_obj_set_flex_flow(g_ui.dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_ui.dots, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_ui.dots, 6, 0);
    lv_obj_set_style_bg_opa(g_ui.dots, LV_OPA_TRANSP, 0);

    if(ad_store_count() > 0) {
        /* 重扫后优先接着播同一条 */
        int target = 0;
        for(int i = 0; i < ad_store_count(); i++) {
            const ad_item_t *it = ad_store_get(i);
            if(it != NULL && g_ui.cur_name[0] != '\0' && strcmp(it->name, g_ui.cur_name) == 0) {
                target = i;
                break;
            }
        }
        play_enter(target);
    }
}

/* ======================= 广告库 ======================= */

static void lib_item_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    /* 先把"要播哪条"记下来：build_play_page 靠 cur_name 回找同一条广告 */
    const ad_item_t *it = ad_store_get(idx);
    if(it != NULL) {
        g_ui.cur = idx;
        snprintf(g_ui.cur_name, sizeof(g_ui.cur_name), "%s", it->name);
    }

    g_ui.page = PAGE_PLAY;
    for(int i = 0; i < 3; i++) style_tab(i, i == 0);

    defer_call(DEFER_REBUILD);
}

static void build_lib_page(void)
{
    lv_obj_t *grid = mk_obj(g_ui.content);
    lv_obj_set_pos(grid, 0, 0);
    lv_obj_set_size(grid, SCR_W, CONTENT_H);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);

    /* 外面几层（屏幕 / root / content）都被设成不可滚动了，
       所以这一层要自己把 SCROLLABLE 打开，广告多了才能上下滑。 */
    lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_left(grid, 36, 0);
    lv_obj_set_style_pad_right(grid, 36, 0);
    lv_obj_set_style_pad_top(grid, 14, 0);
    lv_obj_set_style_pad_row(grid, 16, 0);
    lv_obj_set_style_pad_column(grid, 16, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    g_ui.grid = grid;

    int n = ad_store_count();
    for(int i = 0; i < n; i++) {
        const ad_item_t *it = ad_store_get(i);
        if(it == NULL) continue;

        lv_obj_t *c = mk_card(grid, 0, 0, THUMB_W, LIB_CARD_H);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, lib_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *img = lv_img_create(c);
        lv_obj_set_pos(img, 0, 0);

        char src[AD_PATH_LEN + 4];
        snprintf(src, sizeof(src), "A:%s", it->thumb);
        lv_img_set_src(img, src);

        /* 类型角标 */
        lv_obj_t *badge = mk_obj(c);
        lv_obj_set_size(badge, 46, 20);
        lv_obj_set_pos(badge, 10, 10);
        lv_obj_set_style_radius(badge, 6, 0);
        lv_obj_set_style_bg_color(badge, it->type == AD_TYPE_VIDEO ? C_MINT : C_AMBER, 0);
        lv_obj_set_style_bg_opa(badge, 235, 0);
        lv_obj_t *bl = mk_label(badge, it->type == AD_TYPE_VIDEO ? "视频" : "图片",
                                &ad_font_14, C_SURFACE);
        lv_obj_center(bl);

        lv_obj_t *t = mk_label(c, it->title, &ad_font_16, C_INK);
        lv_obj_set_pos(t, 12, THUMB_H + 5);
        lv_obj_set_width(t, THUMB_W - 24);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);

        /* 客户备注：客户在网页上填的那句话，管理员在广告库里一眼能看到来源/档期 */
        if(it->remark[0] != '\0') {
            lv_obj_t *rm = mk_label(c, it->remark, &ad_font_14, C_MUTED);
            lv_obj_set_pos(rm, 12, THUMB_H + 27);
            lv_obj_set_width(rm, THUMB_W - 24);
            lv_label_set_long_mode(rm, LV_LABEL_LONG_DOT);
        }
    }

    if(n == 0) {
        lv_obj_t *empty = mk_label(grid, "还没有广告，先用客户端上传吧", &ad_font_16, C_MUTED);
        lv_obj_set_pos(empty, 20, 120);
    }
}

/* ======================= 信息页 ======================= */

static void reload_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    defer_call(DEFER_REBUILD);
}

static void logout_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    defer_call(DEFER_LOGOUT);
}

static void hold_minus_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if(g_ui.hold_sec > 2) g_ui.hold_sec--;
    if(g_ui.hold_lbl != NULL) lv_label_set_text_fmt(g_ui.hold_lbl, "%d s", g_ui.hold_sec);
}

static void hold_plus_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if(g_ui.hold_sec < 30) g_ui.hold_sec++;
    if(g_ui.hold_lbl != NULL) lv_label_set_text_fmt(g_ui.hold_lbl, "%d s", g_ui.hold_sec);
}

/* 卡片里的一行「键 ———— 值」 */
static void mk_row(lv_obj_t *card, int y, const char *key, const char *value)
{
    lv_obj_t *k = mk_label(card, key, &ad_font_14, C_MUTED);
    lv_obj_set_pos(k, 20, y);

    lv_obj_t *v = mk_label(card, value, &ad_font_16, C_INK);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -20, y - 1);
}

static void mk_card_title(lv_obj_t *card, const char *text)
{
    lv_obj_t *bar = mk_obj(card);
    lv_obj_set_pos(bar, 20, 21);
    lv_obj_set_size(bar, 3, 16);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_color(bar, C_MINT, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    lv_obj_t *l = mk_label(card, text, &ad_font_16, C_INK);
    lv_obj_set_pos(l, 31, 18);
}

static void build_info_page(void)
{
    char buf[96];

    /* 左卡：设备 */
    lv_obj_t *c1 = mk_card(g_ui.content, 36, 14, 352, 250);
    mk_card_title(c1, "设备信息");

    mk_row(c1, 56,  "型号",     "GEC6818");
    mk_row(c1, 92,  "屏幕",     "800 x 480");
    mk_row(c1, 128, "色深",     "32 bpp");
    snprintf(buf, sizeof(buf), "%d 条", ad_store_count());
    mk_row(c1, 164, "广告数量", buf);
    mk_row(c1, 200, "播放模式", "循环播放");

    /* 右卡：传输 */
    lv_obj_t *c2 = mk_card(g_ui.content, 412, 14, 352, 250);
    mk_card_title(c2, "传输服务");

    char ip[32];
    get_board_ip(ip, sizeof(ip));
    mk_row(c2, 56,  "本机地址", ip);
    mk_row(c2, 92,  "监听端口", "9500");
    mk_row(c2, 128, "服务状态", "等待连接");

    struct statvfs vfs;
    if(statvfs(ad_store_root(), &vfs) == 0) {
        unsigned long long mb = (unsigned long long)vfs.f_bavail * vfs.f_frsize / (1024ull * 1024ull);
        snprintf(buf, sizeof(buf), "%llu MB", mb);
    }
    else {
        snprintf(buf, sizeof(buf), "未知");
    }
    mk_row(c2, 164, "可用空间", buf);
    mk_row(c2, 200, "素材目录", ad_store_root());

    /* 底部操作区 */
    mk_button(g_ui.content, 36, 286, 140, 44, "重新扫描", reload_btn_cb, NULL, true);
    mk_button(g_ui.content, 188, 286, 140, 44, "退出登录", logout_btn_cb, NULL, false);

    /* 图片停留时间调节 */
    lv_obj_t *box = mk_obj(g_ui.content);
    lv_obj_set_pos(box, 412, 286);
    lv_obj_set_size(box, 352, 44);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);

    lv_obj_t *lbl = mk_label(box, "图片停留", &ad_font_16, C_INK2);
    lv_obj_set_pos(lbl, 0, 12);

    mk_button(box, 96, 2, 40, 40, "-", hold_minus_cb, NULL, false);
    mk_button(box, 200, 2, 40, 40, "+", hold_plus_cb, NULL, false);

    g_ui.hold_lbl = mk_label(box, "", &lv_font_montserrat_16, C_INK);
    lv_label_set_text_fmt(g_ui.hold_lbl, "%d s", g_ui.hold_sec);
    lv_obj_set_pos(g_ui.hold_lbl, 148, 12);
    lv_obj_set_width(g_ui.hold_lbl, 44);
    lv_obj_set_style_text_align(g_ui.hold_lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *foot = mk_label(g_ui.content,
                              "素材由电脑端 adclient.py 上传，约 1 秒后自动刷新",
                              &ad_font_14, C_MUTED);
    lv_obj_set_pos(foot, 36, 352);
}

/* ======================= 页面调度 ======================= */

static void page_clear(void)
{
    if(g_ui.play_timer != NULL) {
        lv_timer_del(g_ui.play_timer);
        g_ui.play_timer = NULL;
    }
    lv_obj_clean(g_ui.content);

    g_ui.media       = NULL;
    g_ui.flash       = NULL;
    g_ui.prog        = NULL;
    g_ui.title_lbl   = NULL;
    g_ui.sub_lbl     = NULL;
    g_ui.counter_lbl = NULL;
    g_ui.dots        = NULL;
    g_ui.grid        = NULL;
    g_ui.hold_lbl    = NULL;
}

static void build_page(page_t page)
{
    page_clear();

    switch(page) {
        case PAGE_PLAY: build_play_page(); break;
        case PAGE_LIB:  build_lib_page();  break;
        case PAGE_INFO: build_info_page(); break;
        default: break;
    }
}

/* 每秒轮询清单；有变化就重建当前页 */
static void poll_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);

    /* 滚动公告是常驻对象，单独检查它有没有被换过，不用为它重建整页 */
    ad_store_refresh_ticker();
    if(ad_store_ticker_gen() != g_ui.ticker_gen) {
        g_ui.ticker_gen = ad_store_ticker_gen();
        set_ticker_text(ad_store_ticker());
    }

    if(!ad_store_poll()) return;

    const ad_item_t *it = ad_store_get(g_ui.cur);
    if(it != NULL) snprintf(g_ui.cur_name, sizeof(g_ui.cur_name), "%s", it->name);

    build_page(g_ui.page);
}

/* ======================= 对外接口 ======================= */

void advert_ui_close(void)
{
    if(g_ui.play_timer  != NULL) { lv_timer_del(g_ui.play_timer);  g_ui.play_timer  = NULL; }
    if(g_ui.clock_timer != NULL) { lv_timer_del(g_ui.clock_timer); g_ui.clock_timer = NULL; }
    if(g_ui.poll_timer  != NULL) { lv_timer_del(g_ui.poll_timer);  g_ui.poll_timer  = NULL; }

    if(g_ui.root != NULL) {
        lv_anim_del_all();
        lv_obj_del(g_ui.root);
    }

    int keep_hold = g_ui.hold_sec;
    memset(&g_ui, 0, sizeof(g_ui));
    g_ui.hold_sec = (keep_hold > 0) ? keep_hold : 5;
}

void advert_ui_show(void)
{
    advert_ui_close();

    ad_store_init(NULL);
    ad_store_scan();

    g_ui.page = PAGE_PLAY;
    g_ui.cur  = 0;

    /* 调试入口：ADVERT_PAGE=lib / info 可直接起在指定页，省得用触摸去点 */
    const char *pg = getenv("ADVERT_PAGE");
    if(pg != NULL) {
        if(strcmp(pg, "lib") == 0)       g_ui.page = PAGE_LIB;
        else if(strcmp(pg, "info") == 0) g_ui.page = PAGE_INFO;
    }

    lv_obj_clean(lv_scr_act());

    g_ui.root = mk_obj(lv_scr_act());
    lv_obj_set_pos(g_ui.root, 0, 0);
    lv_obj_set_size(g_ui.root, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(g_ui.root, C_BG, 0);
    lv_obj_set_style_bg_opa(g_ui.root, LV_OPA_COVER, 0);

    /* 屏幕本身必须"不可滚动"。
     * LVGL 的手势有个前置条件（lv_indev.c 的 indev_gesture 开头 `if(scroll_obj) return;`）：
     * 只要从按下的对象往上能找到可滚动祖先，拖动就优先当成滚屏，
     * LV_EVENT_GESTURE 永远不会发出来。lv_scr_act() 默认是 SCROLLABLE，
     * 于是"左右划切广告"会变成"拖着整页跑"，松手后还照常发 CLICKED ——
     * 表现就是划一下反而退出了全屏。
     * 关掉之后手势才会落到卡片 / 全屏层上；确实需要滚动的列表自己带 SCROLLABLE。 */
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

    build_topbar();

    g_ui.content = mk_obj(g_ui.root);
    lv_obj_set_pos(g_ui.content, 0, TOP_H);
    lv_obj_set_size(g_ui.content, SCR_W, CONTENT_H);
    lv_obj_set_style_bg_opa(g_ui.content, LV_OPA_TRANSP, 0);

    /* 滚动公告放在 content 之后创建，保证它在内容之上、全屏层之下 */
    build_ticker();

    for(int i = 0; i < 3; i++) style_tab(i, i == (int)g_ui.page);

    build_page(g_ui.page);

    g_ui.clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
    g_ui.poll_timer  = lv_timer_create(poll_timer_cb, 1000, NULL);
    clock_timer_cb(NULL);

    /* 调试入口：ADVERT_FULLSCREEN=1 启动 1.5 秒后自动进全屏，方便远程截图验证 */
    if(getenv("ADVERT_FULLSCREEN") != NULL && g_ui.page == PAGE_PLAY && ad_store_count() > 0) {
        lv_timer_t *t = lv_timer_create(fs_auto_timer_cb, 1500, NULL);
        if(t != NULL) lv_timer_set_repeat_count(t, 1);
    }
}
