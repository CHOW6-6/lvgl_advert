
/**
 * @file main
 *
 */

/*********************
 *      INCLUDES
 *********************/
/* 这份 Makefile 现在编两个互不相干的程序，编哪个由 -D 决定：
 *   -DAPP_ADVERT=1  -> build/bin/demo     开机动画 / 登录 / 广告三页 / 板端接收服务
 *   -DAPP_WEATHER=1 -> build/bin/weather  天气监测中心（不链接广告机、不链接 test/）
 * 别在同一个二进制里塞两套 UI 了 —— 一个 --weather 开关切来切去，
 * 体积、依赖、部署内容全糊在一起，出问题根本分不清是谁的锅。 */
#if !defined(APP_ADVERT) && !defined(APP_WEATHER)
#error "必须指定 -DAPP_ADVERT=1 或 -DAPP_WEATHER=1（请用 make 或 make weather）"
#endif
#if defined(APP_ADVERT) && defined(APP_WEATHER)
#error "APP_ADVERT 和 APP_WEATHER 不能同时定义"
#endif

#define _DEFAULT_SOURCE /* needed for usleep() */
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/file.h>
#include <stdio.h> /* printf()，回调函数里要用 */
#include <unistd.h>
#include <pthread.h>
#include "lv_drv_conf.h"
#include "lvgl/lvgl.h"
#include "lvgl/examples/lv_examples.h"
#include "lvgl/demos/lv_demos.h"
#ifdef APP_WEATHER
#include "weather/weather_app.h"   /* 天气监测中心应用 */
#endif
#if USE_SDL
#define SDL_MAIN_HANDLED /*To fix SDL's "undefined reference to WinMain" issue*/
#include <SDL2/SDL.h>
#include "lv_drivers/sdl/sdl.h"
#elif USE_X11
#include "lv_drivers/x11/x11.h"
#elif USE_FBDEV
/* GEC6818：LCD 走内核 framebuffer，触摸走 evdev */
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"
#ifdef APP_ADVERT
/* 广告机专属：开机动画、登录页、三页广告、测试界面 */
#include "my_win.h"
#include "login.h"
#include "boot_screen.h"
#include "advert_ui.h"
#include "test.h"
#endif
#endif
// #include "lv_drivers/display/monitor.h"
// #include "lv_drivers/indev/mouse.h"
// #include "lv_drivers/indev/keyboard.h"
// #include "lv_drivers/indev/mousewheel.h"

/*********************
 *      DEFINES
 *********************/
/* 日志前缀，方便一眼看出是哪个程序在说话 */
#ifdef APP_WEATHER
#define APP_TAG_NAME "weather"
#else
#define APP_TAG_NAME "demo"
#endif

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void hal_init(void);
static void hal_deinit(void);
static void *tick_thread(void *data);

/* ADVERT_TAP / ADVERT_SWIPE 调试用：假指针设备的参数 */
static lv_point_t s_tap_pt;        /* 单击位置 */
static uint32_t   s_tap_t0;
static uint32_t   s_tap_at = 2000; /* 第几毫秒按下，可用 x,y,ms 指定 */

static bool       s_swipe_on = false;
static lv_point_t s_swipe_a, s_swipe_b;
static uint32_t   s_swipe_ms = 300;

/* 按下窗口：[tap_at, tap_at+400ms)，其余时间松开。LVGL 每 30ms 读一次，
   这个窗口足够它识别出一次完整的 press→release。

   如果设了 ADVERT_SWIPE，就改成"按住并从 A 匀速拖到 B"，
   这样才会累计出超过 50px 的位移，触发 LVGL 的 LV_EVENT_GESTURE。 */
static void tap_indev_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
  LV_UNUSED(drv);

  uint32_t dt = lv_tick_elaps(s_tap_t0);

  if(s_swipe_on) {
    if(dt < s_tap_at) {
      data->point.x = s_swipe_a.x;
      data->point.y = s_swipe_a.y;
      data->state   = LV_INDEV_STATE_RELEASED;
      return;
    }

    uint32_t k = dt - s_tap_at;
    if(k >= s_swipe_ms) {                 /* 松手 */
      data->point.x = s_swipe_b.x;
      data->point.y = s_swipe_b.y;
      data->state   = LV_INDEV_STATE_RELEASED;
      return;
    }

    int32_t t = (int32_t)k;
    int32_t d = (int32_t)s_swipe_ms;
    data->point.x = (lv_coord_t)(s_swipe_a.x + (int32_t)(s_swipe_b.x - s_swipe_a.x) * t / d);
    data->point.y = (lv_coord_t)(s_swipe_a.y + (int32_t)(s_swipe_b.y - s_swipe_a.y) * t / d);
    data->state   = LV_INDEV_STATE_PRESSED;
    return;
  }

  data->point.x = s_tap_pt.x;
  data->point.y = s_tap_pt.y;
  data->state = (dt >= s_tap_at && dt < s_tap_at + 400) ? LV_INDEV_STATE_PRESSED
                                                        : LV_INDEV_STATE_RELEASED;
}

