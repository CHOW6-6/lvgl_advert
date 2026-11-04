#ifndef __TEST_H__
#define __TEST_H__

#include <stdlib.h>
#include <stdio.h> /* printf()，回调函数里要用 */
#include <unistd.h>
#include <pthread.h>
#include "lv_drv_conf.h"
#include "lvgl/lvgl.h"
#include "lvgl/examples/lv_examples.h"
#include "lvgl/demos/lv_demos.h"

/* --- 低层驱动：按平台三选一，选完就把条件编译关掉 --- */
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
#include "lv_font_source_han_sans_bold.h" //中文字体的头文件

#endif

/* --- 布局常量：与显示驱动无关，board / sim 两种编译下都要可见 --- */
#define PHOTO_BOX_W   236   /*相框宽*/
#define PHOTO_BOX_H   276   /*相框高*/
#define BTN_W         150
#define BTN_H         60

/* --- test.c 对外提供的接口 --- */
extern void lv_bling_imagebtn_2(void);   /*png 图片按钮*/
extern void lv_bling_gif_1(void);        /*gif 动图*/

extern void homework_ui(void);           /*作业：相框 + 三个按钮*/
extern void lv_bling_msgbox_1(void);     /*消息框*/
extern void lv_example_keyboard_1(void); /*键盘*/
extern void lv_bling_list_1(void);       /*列表*/
extern void lv_bling_tabview_1(void);    /*标签页*/
extern void lv_bling_textarea_1(void);   /*文本框 + 按钮矩阵*/
extern void lv_bling_stylesheet_1(void); /*样式表*/
extern void lv_bling_border_stylesheet_2(void); /*样式表：带边框*/
extern void lv_example_style_2(void);      /*样式表：带边框*/
extern void lv_example_style_5(void);
extern void font_test();
extern void lv_bling_label_3(void);
extern void timer_button_test();
extern void lv_bling_grid_1(void);
extern void lv_bling_flex_3(void);




#endif /* __TEST_H__ */
