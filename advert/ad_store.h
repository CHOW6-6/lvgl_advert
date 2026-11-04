/**
 * @file ad_store.h
 * @brief 广告清单数据层：扫描 <素材根目录> 下的 meta 与 media
 *
 * 板上目录约定（AD_ROOT 默认取可执行文件同级的 advert/，即 /root/lvgl-demo/advert）：
 *
 *   AD_ROOT/
 *   ├── meta/                    每个广告一个 .conf（文件名即排序依据）
 *   │   └── 01_watch.conf        一行：类型|标题|副标题|帧率|停留秒
 *   └── media/
 *       └── 01_watch/
 *           ├── cover.jpg        704x340 正片/封面（展示用）
 *           ├── thumb.jpg        232x130 缩略图（广告库用）
 *           └── f_000.jpg ...    帧序列（仅视频广告有）
 *
 * 这样设计的好处：客户端上传一个新广告 = 传 media 目录 + 传一个 meta 文件，
 * 板上不需要维护中心化的 playlist，也就没有"并发改同一张表"的竞态。
 */

#ifndef AD_STORE_H
#define AD_STORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AD_MAX_ITEMS     64
#define AD_TITLE_LEN     128
#define AD_SUBTITLE_LEN  80
#define AD_REMARK_LEN    160
#define AD_NAME_LEN      48
#define AD_PATH_LEN      320
#define AD_MAX_FRAMES    600
#define AD_NOTICE_LEN    256

/* 广告类型 */
typedef enum {
    AD_TYPE_IMAGE = 0,   /* 纯图片：停留 hold_ms 后切下一条 */
    AD_TYPE_VIDEO = 1    /* 帧序列：按 fps 逐帧播放，播完切下一条 */
} ad_type_t;

/* 单条广告 */
typedef struct {
    ad_type_t type;
    char title[AD_TITLE_LEN];      /* 中文标题（UTF-8） */
    char subtitle[AD_SUBTITLE_LEN];/* 英文副标题 */
    char remark[AD_REMARK_LEN];    /* 客户备注（第 6 个字段，老清单没有则为空） */
    char name[AD_NAME_LEN];        /* meta 文件名去掉 .conf，即 media 子目录名 */
    char cover[AD_PATH_LEN];       /* 站内绝对路径（不带 A: 前缀） */
    char thumb[AD_PATH_LEN];
    int  fps;                      /* 视频帧率；图片为 0 */
    int  frame_count;              /* 视频帧数；图片为 0 */
    int  hold_ms;                  /* 图片停留时间 */
} ad_item_t;

/* 设置根目录。优先级：参数 > 环境变量 ADVERT_ROOT > ./advert（存在时）> /root/lvgl-demo/advert */
void ad_store_init(const char *root);
const char *ad_store_root(void);

/**
 * 全量扫描。返回 true 表示内容与上次不同（需要重建 UI）。
 * 首次调用总是返回 true。
 */
bool ad_store_scan(void);

/**
 * 轻量轮询：只 stat 一次 meta 目录，mtime 变了才真正重新扫描。
 * 返回 true 表示清单有变化。建议 1 秒调一次。
 */
bool ad_store_poll(void);

int  ad_store_count(void);
const ad_item_t *ad_store_get(int idx);

/* 取第 idx 条广告第 frame 帧的绝对路径，写进 buf。成功返回 true。 */
bool ad_store_frame_path(int idx, int frame, char *buf, int bufsz);

/* ---------------- 底部滚动公告 ---------------- */

/**
 * 公告文案存放在 <AD_ROOT>/ticker.conf（UTF-8，只取第一行）。
 * 文件不存在或读不出来时，回落到内置文案。
 * 放成文件而不是编译进去，是为了能用客户端远程改：
 *     ./adclient.py ticker --text "新的公益文案"
 */
const char *ad_store_ticker(void);

/* 检查 ticker.conf 是否变化，变了就重读（版本号 +1）。建议每秒调一次。 */
void ad_store_refresh_ticker(void);

/* 文案版本号：界面拿它和自己记的值比，不一致才去更新 label */
uint32_t ad_store_ticker_gen(void);

#ifdef __cplusplus
}
#endif

#endif /* AD_STORE_H */
