#include "lvgl/lvgl.h"
#include "lvgl/demos/lv_demos.h"
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>


#include <stdio.h>
#include <string.h>                            // 用于字符串操作函数
#include "lv_font_source_han_sans_bold.h" // 中文支持
#include "my_win.h"
#include "login.h"

/* 背景图（boot_bg.c 提供的 LV_IMG_CF_TRUE_COLOR 800x480 位图） */
extern const lv_img_dsc_t boot_bg;

/* ==================== 配色（DeepSeek 深蓝主题，适配明亮背景图） ==================== */
#define C_CARD_BG      0x0e1a38   /* 卡片深蓝底 */
#define C_CARD_BG_OPA  230         /* 卡片不透明度 */
#define C_FIELD_BG     0xffffff   /* 输入框底 */
#define C_FIELD_BG_OPA 32          /* 输入框不透明度 */
#define C_FIELD_BORDER 0x5b7fb8   /* 输入框边框 */
#define C_ACCENT       0x2f6fed   /* 登录按钮主题蓝 */
#define C_ACCENT_DARK  0x1e4fc0   /* 登录按钮按下态 */
#define C_TEXT         0xffffff   /* 主文字 */
#define C_TEXT_MUTED   0xbcc8e0   /* 次要文字 */

/* 全局变量声明 */
static lv_obj_t * kb;           // 虚拟键盘对象指针
static char account[20]  = {0}; // 存储当前输入的账号
static char password[20] = {0}; // 存储当前输入的密码
static lv_obj_t * login_btn;    // 登录按钮
static lv_obj_t * register_btn; // 注册按钮
static lv_obj_t * tip_label;    // 提示信息标签
static lv_obj_t * account_ta;   // 账号输入框（注册成功后要清空，必须记指针）
static lv_obj_t * pwd_ta;       // 密码输入框（同上）
static lv_obj_t * bg_img;       // 背景图
static lv_obj_t * card;         // 表单卡片容器
static lv_obj_t * account_label; // 账号标签（可点击）
static lv_obj_t * pwd_label;     // 密码标签（可点击）

// 新增：用户数据库（模拟存储注册信息）
#define MAX_USERS 10  // 最大用户数
typedef struct {
    char account[20];
    char password[20];
} User;

/* 出厂内置账号：上电就能直接登录，不用先注册（密码长度需 >= 6，符合注册校验规则）。
   想换账号改下面两行即可；改成 "" 就等于取消内置账号。 */
#define DEFAULT_ACCOUNT  "admin"
#define DEFAULT_PASSWORD "123456"

static User user_db[MAX_USERS] = {
    { DEFAULT_ACCOUNT, DEFAULT_PASSWORD },   /* index 0：内置账号，占用 1 个名额 */
};
static int user_count = 1;            // 已注册用户数量（含内置的这 1 个）


/**
 * 新增：检查账号是否已注册
 * 返回值：1=已注册，0=未注册
 */
