#ifndef __LOGIN_H__
#define __LOGIN_H__

/**
 *    创建两个文本框示例（密码框和普通文本框）
 *        并附带虚拟键盘功能
 */

extern void btn_wind(void);//登录

/* 调试用：自动填入内置账号并触发一次登录（配合 ./demo --autologin） */
extern void login_debug_auto(void);

#endif