/**********************
 *  STATIC VARIABLES
 **********************/
static pthread_t thr_tick;    /* thread */
static bool end_tick = false; /* flag to terminate thread */

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *      VARIABLES
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
#if 0
static void user_image_demo()
{
  lv_obj_t * img = lv_gif_create(lv_scr_act());
  lv_gif_set_src(img, "A:lvgl/examples/libs/gif/bulb.gif");
  lv_obj_align(img, LV_ALIGN_BOTTOM_RIGHT, -20, -20);

  lv_color_t bg_color = lv_palette_lighten(LV_PALETTE_LIGHT_BLUE, 5);
    lv_color_t fg_color = lv_palette_darken(LV_PALETTE_BLUE, 4);

    lv_obj_t * qr = lv_qrcode_create(lv_scr_act(), 150, fg_color, bg_color);

    /*Set data*/
    const char * data = "https://lvgl.io";
    lv_qrcode_update(qr, data, strlen(data));
    lv_obj_center(qr);

    /*Add a border with bg_color*/
    lv_obj_set_style_border_color(qr, bg_color, 0);
    lv_obj_set_style_border_width(qr, 5, 0);

    /*Create a font*/
    static lv_ft_info_t info;
    /*FreeType uses C standard file system, so no driver letter is required.*/
    info.name = "./lvgl/examples/libs/freetype/Lato-Regular.ttf";
    info.weight = 24;
    info.style = FT_FONT_STYLE_NORMAL;
    info.mem = NULL;
    if(!lv_ft_font_init(&info)) {
        LV_LOG_ERROR("create failed.");
    }

    /*Create style with the new font*/
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_text_font(&style, info.font);
    lv_style_set_text_align(&style, LV_TEXT_ALIGN_CENTER);

    /*Create a label with the new style*/
    lv_obj_t * label = lv_label_create(lv_scr_act());
    lv_obj_add_style(label, &style, 0);
    lv_label_set_text(label, "Hello world\nI'm a font created with FreeType");
    lv_obj_set_pos(label, 10, 10);

    lv_obj_t *  img1 = lv_img_create(lv_scr_act());
    /* Assuming a File system is attached to letter 'A'
     * E.g. set LV_USE_FS_STDIO 'A' in lv_conf.h */
    lv_img_set_src(img1, "A:lvgl/examples/libs/png/wink.png");
    lv_obj_align(img1, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t * wp;

    wp = lv_img_create(lv_scr_act());
    /* Assuming a File system is attached to letter 'A'
     * E.g. set LV_USE_FS_STDIO 'A' in lv_conf.h */
    lv_img_set_src(wp, "A:lvgl/examples/libs/sjpg/small_image.sjpg");
    lv_obj_align(wp, LV_ALIGN_RIGHT_MID, -20, 0);

    lv_obj_t * img2 = lv_img_create(lv_scr_act());
    /* Assuming a File system is attached to letter 'A'
     * E.g. set LV_USE_FS_STDIO 'A' in lv_conf.h */
    lv_img_set_src(img2, "A:lvgl/examples/libs/sjpg/lv_example_jpg.jpg");
    //lv_obj_center(img);
    lv_obj_align(img2, LV_ALIGN_TOP_RIGHT, -20, 20);

    lv_obj_t * img3 = lv_img_create(lv_scr_act());
    /* Assuming a File system is attached to letter 'A'
     * E.g. set LV_USE_FS_STDIO 'A' in lv_conf.h */
#if LV_COLOR_DEPTH == 32
    lv_img_set_src(img3, "A:lvgl/examples/libs/bmp/example_32bit.bmp");
#elif LV_COLOR_DEPTH == 16
    lv_img_set_src(img, "A:lvgl/examples/libs/bmp/example_16bit.bmp");
#endif
    lv_obj_align(img3, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_obj_t * img4 = lv_img_create(lv_scr_act());
    lv_img_set_src(img4, "A:lvgl/examples/libs/ffmpeg/ffmpeg.png");
    lv_obj_align(img4, LV_ALIGN_BOTTOM_LEFT, 20, -20);

    lv_obj_t * player = lv_ffmpeg_player_create(lv_scr_act());
    lv_ffmpeg_player_set_src(player, "./lvgl/examples/libs/ffmpeg/birds.mp4");
    lv_ffmpeg_player_set_auto_restart(player, true);
    lv_ffmpeg_player_set_cmd(player, LV_FFMPEG_PLAYER_CMD_START);
    lv_obj_align(player, LV_ALIGN_TOP_MID, 0, 20);
}
#endif




/* --autologin 用：延时 1.2 秒后触发一次自动登录（只有广告机有登录页） */
#if defined(APP_ADVERT)
static void autologin_timer_cb(lv_timer_t *t)
{
  lv_timer_del(t);
  login_debug_auto();
}
#endif

/**
 * 单实例保护。
 *
 * 两个 GUI 进程同时跑 = 各往 /dev/fb0 里画自己的界面，谁也不让谁，
 * 最后屏幕上就是"启动动画的字压在广告机卡片上、底下还在滚公告"那种鬼样子。
 * 看起来像 UI 错乱，其实是进程打架。
 *
 * 用 flock 加锁：拿到锁的继续跑，后来者直接退出并告诉使用者该怎么办。
 * 进程被 kill 掉时内核会自动释放锁，不会有残留死锁。
 * 注意这把锁是 demo 和 weather 共用的 —— 两个程序都吃 /dev/fb0，互斥才有意义。
 * 返回锁文件描述符（要保持打开），失败返回 -1。
 */
static int single_instance_guard(void)
{
  int fd = open("/tmp/lvgl-fb.lock", O_CREAT | O_RDWR, 0666);
  if(fd < 0) return 0;   /* 打不开锁文件就放行，别因为保护把自己卡死 */

  if(flock(fd, LOCK_EX | LOCK_NB) != 0) {
    char buf[32] = {0};
    if(read(fd, buf, sizeof(buf) - 1) > 0) {
      char *nl = strpbrk(buf, "\r\n");
      if(nl != NULL) *nl = '\0';
    }
    fprintf(stderr,
            "[%s] 已经有一个界面程序在跑（PID %s）。两个进程会同时往 framebuffer 画，\n"
            "       屏幕会变成两套 UI 叠在一起。先结束旧的再启动：\n"
            "       pkill -9 demo; pkill -9 weather\n",
            APP_TAG_NAME, buf[0] ? buf : "?");
    close(fd);
    return -1;
  }

  /* 拿到锁了，把 PID 写进去给后来者看 */
  if(ftruncate(fd, 0) == 0) {
    char pid[24];
    int n = snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
    if(n > 0) (void)!write(fd, pid, (size_t)n);
  }
  return fd;
}

int main(int argc, char **argv)
{
  /* 板端一般会把 stdout 重定向到文件，默认的 4KB 全缓冲会让"程序到底说了什么"
   * 一直看不到 —— 排查时很容易误判成没执行到。改成行缓冲，说一句出一句。 */
  setvbuf(stdout, NULL, _IOLBF, 0);

  /* 单实例：两个实例抢 framebuffer 会把界面画成两套叠在一起 */
  int lock_fd = single_instance_guard();
  if(lock_fd < 0) return 1;
  (void)lock_fd;   /* 保持打开，进程退出时内核自动释放 */

  /* GEC6818 没有 RTC 电池，rootfs 里也没有 /etc/localtime，
     不指定时区的话 localtime() 就按 UTC 走，界面上的钟会比北京时间少 8 小时。 */
  setenv("TZ", "CST-8", 1);
  tzset();

  /* 调试用：
   *   ./demo             正常开机 → 动画 → 登录 → 广告机
   *   ./demo --login     跳过开机动画，直接进登录页
   *   ./demo --ad        跳过动画和登录，直接进广告机（调界面时最省时间）
   *   ./demo --autologin 跳过动画，进登录页后自动用内置账号登录一次
   * 天气程序没有参数，跑起来就是天气界面（see build/bin/weather）。
   */
  bool skip_boot  = false;
  bool skip_login = false;
  bool auto_login = false;
  bool want_weather = false;
  for(int i = 1; i < argc; i++) {
    if(strcmp(argv[i], "--ad") == 0)         { skip_boot = true; skip_login = true; }
    if(strcmp(argv[i], "--login") == 0)      { skip_boot = true; }
    if(strcmp(argv[i], "--autologin") == 0)  { skip_boot = true; auto_login = true; }
    if(strcmp(argv[i], "--weather") == 0)    { want_weather = true; }
  }

#ifdef APP_WEATHER
  /* 天气程序不认广告机那几个参数；--weather 保留成兼容写法（本程序天生就是天气界面） */
  (void)skip_boot; (void)skip_login; (void)auto_login; (void)want_weather;
#endif

  /*Initialize LVGL*/
  lv_init();

  /*Initialize the HAL (display, input devices, tick) for LVGL*/
  hal_init();

  //  lv_example_switch_1();
  //  lv_example_calendar_1();
  //  lv_example_btnmatrix_2();
  //  lv_example_checkbox_1();
  //  lv_example_colorwheel_1();
  //  lv_example_chart_6();
  //  lv_example_table_2();
  //  lv_example_scroll_2();
  //  lv_example_textarea_1();
  //  lv_example_msgbox_1();
  //  lv_example_dropdown_2();
  //  lv_example_btn_1();
  //  lv_example_scroll_1();
  //  lv_example_tabview_1();
  //  lv_example_tabview_1();
  //  lv_example_flex_3();
  //  lv_example_label_1();

  // lv_demo_widgets();
  //  lv_demo_keypad_encoder();
  //  lv_demo_benchmark();
  //  lv_demo_stress();
  //  lv_demo_music();

  //  user_image_demo();

  //--------------------------------
  // 测试

  // lv_bling_imagebtn_2();  //png图片
  // lv_bling_gif_1(); //gif图片

  // homework_ui();
  // lv_bling_msgbox_1();
  // lv_example_keyboard_1();
  // lv_bling_list_1();
  // lv_bling_tabview_1();
  // lv_bling_textarea_1();
  // lv_bling_stylesheet_1();
  // lv_bling_border_stylesheet_2();
  // lv_example_style_2();
  // lv_example_style_5();
  // font_test();
  // lv_bling_label_3();
  // timer_button_test();
  // lv_bling_grid_1();
  // lv_bling_flex_3();
  // lv_login_register_demo();
#ifdef APP_WEATHER
  /* 天气程序：起来就是天气界面（初始化含后台拉取线程 + IoT 连接） */
  weather_app_init();
  weather_app_show_ui();
#else
  if(want_weather) {
    printf("[demo] 天气界面已经拆成独立程序了，这里只剩下广告机。\n"
           "[demo] 想看天气：板上执行 ./weather（与 ./demo 放在同一目录）\n");
  }
  if(skip_login) {
    advert_ui_show();
  }
  else if(skip_boot) {
    btn_wind();
    if(auto_login) {
      /* 等登录界面建好再模拟点击，1.2 秒足够 */
      lv_timer_t *t = lv_timer_create(autologin_timer_cb, 1200, NULL);
      if(t != NULL) lv_timer_set_repeat_count(t, 1);
    }
  }
  else {
    boot_screen_show(btn_wind);
  }
#endif
  // show_main_win();


  //--------------------------------

  while (1)
  {
    /* Periodically call the lv_task handler.
     * It could be done in a timer interrupt or an OS task too.*/
    lv_timer_handler();
    usleep(5 * 1000);
  }

  hal_deinit();
  return 0;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Initialize the Hardware Abstraction Layer (HAL) for the LVGL graphics
 * library
 */
static void hal_init(void)
{
  /* mouse input device */
  static lv_indev_drv_t indev_drv_1;
  lv_indev_drv_init(&indev_drv_1);
  indev_drv_1.type = LV_INDEV_TYPE_POINTER;

  /* keyboard input device */
  static lv_indev_drv_t indev_drv_2;
  lv_indev_drv_init(&indev_drv_2);
  indev_drv_2.type = LV_INDEV_TYPE_KEYPAD;

  /* mouse scroll wheel input device */
  static lv_indev_drv_t indev_drv_3;
  lv_indev_drv_init(&indev_drv_3);
  indev_drv_3.type = LV_INDEV_TYPE_ENCODER;

  lv_group_t *g = lv_group_create();
  lv_group_set_default(g);

  lv_disp_t *disp = NULL;

#if USE_SDL
  /* Use the 'monitor' driver which creates window on PC's monitor to simulate a display*/
  sdl_init();

  /*Create a display buffer*/
  static lv_disp_draw_buf_t disp_buf1;
  static lv_color_t buf1_1[MONITOR_HOR_RES * 100];
  static lv_color_t buf1_2[MONITOR_HOR_RES * 100];
  lv_disp_draw_buf_init(&disp_buf1, buf1_1, buf1_2, MONITOR_HOR_RES * 100);

  /*Create a display*/
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv); /*Basic initialization*/
  disp_drv.draw_buf = &disp_buf1;
  disp_drv.flush_cb = sdl_display_flush;
  disp_drv.hor_res = MONITOR_HOR_RES;
  disp_drv.ver_res = MONITOR_VER_RES;
  disp_drv.antialiasing = 1;

  disp = lv_disp_drv_register(&disp_drv);

  /* Add the input device driver */
  // mouse_init();
  indev_drv_1.read_cb = sdl_mouse_read;

  // keyboard_init();
  indev_drv_2.read_cb = sdl_keyboard_read;

  // mousewheel_init();
  indev_drv_3.read_cb = sdl_mousewheel_read;

#elif USE_X11
  lv_x11_init("LVGL Simulator Demo", DISP_HOR_RES, DISP_VER_RES);

  /*Create a display buffer*/
  static lv_disp_draw_buf_t disp_buf1;
  static lv_color_t buf1_1[DISP_HOR_RES * 100];
  static lv_color_t buf1_2[DISP_HOR_RES * 100];
  lv_disp_draw_buf_init(&disp_buf1, buf1_1, buf1_2, DISP_HOR_RES * 100);

  /*Create a display*/
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.draw_buf = &disp_buf1;
  disp_drv.flush_cb = lv_x11_flush;
  disp_drv.hor_res = DISP_HOR_RES;
  disp_drv.ver_res = DISP_VER_RES;
  disp_drv.antialiasing = 1;

  disp = lv_disp_drv_register(&disp_drv);

  /* Add the input device driver */
  indev_drv_1.read_cb = lv_x11_get_pointer;
  indev_drv_2.read_cb = lv_x11_get_keyboard;
  indev_drv_3.read_cb = lv_x11_get_mousewheel;

#elif USE_FBDEV
  /* ---- GEC6818 开发板：/dev/fb0 显示 + /dev/input/event0 触摸 ---- */
  fbdev_init();

  /* 分辨率以 framebuffer 驱动上报的为准，编译期常量只用来分配缓冲区 */
  uint32_t fb_w = 0, fb_h = 0;
  fbdev_get_sizes(&fb_w, &fb_h, NULL);
  if (fb_w != FBDEV_HOR_RES || fb_h != FBDEV_VER_RES)
  {
    LV_LOG_WARN("framebuffer is %ux%u, build-time setting is %dx%d",
                (unsigned)fb_w, (unsigned)fb_h, FBDEV_HOR_RES, FBDEV_VER_RES);
  }
  if (fb_w == 0 || fb_w > FBDEV_HOR_RES)
    fb_w = FBDEV_HOR_RES;
  if (fb_h == 0 || fb_h > FBDEV_VER_RES)
    fb_h = FBDEV_VER_RES;

  /*Create a display buffer*/
  static lv_disp_draw_buf_t disp_buf1;
  static lv_color_t buf1_1[FBDEV_HOR_RES * FBDEV_DRAW_BUF_LINES];
  static lv_color_t buf1_2[FBDEV_HOR_RES * FBDEV_DRAW_BUF_LINES];
  lv_disp_draw_buf_init(&disp_buf1, buf1_1, buf1_2, FBDEV_HOR_RES * FBDEV_DRAW_BUF_LINES);

  /*Create a display*/
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.draw_buf = &disp_buf1;
  disp_drv.flush_cb = fbdev_flush;
  disp_drv.hor_res = fb_w;
  disp_drv.ver_res = fb_h;
  disp_drv.antialiasing = 1;

  disp = lv_disp_drv_register(&disp_drv);

  /* Add the touchscreen (gslX680) as pointer input device */
  evdev_init();
  indev_drv_1.read_cb = evdev_read;

  /* ---- 调试：ADVERT_TAP=x,y[,ms] 模拟点击；ADVERT_SWIPE=x1,y1,x2,y2[,ms] 模拟滑动 ----
   * 板上没有键鼠、也没法从外部往 /dev/input 注入事件，改完界面根本不知道
   * "点得到点不到、划得动划不动"。这里挂一个假的指针设备，走 LVGL 自己的
   * 命中测试和手势识别，和真手指是同一条路径。
   * 滑动必须"分多次小步移动"，一次性跳过去不会触发 LV_EVENT_GESTURE。 */
  {
    const char *sw = getenv("ADVERT_SWIPE");
    const char *tp = getenv("ADVERT_TAP");

    int x1 = 0, y1 = 0, x2 = 0, y2 = 0, tms = 0;
    bool ok = false;

    if(sw != NULL && sscanf(sw, "%d,%d,%d,%d,%d", &x1, &y1, &x2, &y2, &tms) >= 4) {
      s_swipe_on   = true;
      s_swipe_a.x  = (lv_coord_t)x1;
      s_swipe_a.y  = (lv_coord_t)y1;
      s_swipe_b.x  = (lv_coord_t)x2;
      s_swipe_b.y  = (lv_coord_t)y2;
      if(tms > 0) { s_tap_at = (uint32_t)tms; }
      ok = true;
    }
    else if(tp != NULL && sscanf(tp, "%d,%d,%d", &x1, &y1, &tms) >= 2) {
      s_tap_pt.x = (lv_coord_t)x1;
      s_tap_pt.y = (lv_coord_t)y1;
      if(tms > 0) { s_tap_at = (uint32_t)tms; }
      ok = true;
    }

    if(ok) {
      static lv_indev_drv_t tap_drv;
      s_tap_t0 = lv_tick_get();

      lv_indev_drv_init(&tap_drv);
      tap_drv.type    = LV_INDEV_TYPE_POINTER;
      tap_drv.read_cb = tap_indev_read;
      lv_indev_drv_register(&tap_drv);

      if(s_swipe_on) {
        printf("[tap] %u ms 后模拟滑动 (%d,%d) -> (%d,%d)，用时 %u ms\n",
               (unsigned)s_tap_at, x1, y1, x2, y2, (unsigned)s_swipe_ms);
      }
      else {
        printf("[tap] %u ms 后模拟点击 (%d,%d)\n", (unsigned)s_tap_at, s_tap_pt.x, s_tap_pt.y);
      }
      fflush(stdout);
    }
  }
#endif
  /* Set diplay theme */
  lv_theme_t *th = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED), LV_THEME_DEFAULT_DARK, LV_FONT_DEFAULT);
  lv_disp_set_theme(disp, th);

  /* Tick init */
  end_tick = false;
  pthread_create(&thr_tick, NULL, tick_thread, NULL);

  /* register input devices */
  lv_indev_t *mouse_indev = lv_indev_drv_register(&indev_drv_1);
  (void)mouse_indev; /* 板子上没有键盘/编码器，也不需要画鼠标指针 */

#if USE_SDL || USE_X11
  lv_indev_t *kb_indev = lv_indev_drv_register(&indev_drv_2);
  lv_indev_t *enc_indev = lv_indev_drv_register(&indev_drv_3);
  lv_indev_set_group(kb_indev, g);
  lv_indev_set_group(enc_indev, g);

  /* Set a cursor for the mouse */
  LV_IMG_DECLARE(mouse_cursor_icon);                  /*Declare the image file.*/
  lv_obj_t *cursor_obj = lv_img_create(lv_scr_act()); /*Create an image object for the cursor*/
  lv_img_set_src(cursor_obj, &mouse_cursor_icon);     /*Set the image source*/
  lv_indev_set_cursor(mouse_indev, cursor_obj);       /*Connect the image  object to the driver*/
#endif
}

/**
 * Releases the Hardware Abstraction Layer (HAL) for the LVGL graphics library
 */
static void hal_deinit(void)
{
  end_tick = true;
  pthread_join(thr_tick, NULL);

#if USE_SDL
  // nop
#elif USE_X11
  lv_x11_deinit();
#elif USE_FBDEV
  fbdev_exit(); /* evdev 驱动没有提供 exit 接口，进程退出即释放 */
#endif
}

/**
 * A task to measure the elapsed time for LVGL
 * @param data unused
 * @return never return
 */
static void *tick_thread(void *data)
{
  (void)data;

  while (!end_tick)
  {
    usleep(5000);
    lv_tick_inc(5); /*Tell LittelvGL that 5 milliseconds were elapsed*/
  }

  return NULL;
}
