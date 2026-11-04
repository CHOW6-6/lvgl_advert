#include "test.h"
#include "lv_font_source_han_sans_bold.h" //中文字体的头文件


//------------------------------------------------------------------------------------------
// 图标按钮在(x,y)的坐标位置 放了一个w宽度*h高度大小的按键 说明：做照片的大小最好和按键的大小一致
lv_obj_t *lv_bling_imgbtn(int x, int y, int w, int h, char *pic_path);
void lv_bling_imagebtn_2(void);
void lv_bling_gif_1(void);

lv_obj_t *lv_bling_imgbtn(int x, int y, int w, int h, char *pic_path)
{
    /*Darken the button when pressed and make it wider*/
    // 添加按键的样式表，使他有一个动态的效果
    static lv_style_t style_pr; // 样式表
    lv_style_init(&style_pr);
    lv_style_set_img_recolor_opa(&style_pr, LV_OPA_30);
    lv_style_set_img_recolor(&style_pr, lv_color_black());
    lv_style_set_transform_height(&style_pr, 20);

    /*Create an image button*/
    lv_obj_t *imgbtn = lv_imgbtn_create(lv_scr_act());
    if (imgbtn == NULL)
    {
        LV_LOG_ERROR("lv_imgbtn_create fail:"); // 输出错误提示
        return NULL;
    }

    lv_obj_set_size(imgbtn, w, h); // 设置图片按键的大小w*h
    lv_imgbtn_set_src(imgbtn, LV_IMGBTN_STATE_RELEASED, NULL, pic_path, NULL);
    // lv_obj_add_style(imgbtn1, &style_def, 0);
    lv_obj_add_style(imgbtn, &style_pr, LV_STATE_PRESSED);

    //(x和y按键偏移的位置--起始坐标位置)
    lv_obj_align(imgbtn, LV_ALIGN_TOP_LEFT, x, y);

    return imgbtn;
}
// 按键的回调函数
static void video_btn_event_callback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    // 点击按键之后所处理的事情在这个if语句里面实现
    if (code == LV_EVENT_CLICKED)
    {
        printf("video.png\n");
        // LV_LOG_USER("Clicked");
    }
}

// 显示图像按钮
void lv_bling_imagebtn_2()
{
    /* 盘符必须和 lv_conf.h 里 LV_FS_STDIO_LETTER 一致（当前是 'A'），
     * 写成 "S:" 时 LVGL 找不到对应驱动，根本不会去 fopen，只会刷
     * "Image draw cannot open the image resource"。'A' 后面的部分就是板上真实路径。 */
    lv_obj_t *video = lv_bling_imgbtn(100, 100, 212, 200, "A:/root/feibi.png");
    lv_obj_add_event_cb(video, video_btn_event_callback, LV_EVENT_CLICKED, NULL);
}

/*********************************gif***********************************/
/*
    gif动态图的demo
*/
void lv_bling_gif_1(void)
{
    /*
        如何实现gif动态图的控件
    */
    // 显示gif在坐标(200,0)的位置放了一张gif图
    // 实例化一个gif的对象
    lv_obj_t *gif1 = lv_gif_create(lv_scr_act());
    // 设置gif图的坐标位置
    lv_obj_set_pos(gif1, 0, 0);
    // lv_obj_align(gif1, LV_ALIGN_RIGHT_MID, -20, 0); //在起始坐标的位置左移20
    // 设置gif图的路径
    /* 同样是盘符问题：必须用 lv_conf.h 里注册的 'A'，写成 "S:" 会直接打不开。
     * 另外这张 gif 是 480x270，解码要占约 520KB 的 LVGL 堆（LV_MEM_SIZE）。 */
    lv_gif_set_src(gif1, "A:/root/jige.gif");
    printf("show gif\n");
}
//------------------------------------------------------------------------------------------

/*********************************btn***********************************/

/*每个按钮对应一张图片：A: 是 LV_USE_FS_STDIO 里配置的盘符*/
static const char *g_photo_path[] = {
    "A:test/nuonuo.png",
    "A:test/feibi.png",
    "A:test/nailong.png",
};

static lv_obj_t *g_img;         /*显示照片的 img 控件*/
static lv_obj_t *g_placeholder; /*初始占位文字"照片"*/

