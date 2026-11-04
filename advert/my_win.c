/**
 * @file my_win.c
 * @brief 登录成功后的入口。
 *
 * 原来这里是一堆 VIDEO / MUSIC 的示例窗口（还依赖板上不存在的
 * A:/root/nuonuo.png 图标），现在统一交给 advert_ui 接管：
 * 登录 → 广告机主界面（播放 / 广告库 / 信息 三页）。
 *
 * 保留 show_main_win() 这个名字，是为了不动 main.c 与 login.c 里的调用点。
 */

#include "my_win.h"
#include "advert_ui.h"

void show_main_win(void)
{
    advert_ui_show();
}