static int is_account_registered(const char *acc) {
    for (int i = 0; i < user_count; i++) {
        if (strcmp(user_db[i].account, acc) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * 新增：验证账号密码是否匹配（登录核心逻辑）
 * 返回值：1=匹配，0=不匹配
 */
static int verify_login(const char *acc, const char *pwd) {
    for (int i = 0; i < user_count; i++) {
        if (strcmp(user_db[i].account, acc) == 0 && 
            strcmp(user_db[i].password, pwd) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * 新增：跳转至主页面（登录成功后，由一次性定时器调用一次）
 */
static void go_to_main_page(lv_timer_t * t) {
    LV_UNUSED(t);

    /* 注意：lv_obj_clean(lv_scr_act()) 会删除屏幕的所有子对象，键盘 kb 就是
       屏幕的子对象，已经被一起删掉了。原代码在 clean 之后又 lv_obj_del(kb)，
       属于对已释放内存二次释放，必崩。这里只把指针置空，绝不再 del。 */
    kb         = NULL;
    tip_label  = NULL;
    account_ta = NULL;
    pwd_ta     = NULL;
    bg_img     = NULL;
    card       = NULL;
    account_label = NULL;
    pwd_label     = NULL;

    lv_obj_clean(lv_scr_act()); // 清空登录界面（键盘随之销毁）
    show_main_win();            // 创建主界面
}

/**
 * 点击"账号/密码"标签：弹出键盘并聚焦到对应输入框。
 * 输入框本身常驻显示，这里只负责把键盘关联过去。
 */
static void field_label_cb(lv_event_t * e) {
    lv_obj_t * ta = (lv_obj_t *)lv_event_get_user_data(e);
    if (ta == NULL) return;
    printf("[login] field_label_cb user_data=%p (account=%p pwd=%p)\n", (void*)ta, (void*)account_ta, (void*)pwd_ta);
    if (kb != NULL) {
        lv_keyboard_set_textarea(kb, ta);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * 文本框事件回调（输入处理）
 */
static void ta_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);
    char * user_data = (char *)lv_event_get_user_data(e);

    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        if (kb != NULL) {
            lv_keyboard_set_textarea(kb, ta); // 关联键盘到输入框
            lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 关键改动：原来只在 LV_EVENT_READY（按键盘上的 ✓）时才保存文本。
       但用户完全可能敲完字直接点"登录"按钮 —— 那时 READY 从没触发过，
       account/password 还是空字符串，登录必被判成"不能为空"。
       所以这里改成：任何事件都同步一次文本，保证点按钮时数据是最新的。 */
    if (code != LV_EVENT_DELETE && ta != NULL) {
        const char * text = lv_textarea_get_text(ta);   // 密码模式下返回明文
        if (text == NULL) text = "";
        if (strcmp(user_data, "account") == 0) {
            strncpy(account, text, sizeof(account)-1);
            account[sizeof(account)-1] = '\0';
        } else if (strcmp(user_data, "password") == 0) {
            strncpy(password, text, sizeof(password)-1);
            password[sizeof(password)-1] = '\0';
        }
    }
}

/**
 * 登录按钮事件回调（完善登录逻辑）
 */
static void login_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {

        // 1. 空值校验
        if (strlen(account) == 0 || strlen(password) == 0) {
            lv_label_set_text(tip_label, "提示: 账号或密码不能为空");
            return;
        }

        // 2. 检查账号是否注册（未注册则无法登录）
        if (!is_account_registered(account)) {
            lv_label_set_text(tip_label, "登录失败: 账号未注册, 请先注册");

            
            return;
        }

        // 3. 验证账号密码是否匹配
        if (verify_login(account, password)) {
            lv_label_set_text(tip_label, "登录成功, 正在跳转...");
            /* 一次性定时器：0.8 秒后跳转主页面。
               lv_timer_create 的默认 repeat_count 是 -1（永久重复），
               不显式设成 1 的话 go_to_main_page 会每秒执行一次，
               每次都 lv_obj_clean 把刚建好的主界面清空重建。 */
            lv_timer_t * t = lv_timer_create(go_to_main_page, 800, NULL);
            if (t != NULL) lv_timer_set_repeat_count(t, 1);
        } else {
            lv_label_set_text(tip_label, "登录失败: 密码错误");
        }
        /* 原来这里还有一句无条件 show_main_win()：登录失败也会跳转，
           提示刚 set 就被主界面盖掉。已删除 —— 跳转只发生在成功分支里。 */
    }
}

/**
 * 注册按钮事件回调（完善注册逻辑）
 */
static void register_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        // 1. 空值校验
        if (strlen(account) == 0 || strlen(password) == 0) {
            lv_label_set_text(tip_label, "提示: 账号或密码不能为空");
            return;
        }

        // 2. 账号长度校验（至少3位）
        if (strlen(account) < 3) {
            lv_label_set_text(tip_label, "注册失败:账号至少3个字符");
            return;
        }

        // 3. 密码长度校验（至少6位）
        if (strlen(password) < 6) {
            lv_label_set_text(tip_label, "注册失败: 密码至少6个字符");
            return;
        }

        // 4. 检查账号是否已注册
        if (is_account_registered(account)) {
            lv_label_set_text(tip_label, "注册失败: 账号已被注册");
            return;
        }

        // 5. 检查数据库是否已满
        if (user_count >= MAX_USERS) {
            lv_label_set_text(tip_label, "注册失败: 用户数量已达上限");
            return;
        }

        // 6. 注册成功：保存到数据库
        strncpy(user_db[user_count].account, account, sizeof(user_db[user_count].account)-1);
        strncpy(user_db[user_count].password, password, sizeof(user_db[user_count].password)-1);
        user_count++; // 用户数+1

        lv_label_set_text(tip_label, "注册成功! 请登录");
        /* 清空输入框。原来用 lv_obj_get_child(lv_scr_act(), 2) 靠索引猜，
           但屏幕子对象的真实顺序是：
             0 tip_label  1 账号框  2 账号"标签"  3 密码框  4 密码"标签"
             5 登录按钮   6 注册按钮  7 键盘
           索引 2 和 4 拿到的是 lv_label，lv_textarea_set_text 里
           LV_ASSERT_OBJ 会失败，而 LV_ASSERT_HANDLER 正是 while(1) —— 直接死机。
           改用创建时保存的真实指针。 */
        if (account_ta != NULL) lv_textarea_set_text(account_ta, "");
        if (pwd_ta != NULL)     lv_textarea_set_text(pwd_ta, "");
    }
}

/**
 * 输入框统一样式：半透明白底、圆角、淡蓝描边、白字、中文占位符
 */
static void style_field(lv_obj_t * ta, const char * placeholder) {
    lv_obj_set_style_bg_color(ta, lv_color_hex(C_FIELD_BG), 0);
    lv_obj_set_style_bg_opa(ta, C_FIELD_BG_OPA, 0);
    lv_obj_set_style_radius(ta, 10, 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(C_FIELD_BORDER), 0);
    lv_obj_set_style_text_font(ta, &chinese_ziku, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_pad_left(ta, 12, 0);
    lv_obj_set_style_pad_right(ta, 12, 0);
    lv_obj_set_style_pad_top(ta, 8, 0);
    lv_obj_set_style_pad_bottom(ta, 8, 0);
    /* 聚焦时高亮边框 */
    lv_obj_set_style_border_color(ta, lv_color_hex(C_ACCENT), LV_STATE_FOCUSED);
    if (placeholder != NULL) {
        lv_textarea_set_placeholder_text(ta, placeholder);
        lv_obj_set_style_text_color(ta, lv_color_hex(0x8fa3c8),
                                    LV_PART_TEXTAREA_PLACEHOLDER | LV_STATE_DEFAULT);
    }
}

/**
 * 按钮统一样式
 */
static void style_btn(lv_obj_t * btn, int filled) {
    if (filled) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(C_ACCENT_DARK), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 12, 0);
        lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(btn, 80, 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(C_FIELD_BG), 0);
        lv_obj_set_style_bg_opa(btn, 30, 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(C_FIELD_BORDER), 0);
    }
    lv_obj_set_style_radius(btn, 10, 0);
}

/**
 * 创建登录/注册界面
 */
void btn_wind(void) {
    /* 1. 背景图铺满全屏（动漫风格卡通人物背景） */
    bg_img = lv_img_create(lv_scr_act());
    lv_img_set_src(bg_img, &boot_bg);
    lv_obj_set_pos(bg_img, 0, 0);

    /* 2. 顶部标题：英文大字 + 中文副标题 */
    lv_obj_t * title_en = lv_label_create(lv_scr_act());
    lv_label_set_text(title_en, "WELCOME BACK");
    lv_obj_set_style_text_font(title_en, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title_en, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_bg_color(title_en, lv_color_hex(C_CARD_BG), 0);
    lv_obj_set_style_bg_opa(title_en, 150, 0);
    lv_obj_set_style_radius(title_en, 14, 0);
    lv_obj_set_style_pad_top(title_en, 6, 0);
    lv_obj_set_style_pad_bottom(title_en, 6, 0);
    lv_obj_set_style_pad_left(title_en, 20, 0);
    lv_obj_set_style_pad_right(title_en, 20, 0);
    lv_obj_align(title_en, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_t * title_zh = lv_label_create(lv_scr_act());
    lv_label_set_text(title_zh, "欢迎登录");
    lv_obj_set_style_text_font(title_zh, &chinese_ziku, 0);
    lv_obj_set_style_text_color(title_zh, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_bg_color(title_zh, lv_color_hex(C_CARD_BG), 0);
    lv_obj_set_style_bg_opa(title_zh, 150, 0);
    lv_obj_set_style_radius(title_zh, 12, 0);
    lv_obj_set_style_pad_top(title_zh, 4, 0);
    lv_obj_set_style_pad_bottom(title_zh, 4, 0);
    lv_obj_set_style_pad_left(title_zh, 16, 0);
    lv_obj_set_style_pad_right(title_zh, 16, 0);
    lv_obj_align(title_zh, LV_ALIGN_TOP_MID, 0, 56);

    /* 3. 半透明毛玻璃卡片容器 */
    card = lv_obj_create(lv_scr_act());
    lv_obj_set_size(card, 460, 190);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD_BG), 0);
    lv_obj_set_style_bg_opa(card, C_CARD_BG_OPA, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_shadow_width(card, 24, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(card, 90, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 4. 账号输入框（常驻显示）+ 标签（可点击聚焦） */
    account_ta = lv_textarea_create(card);
    lv_textarea_set_one_line(account_ta, true);
    lv_textarea_set_password_mode(account_ta, false);
    lv_obj_set_size(account_ta, 300, 42);
    lv_obj_align(account_ta, LV_ALIGN_TOP_RIGHT, -22, 14);
    lv_obj_add_event_cb(account_ta, ta_event_cb, LV_EVENT_ALL, (void *)"account");
    style_field(account_ta, "请输入账号");
    /* 账号框常驻显示，不再隐藏 —— 用户要能直接看到账号内容 */

    account_label = lv_btn_create(card);
    lv_obj_set_size(account_label, 74, 42);
    lv_obj_align(account_label, LV_ALIGN_TOP_LEFT, 16, 14);
    lv_obj_set_style_bg_opa(account_label, 0, 0);
    lv_obj_set_style_bg_opa(account_label, 40, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(account_label, lv_color_hex(C_TEXT), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(account_label, 0, 0);
    lv_obj_set_style_shadow_width(account_label, 0, 0);
    lv_obj_set_style_radius(account_label, 8, 0);
    lv_obj_add_event_cb(account_label, field_label_cb, LV_EVENT_CLICKED, account_ta);
    {
        lv_obj_t * l = lv_label_create(account_label);
        lv_label_set_text(l, "账号");
        lv_obj_set_style_text_font(l, &chinese_ziku, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
        lv_obj_center(l);
    }

    /* 5. 密码输入框（常驻显示，密码用 * 遮蔽）+ 标签（可点击聚焦） */
    pwd_ta = lv_textarea_create(card);
    lv_textarea_set_text(pwd_ta, "");
    lv_textarea_set_password_mode(pwd_ta, true);
    lv_textarea_set_one_line(pwd_ta, true);
    lv_obj_set_size(pwd_ta, 300, 42);
    lv_obj_align(pwd_ta, LV_ALIGN_TOP_RIGHT, -22, 74);
    lv_obj_add_event_cb(pwd_ta, ta_event_cb, LV_EVENT_ALL, (void *)"password");
    style_field(pwd_ta, "请输入密码");
    /* 密码遮蔽符：默认 bullet 是 U+2022，chinese_ziku 里没有该字形会显示空白，
       改用 ASCII '*'（chinese_ziku 一定覆盖）。密码框常驻显示，输入内容以 * 呈现。 */
    lv_textarea_set_password_bullet(pwd_ta, "*");

    pwd_label = lv_btn_create(card);
    lv_obj_set_size(pwd_label, 74, 42);
    lv_obj_align(pwd_label, LV_ALIGN_TOP_LEFT, 16, 74);
    lv_obj_set_style_bg_opa(pwd_label, 0, 0);
    lv_obj_set_style_bg_opa(pwd_label, 40, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(pwd_label, lv_color_hex(C_TEXT), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(pwd_label, 0, 0);
    lv_obj_set_style_shadow_width(pwd_label, 0, 0);
    lv_obj_set_style_radius(pwd_label, 8, 0);
    lv_obj_add_event_cb(pwd_label, field_label_cb, LV_EVENT_CLICKED, pwd_ta);
    {
        lv_obj_t * l = lv_label_create(pwd_label);
        lv_label_set_text(l, "密码");
        lv_obj_set_style_text_font(l, &chinese_ziku, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
        lv_obj_center(l);
    }

    /* 6. 登录按钮（主题蓝实心） */
    login_btn = lv_btn_create(card);
    lv_obj_set_size(login_btn, 190, 44);
    lv_obj_align(login_btn, LV_ALIGN_BOTTOM_LEFT, 22, -14);
    lv_obj_add_event_cb(login_btn, login_event_cb, LV_EVENT_CLICKED, NULL);
    style_btn(login_btn, 1);

    lv_obj_t * login_label = lv_label_create(login_btn);
    lv_label_set_text(login_label, "登录");
    lv_obj_set_style_text_font(login_label, &chinese_ziku, 0);
    lv_obj_set_style_text_color(login_label, lv_color_hex(C_TEXT), 0);
    lv_obj_center(login_label);

    /* 7. 注册按钮（半透明描边） */
    register_btn = lv_btn_create(card);
    lv_obj_set_size(register_btn, 190, 44);
    lv_obj_align(register_btn, LV_ALIGN_BOTTOM_RIGHT, -22, -14);
    lv_obj_add_event_cb(register_btn, register_event_cb, LV_EVENT_CLICKED, NULL);
    style_btn(register_btn, 0);

    lv_obj_t * register_label = lv_label_create(register_btn);
    lv_label_set_text(register_label, "注册");
    lv_obj_set_style_text_font(register_label, &chinese_ziku, 0);
    lv_obj_set_style_text_color(register_label, lv_color_hex(C_TEXT), 0);
    lv_obj_center(register_label);

    /* 8. 提示标签（卡片下方） */
    tip_label = lv_label_create(lv_scr_act());
    lv_label_set_text(tip_label, "点击输入框或标签进行输入");
    lv_obj_set_style_text_font(tip_label, &chinese_ziku, 0);
    lv_obj_set_style_text_color(tip_label, lv_color_hex(0xffe08a), 0);
    lv_obj_set_width(tip_label, lv_pct(92));
    lv_obj_set_style_text_align(tip_label, LV_TEXT_ALIGN_CENTER, 0);
    /* 加半透明深色底，避免提示文字压在背景人物上难以辨认 */
    lv_obj_set_style_bg_color(tip_label, lv_color_hex(0x0e1a38), 0);
    lv_obj_set_style_bg_opa(tip_label, 170, 0);
    lv_obj_set_style_radius(tip_label, 12, 0);
    lv_obj_set_style_pad_top(tip_label, 4, 0);
    lv_obj_set_style_pad_bottom(tip_label, 4, 0);
    lv_obj_set_style_pad_left(tip_label, 12, 0);
    lv_obj_set_style_pad_right(tip_label, 12, 0);
    lv_obj_align(tip_label, LV_ALIGN_TOP_MID, 0, 292);

    /* 9. 创建虚拟键盘（全宽、贴底部，初始隐藏） */
    kb = lv_keyboard_create(lv_scr_act());
    lv_obj_set_size(kb, LV_HOR_RES, LV_VER_RES / 3);
    /* 初始不关联、隐藏键盘：进入页面时先看到账号/密码框，点击输入框或标签才弹出键盘 */
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

}

// 获取账号和密码（外部接口）
static const char * get_account(void) { return account; }
static const char * get_password(void) { return password; }

/**
 * 仅供调试：把内置账号填进两个输入框，再模拟点一次「登录」。
 *
 * 板上没有键鼠、也没法从外部注入触摸事件，想验证
 * 「登录 → 广告机」这条链路时就用它（main.c 的 --autologin）。
 * 走的是和用户手输完全一样的路径：set_text 触发 VALUE_CHANGED →
 * ta_event_cb 同步缓冲 → CLICKED → login_event_cb → 一次性定时器 → show_main_win。
 */
void login_debug_auto(void)
{
    if(account_ta == NULL || pwd_ta == NULL || login_btn == NULL) {
        printf("[login] 调试自动登录失败：界面还没建好\n");
        return;
    }

    lv_textarea_set_text(account_ta, DEFAULT_ACCOUNT);
    lv_textarea_set_text(pwd_ta, DEFAULT_PASSWORD);
    lv_event_send(login_btn, LV_EVENT_CLICKED, NULL);
}