/*把图片按比例缩放并居中放进相框*/
static void photo_show(const char *path)
{
    lv_img_header_t header;
    if (lv_img_decoder_get_info(path, &header) != LV_RES_OK)
    {
        LV_LOG_WARN("open image failed: %s (check LV_USE_PNG / path)", path);
        return;
    }

    lv_obj_add_flag(g_placeholder, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_img, LV_OBJ_FLAG_HIDDEN);

    lv_img_set_src(g_img, path);

    int32_t zw = (PHOTO_BOX_W - 16) * 256 / (int32_t)header.w;
    int32_t zh = (PHOTO_BOX_H - 16) * 256 / (int32_t)header.h;
    int32_t zoom = (zw < zh) ? zw : zh;
    if (zoom > 256)
        zoom = 256; /*放得下就不放大*/
    lv_img_set_zoom(g_img, (uint16_t)zoom);
    lv_obj_center(g_img);
}

/*按钮回调：user_data 就是该按钮对应的图片路径*/
static void photo_btn_event_cb(lv_event_t *e)
{
    const char *path = (const char *)lv_event_get_user_data(e);
    photo_show(path);
}

/*创建一个按钮*/
static lv_obj_t *create_btn(lv_obj_t *parent, const char *txt, const char *path)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, photo_btn_event_cb, LV_EVENT_CLICKED, (void *)path);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, txt);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_center(label);
    return btn;
}

void homework_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xF4F6F8), 0);
    lv_obj_set_style_pad_all(scr, 16, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /*========== 左：相框 ==========*/
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_set_size(box, PHOTO_BOX_W, PHOTO_BOX_H);
    lv_obj_set_style_radius(box, 18, 0);
    lv_obj_set_style_clip_corner(box, true, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_shadow_width(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    g_placeholder = lv_label_create(box);
    lv_obj_set_style_text_font(g_placeholder, &lv_font_simsun_16_cjk, 0);
    lv_label_set_text(g_placeholder, "照片");
    lv_obj_center(g_placeholder);

    g_img = lv_img_create(box);
    lv_obj_add_flag(g_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(g_img);

    /*========== 右：三个按钮竖排 ==========*/
    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_remove_style_all(col); /*去掉背景/边框，只当容器*/
    lv_obj_set_size(col, BTN_W, PHOTO_BOX_H);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    create_btn(col, "first", g_photo_path[0]);
    create_btn(col, "second", g_photo_path[1]);
    create_btn(col, "third", g_photo_path[2]);
}

//--------------------------------------------------------------

/*********************************msgbox***********************************/
static void event_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    LV_LOG_USER("Button %s clicked", lv_msgbox_get_active_btn_text(obj));

    // 如果是lv_msgbox_get_active_btn_text(obj) == "apply" 表示确定
    // 如果是lv_msgbox_get_active_btn_text(obj) == "close" 表示不确定
    // 用strcmp函数
}

void lv_bling_msgbox_1(void)
{
    static const char *btns[] = {"Apply", "Close", ""};
    /*
        "Hello"是title标签
        "This is a message box with two buttons."是文本信息
    */
    lv_obj_t *mbox1 = lv_msgbox_create(NULL, "Hello", "This is a message box with two buttons.", btns, true);
    lv_obj_add_event_cb(mbox1, event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(mbox1);
}

//--------------------------------------------------------------
// keyboard
static void ta_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    lv_obj_t *kb = lv_event_get_user_data(e);
    if (code == LV_EVENT_FOCUSED)
    {
        lv_keyboard_set_textarea(kb, ta);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }

    if (code == LV_EVENT_DEFOCUSED)
    {
        lv_keyboard_set_textarea(kb, NULL);
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
}

void lv_example_keyboard_1(void)
{
    /*Create a keyboard to use it with an of the text areas*/
    lv_obj_t *kb = lv_keyboard_create(lv_scr_act());

    /*Create a text area. The keyboard will write here*/
    lv_obj_t *ta;
    ta = lv_textarea_create(lv_scr_act());
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_add_event_cb(ta, ta_event_cb, LV_EVENT_ALL, kb);
    lv_textarea_set_placeholder_text(ta, "Hello");
    lv_obj_set_size(ta, 140, 80);

    ta = lv_textarea_create(lv_scr_act());
    lv_obj_align(ta, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_add_event_cb(ta, ta_event_cb, LV_EVENT_ALL, kb);
    lv_obj_set_size(ta, 140, 80);

    lv_keyboard_set_textarea(kb, ta);
}

//----------------------------------------------------------------

/*********************************list列表***********************************/
static lv_obj_t *list1;

static void event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_target(e);
    if (code == LV_EVENT_CLICKED)
    {
        LV_LOG_USER("Clicked: %s", lv_list_get_btn_text(list1, obj));
    }
}

void lv_bling_list_1(void)
{
    /*Create a list*/
    list1 = lv_list_create(lv_scr_act());
    lv_obj_set_size(list1, 180, 220);
    lv_obj_center(list1);

    /*Add buttons to the list*/
    lv_obj_t *btn;

    lv_list_add_text(list1, "File");
    btn = lv_list_add_btn(list1, LV_SYMBOL_FILE, "New");
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);
    btn = lv_list_add_btn(list1, LV_SYMBOL_DIRECTORY, "Open");
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);
    btn = lv_list_add_btn(list1, LV_SYMBOL_SAVE, "Save");
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);
    btn = lv_list_add_btn(list1, LV_SYMBOL_CLOSE, "Delete");
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);
    btn = lv_list_add_btn(list1, LV_SYMBOL_EDIT, "Edit");
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);

    lv_list_add_text(list1, "Connectivity");
    btn = lv_list_add_btn(list1, LV_SYMBOL_BLUETOOTH, "Bluetooth");
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);
    btn = lv_list_add_btn(list1, LV_SYMBOL_GPS, "Navigation");
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);
    btn = lv_list_add_btn(list1, LV_SYMBOL_USB, "USB");
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);
    btn = lv_list_add_btn(list1, LV_SYMBOL_BATTERY_FULL, "Battery");
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);

    lv_list_add_text(list1, "Exit");
    btn = lv_list_add_btn(list1, LV_SYMBOL_OK, "Apply");
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);
    btn = lv_list_add_btn(list1, LV_SYMBOL_CLOSE, "Close");
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);
}

//----------------------------------------------------------------
// table
void lv_bling_tabview_1(void)
{
    /*Create a Tab view object*/
    lv_obj_t *tabview;
    tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 50);

    /*Add 3 tabs (the tabs are page (lv_page) and can be scrolled*/
    lv_obj_t *tab1 = lv_tabview_add_tab(tabview, "Tab 1");
    lv_obj_t *tab2 = lv_tabview_add_tab(tabview, "Tab 2");
    lv_obj_t *tab3 = lv_tabview_add_tab(tabview, "Tab 3");

    /*Add content to the tabs*/
    lv_obj_t *label = lv_label_create(tab1);
    lv_label_set_text(label, "This the first tab\n\n"
                             "If the content\n"
                             "of a tab\n"
                             "becomes too\n"
                             "longer\n"
                             "than the\n"
                             "container\n"
                             "then it\n"
                             "automatically\n"
                             "becomes\n"
                             "scrollable.\n"
                             "\n"
                             "\n"
                             "\n"
                             "Can you see it?");

    label = lv_label_create(tab2);
    lv_label_set_text(label, "Second tab");

    label = lv_label_create(tab3);
    lv_label_set_text(label, "Third tab");

    lv_obj_scroll_to_view_recursive(label, LV_ANIM_ON);
}

//---------------------------------------------------------------
// test area
static void textarea_event_handler(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    // 这个打印的是你文本框输入的内容
    LV_LOG_USER("Enter was pressed. The current text is: %s", lv_textarea_get_text(ta));
}

static void btnm_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_obj_t *ta = lv_event_get_user_data(e);
    const char *txt = lv_btnmatrix_get_btn_text(obj, lv_btnmatrix_get_selected_btn(obj));

    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0)
        lv_textarea_del_char(ta);
    else if (strcmp(txt, LV_SYMBOL_NEW_LINE) == 0)
        lv_event_send(ta, LV_EVENT_READY, NULL);
    else
        lv_textarea_add_text(ta, txt);
}

void lv_bling_textarea_1(void)
{
    lv_obj_t *ta = lv_textarea_create(lv_scr_act());
    lv_textarea_set_one_line(ta, true);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_add_event_cb(ta, textarea_event_handler, LV_EVENT_READY, ta);
    lv_obj_add_state(ta, LV_STATE_FOCUSED); /*To be sure the cursor is visible*/

    static const char *btnm_map[] = {"1", "2", "3", "\n",
                                     "4", "5", "6", "\n",
                                     "7", "8", "9", "\n",
                                     LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_NEW_LINE, ""};

    lv_obj_t *btnm = lv_btnmatrix_create(lv_scr_act());
    lv_obj_set_size(btnm, 200, 150); // 设置键盘的大小
    lv_obj_align(btnm, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(btnm, btnm_event_handler, LV_EVENT_VALUE_CHANGED, ta);
    lv_obj_clear_flag(btnm, LV_OBJ_FLAG_CLICK_FOCUSABLE); /*To keep the text area focused on button clicks*/
    lv_btnmatrix_set_map(btnm, btnm_map);
}

//----------------------------------------------------------------

//------------------------样式表1---------------------------------
void lv_bling_stylesheet_1(void)
{
    lv_obj_t *widget = lv_obj_create(lv_scr_act());
    lv_obj_set_size(widget, 100, 100);
    lv_obj_set_align(widget, LV_ALIGN_CENTER);

    // 1、定义一个样式变量
    static lv_style_t style;
    // 2、初始化变量
    lv_style_init(&style);
    // 3、往样式变量中设置背景颜色
    lv_style_set_bg_color(&style, lv_color_hex(0xff0000));
    // 4、将样式变量 添加到对象中
    lv_obj_add_style(widget, &style, 0);
}

//--------------------------------------

// ----------------设置样式表边框-------------------
void lv_bling_border_stylesheet_2(void)
{
    // 得到当前屏幕的界面对象
    lv_obj_t *parent = lv_scr_act();
    int width = lv_obj_get_width(parent);
    int height = lv_obj_get_height(parent);

    lv_obj_t *btn1 = lv_btn_create(parent);
    lv_obj_set_size(btn1, 100, 100);

    lv_obj_t *btn2 = lv_btn_create(parent);
    lv_obj_set_size(btn2, 100, 100);
    lv_obj_align(btn2, LV_ALIGN_CENTER, 0, 0);

    // 初始化样式
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_bg_color(&style, lv_color_hex(0xff0000));
    lv_style_set_border_width(&style, 10);
    lv_style_set_border_color(&style, lv_color_hex(0x00ff00));

    // 把设置好的样式生效于控件
    lv_obj_add_style(btn1, &style, LV_PART_MAIN);
    lv_obj_add_style(btn2, &style, LV_PART_MAIN);
}

// ----------------设置样式表边框-------------------

// ----------------设置样式表背景-------------------
/**
 * Using the background style properties
 */
void lv_example_style_2(void)
{
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_radius(&style, 5);

    /*Make a gradient*/
    lv_style_set_bg_opa(&style, LV_OPA_COVER);
    lv_style_set_bg_color(&style, lv_palette_lighten(LV_PALETTE_GREY, 1));
    lv_style_set_bg_grad_color(&style, lv_palette_main(LV_PALETTE_BLUE));
    lv_style_set_bg_grad_dir(&style, LV_GRAD_DIR_VER);

    /*Shift the gradient to the bottom*/
    lv_style_set_bg_main_stop(&style, 128);
    lv_style_set_bg_grad_stop(&style, 192);

    /*Create an object with the new style*/
    lv_obj_t * obj = lv_obj_create(lv_scr_act());
    lv_obj_add_style(obj, &style, 0);
    lv_obj_center(obj);
}

// ----------------设置样式表边框-------------------

//---------------------设置样式表背景-------------------
/**
 * Using the Shadow style properties
 */
void lv_example_style_5(void)
{
    static lv_style_t style;
    lv_style_init(&style);

    /*Set a background color and a radius*/
    lv_style_set_radius(&style, 5);
    lv_style_set_bg_opa(&style, LV_OPA_COVER);
    lv_style_set_bg_color(&style, lv_palette_lighten(LV_PALETTE_GREY, 1));

    /*Add a shadow*/
    lv_style_set_shadow_width(&style, 55);
    lv_style_set_shadow_color(&style, lv_palette_main(LV_PALETTE_BLUE));
//    lv_style_set_shadow_ofs_x(&style, 10);
//    lv_style_set_shadow_ofs_y(&style, 20);

    /*Create an object with the new style*/
    lv_obj_t * obj = lv_obj_create(lv_scr_act());
    lv_obj_add_style(obj, &style, 0);
    lv_obj_center(obj);
}

//---------------------设置样式表背景-------------------


//---------------------设置样式表字体-------------------
static void event_font_handler(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_PRESSED) {
        printf("ok\n");
    }
}
void font_test()
{
    /*
    显示中文字的说明:要添加 chinese_ziku.c lv_font_source_han_sans_bold_20.c两个文件   
    */
    //在按键上面显示中文标签
    lv_obj_t * btn1 = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btn1,200,200);
    lv_obj_set_pos(btn1,400,240);
    lv_obj_add_event_cb(btn1, event_font_handler, LV_EVENT_PRESSED, NULL);

    lv_obj_t * wendu = lv_label_create(btn1);
    lv_obj_align(wendu,LV_ALIGN_CENTER,0,0);
    lv_label_set_text(wendu,"上一张");
    //20字体的大小
    lv_obj_set_style_text_font(wendu,&lv_font_source_han_sans_bold_20,0);
    //20字体的大小粗体 
    lv_obj_set_style_text_font(wendu,&chinese_ziku,0);
    
    //直接显示中文label标签
     lv_obj_t * label = lv_label_create(lv_scr_act());
    //设置文本内容
    lv_label_set_text(label, "中文好丝滑!");
    //设置label的大小：
    lv_obj_set_width(label, 300); // 设置宽度为100px
    lv_obj_set_height(label, 50); // 设置高度为50px        
    //设置字体的大小
    lv_obj_set_style_text_font(label,&lv_font_source_han_sans_bold_20,0);       
    //设置标签的位置
    lv_obj_set_pos(label,100,100);   
    
}

//-------------------------------------------------------------

//-------------------------滚动中文字体-------------------------
void lv_bling_label_3(void)
{
    lv_obj_t * label = lv_label_create(lv_scr_act());
    //设置标签的模式为滚动的模式
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);     /*Circular scroll*/
    lv_obj_set_width(label, 800);
    lv_label_set_recolor(label, true); 
    //注意：字体显示的内容要超过你自定义的区域大小
    lv_label_set_text(label,"#0000ff 辉哥神了,辉哥魔了,辉哥没有JJ,辉哥神了,辉哥魔了,辉哥没有JJ,辉哥神了,辉哥魔了,辉哥没有JJ,辉哥神了,辉哥魔了,辉哥没有JJ,辉哥神了,辉哥魔了,辉哥没有JJ,#"); //设置文本颜色的格式   
    lv_obj_set_style_text_font(label, &chinese_ziku, 0); 
    // lv_label_set_text(label, "It is a circularly scrolling text. ");
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 240);
}
//-------------------------------------------------------------
//两个按键测试定时器
lv_timer_t * timer1 = NULL;
void my_timer_1(lv_timer_t * timer)
{
  /*Use the user_data*/
  uint32_t * user_data = timer->user_data;
  printf("my_timer called with user data: %d\n", *user_data);
}

static void event_handler_bnt_11(lv_event_t * e) //按键1启动定时器
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_PRESSED) {
        printf("ok\n");
        // lv_timer_ready(timer1);  //启动定时器
        lv_timer_resume(timer1); //恢复定时器
    }
}

static void event_handler_bnt_12(lv_event_t * e)//按键2暂停定时器
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_PRESSED) {
        printf("no ok\n");
        // lv_timer_del(timer1); //删除定时器
        lv_timer_pause(timer1); //暂停
    }
}

void timer_button_test()
{
    //初始化一个定时器(每500ms执行一个函数my_timer_1)
    static uint32_t user_data = 10;
    //创建一个定时器每500ms执行一次回调函数my_timer_1，同时传参数user_data
    timer1 = lv_timer_create(my_timer_1, 500,  &user_data);
    lv_timer_pause(timer1); // 暂停(定时器默认是开启的状态,可以先暂停一下)
    
    //按键对象1
    lv_obj_t * btn1 = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btn1,100,100);//设置按键btn的size大小200*200
    lv_obj_set_pos(btn1,600,100); //设置按键btn的位置(400,240)  
    lv_obj_add_event_cb(btn1, event_handler_bnt_11, LV_EVENT_PRESSED, NULL);    
    //显示文字在按键btn上
    lv_obj_t * label1 = lv_label_create(btn1);//实例化一个标签label对象
    lv_label_set_text(label1, "开定时器");//给标签注释
    lv_obj_set_style_text_font(label1,&lv_font_source_han_sans_bold_20,0);    
    lv_obj_center(label1); //将文本放在按键的中间  
    
    //按键对象2
    lv_obj_t * btn2 = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btn2,100,100);//设置按键btn的size大小200*200
    lv_obj_set_pos(btn2,600,300); //设置按键btn的位置(400,240)  
    lv_obj_add_event_cb(btn2, event_handler_bnt_12, LV_EVENT_PRESSED, NULL);    
    //显示文字在按键btn上
    lv_obj_t * label2 = lv_label_create(btn2);//实例化一个标签label对象
    lv_label_set_text(label2, "关定时器");//给标签注释
    lv_obj_set_style_text_font(label2,&lv_font_source_han_sans_bold_20,0); 
    lv_obj_center(label2); //将文本放在按键的中间  

}

//-------------------------------------------------------------

//-------------------------------------------------------------
//栅格布局
/**
 * A simple grid
 */
void lv_bling_grid_1(void)
{
    static lv_coord_t col_dsc[] = {70, 70, 70, LV_GRID_TEMPLATE_LAST};//定义数组3列每一列的的宽度70
    static lv_coord_t row_dsc[] = {50, 50, 50, LV_GRID_TEMPLATE_LAST};//定义数组3行每一行的的宽度50

    /*Create a container with grid*/
    lv_obj_t * cont = lv_obj_create(lv_scr_act()); //创建一个容器，用于布局栅格
    lv_obj_set_style_grid_column_dsc_array(cont, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(cont, row_dsc, 0);
    lv_obj_set_size(cont, 300, 220); //设置容器的大小
    lv_obj_center(cont); //容器的位置
    lv_obj_set_layout(cont, LV_LAYOUT_GRID); //设置栅格布局

    lv_obj_t * label;
    lv_obj_t * obj;

    //设置9个按键,存放在容器cont中;每个按键宽70高50按照栅格自动布局存放在容器cont中
    uint32_t i;
    for(i = 0; i < 9; i++) {
        uint8_t col = i % 3;
        uint8_t row = i / 3;

        obj = lv_btn_create(cont);
        /*Stretch the cell horizontally and vertically too
         *Set span to 1 to make the cell 1 column/row sized*/
        lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_STRETCH, col, 1,
                                  LV_GRID_ALIGN_STRETCH, row, 1);

        label = lv_label_create(obj);
        lv_label_set_text_fmt(label, "c%d, r%d", col, row);
        lv_obj_center(label);
    }
}
//-------------------------------------------------------------

/*********************************flex弹性布局***********************************/
void lv_bling_flex_3(void)
{
    //定义了一个容器
    lv_obj_t * cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(cont, 300, 220);
    lv_obj_center(cont);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);

    //将下面这4个对象放到上面的容器中（按行自动排列布局）
    lv_obj_t * btn1 = lv_btn_create(cont);
    lv_obj_set_height(btn1, 40);
    // lv_obj_add_event_cb(btn1, event_handler_bnt_12, LV_EVENT_PRESSED, NULL); 
    lv_obj_set_flex_grow(btn1, 1);//占据这一行的一等份的位置

    lv_obj_t * btn2 = lv_btn_create(cont);
    lv_obj_set_height(btn2, 40);
    // lv_obj_add_event_cb(btn2, event_handler_bnt_12, LV_EVENT_PRESSED, NULL); 
    lv_obj_set_flex_grow(btn2, 1);  //占据这一行的一等份的位置

    lv_obj_t * btn3 = lv_btn_create(cont);
    lv_obj_set_height(btn3, 40);
    // lv_obj_add_event_cb(btn3, event_handler_bnt_12, LV_EVENT_PRESSED, NULL); 
    lv_obj_set_flex_grow(btn3, 1);  //占据这一行的一等份的位置

    lv_obj_t * btn4 = lv_btn_create(cont);
    lv_obj_set_height(btn4, 40);
    // lv_obj_add_event_cb(btn4, event_handler_bnt_12, LV_EVENT_PRESSED, NULL); 
    lv_obj_set_flex_grow(btn4, 1);  //占据这一行的一等份的位置
}

//-------------------------------------------------