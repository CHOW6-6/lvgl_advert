/**
 * @file ad_store.c
 * @brief 广告清单数据层实现（纯 C，无 LVGL 依赖，方便单独测试）
 */

#include "ad_store.h"

#include <dirent.h>
#include <limits.h>      /* PATH_MAX（realpath 用） */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ====================== 内部状态 ====================== */

static char      s_root[AD_PATH_LEN] = "/root/lvgl-demo/advert";
static char      s_meta_dir[AD_PATH_LEN];
static ad_item_t s_items[AD_MAX_ITEMS];
static int       s_count    = 0;
static uint64_t  s_meta_key = 0;
static uint32_t  s_hash     = 0;
static bool      s_inited   = false;

/* 底部滚动公告 */
#define TICKER_DEFAULT \
    "公益广告 · 节约用水，从点滴做起　·　文明出行，礼让斑马线　·　本广告位由 深蓝公益基金 提供"

static char     s_ticker[512];
static uint64_t s_ticker_key   = 0;
static uint32_t s_ticker_gen   = 0;
static bool     s_ticker_dirty = true;

/* ====================== 小工具 ====================== */

/* 目录 mtime 的粗粒度键：任何增删文件都会更新它 */
static uint64_t dir_key(const char *path)
{
    struct stat st;
    if(stat(path, &st) != 0) return 0;
    return (uint64_t)st.st_mtime * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
}

static uint32_t fnv1a(uint32_t h, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    for(size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t fnv_str(uint32_t h, const char *s)
{
    return fnv1a(h, s, strlen(s));
}

/* 读文件第一行，去掉行尾换行与 UTF-8 BOM；成功返回 true */
static bool read_first_line(const char *path, char *out, size_t cap)
{
    FILE *fp = fopen(path, "r");
    if(fp == NULL) return false;

    size_t n = fread(out, 1, cap - 1, fp);
    fclose(fp);
    out[n] = '\0';

    char *brk = strpbrk(out, "\r\n");
    if(brk != NULL) *brk = '\0';

    if((unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB &&
       (unsigned char)out[2] == 0xBF) {
        memmove(out, out + 3, strlen(out + 3) + 1);
    }
    return out[0] != '\0';
}

/* 按 '|' 切分，最多 maxf 段，不足的补空串 */
static void split_fields(char *s, char *f[], int maxf)
{
    int n = 0;
    f[n++] = s;
    while(n < maxf) {
        char *bar = strchr(f[n - 1], '|');
        if(bar == NULL) break;
        *bar = '\0';
        f[n++] = bar + 1;
    }
    while(n < maxf) f[n++] = (char *)"";
}

static int atoi_safe(const char *s, int def)
{
    if(s == NULL || *s == '\0') return def;
    int v = atoi(s);
    return v <= 0 ? def : v;
}

/* 统计某个目录下 f_000.jpg / f_001.jpg … 的连续帧数 */
static int count_frames(const char *dir)
{
    char p[AD_PATH_LEN];
    int  n = 0;
    while(n < AD_MAX_FRAMES) {
        snprintf(p, sizeof(p), "%s/f_%03d.jpg", dir, n);
        if(access(p, R_OK) != 0) break;
        n++;
    }
    return n;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* ====================== 对外接口 ====================== */

void ad_store_init(const char *root)
{
    const char *env = getenv("ADVERT_ROOT");
    struct stat st;

    if (root != NULL) {
        snprintf(s_root, sizeof(s_root), "%s", root);
    } else if (env != NULL && env[0] != '\0') {
        snprintf(s_root, sizeof(s_root), "%s", env);
    } else if (stat("./advert", &st) == 0 && S_ISDIR(st.st_mode)) {
        /* 板上的标准布局：可执行文件和 advert/ 同级（部署时 cd 进去再启动）。
         * 以前这里写死 /root/lvgl-weather/advert，结果广告机搬到别的目录就"暂无广告"了。 */
        snprintf(s_root, sizeof(s_root), "./advert");
    } else {
        snprintf(s_root, sizeof(s_root), "/root/lvgl-demo/advert");
    }

    /* 落成绝对路径：界面"信息"页要把素材目录显示给人看，显示 "./advert"
     * 等于什么都没说；而且日志里也是绝对路径更好排查。 */
    {
        char resolved[PATH_MAX];
        if(realpath(s_root, resolved) != NULL && strlen(resolved) < sizeof(s_root)) {
            snprintf(s_root, sizeof(s_root), "%s", resolved);
        }
    }
    size_t l = strlen(s_root);
    while(l > 1 && s_root[l - 1] == '/') s_root[--l] = '\0';

    snprintf(s_meta_dir, sizeof(s_meta_dir), "%s/meta", s_root);

    s_inited   = true;
    s_meta_key = dir_key(s_meta_dir);
    s_hash     = 0;
    s_count    = 0;

    s_ticker[0]    = '\0';
    s_ticker_key   = 0;
    s_ticker_gen   = 0;
    s_ticker_dirty = true;
}

const char *ad_store_root(void)
{
    if(!s_inited) ad_store_init(NULL);
    return s_root;
}

bool ad_store_scan(void)
{
    if(!s_inited) ad_store_init(NULL);

    char names[AD_MAX_ITEMS][AD_NAME_LEN];
    int  name_cnt = 0;

    DIR *d = opendir(s_meta_dir);
    if(d != NULL) {
        struct dirent *e;
        while((e = readdir(d)) != NULL && name_cnt < AD_MAX_ITEMS) {
            size_t l = strlen(e->d_name);
            if(l <= 5 || strcmp(e->d_name + l - 5, ".conf") != 0) continue;

            size_t base = l - 5;
            if(base >= AD_NAME_LEN) base = AD_NAME_LEN - 1;
            memcpy(names[name_cnt], e->d_name, base);
            names[name_cnt][base] = '\0';
            name_cnt++;
        }
        closedir(d);
    }

    /* 文件名排序 = 播放顺序，客户端用 01_/02_ 前缀控制 */
    qsort(names, (size_t)name_cnt, AD_NAME_LEN, cmp_name);

    int      count = 0;
    uint32_t hash  = 2166136261u;
    char     line[512];

    for(int i = 0; i < name_cnt && count < AD_MAX_ITEMS; i++) {
        char conf[AD_PATH_LEN];
        snprintf(conf, sizeof(conf), "%s/%s.conf", s_meta_dir, names[i]);

        if(!read_first_line(conf, line, sizeof(line))) continue;

        char  buf[512];
        snprintf(buf, sizeof(buf), "%s", line);

        char *f[6];
        split_fields(buf, f, 6);

        ad_item_t *it = &s_items[count];
        memset(it, 0, sizeof(*it));

        it->type = (f[0][0] == 'V' || f[0][0] == 'v') ? AD_TYPE_VIDEO : AD_TYPE_IMAGE;
        snprintf(it->title, sizeof(it->title), "%s", f[1]);
        snprintf(it->subtitle, sizeof(it->subtitle), "%s", f[2]);
        snprintf(it->remark, sizeof(it->remark), "%s", f[5]);   /* 老清单只有 5 段，这里是空串 */
        snprintf(it->name, sizeof(it->name), "%s", names[i]);
        it->fps     = atoi_safe(f[3], 12);
        it->hold_ms = atoi_safe(f[4], 5) * 1000;

        /* 路径 */
        char dir[AD_PATH_LEN];
        snprintf(dir, sizeof(dir), "%s/media/%s", s_root, it->name);

        snprintf(it->cover, sizeof(it->cover), "%s/cover.jpg", dir);
        if(access(it->cover, R_OK) != 0) {
            /* 没有封面说明这个广告不完整（可能上传到一半），跳过 */
            continue;
        }
        snprintf(it->thumb, sizeof(it->thumb), "%s/thumb.jpg", dir);
        if(access(it->thumb, R_OK) != 0) {
            snprintf(it->thumb, sizeof(it->thumb), "%s", it->cover);
        }

        if(it->type == AD_TYPE_VIDEO) {
            it->frame_count = count_frames(dir);
            if(it->frame_count < 1) continue;   /* 视频没有帧，当坏数据丢掉 */
        }

        hash = fnv_str(hash, it->name);
        hash = fnv_str(hash, it->title);
        hash = fnv1a(hash, &it->fps, sizeof(it->fps));
        hash = fnv1a(hash, &it->frame_count, sizeof(it->frame_count));

        count++;
    }

    bool changed = (count != s_count) || (hash != s_hash) || (s_hash == 0);
    s_count    = count;
    s_hash     = hash;
    s_meta_key = dir_key(s_meta_dir);

    return changed;
}

bool ad_store_poll(void)
{
    if(!s_inited) ad_store_init(NULL);

    uint64_t key = dir_key(s_meta_dir);
    if(key == s_meta_key) return false;

    s_meta_key = key;      /* 先记下，避免 scan 里重复触发 */
    bool changed = ad_store_scan();
    /* 目录变了但内容没变（比如 touch），也算一次"重扫"但不需要重建 UI */
    return changed;
}

int ad_store_count(void)
{
    return s_count;
}

const ad_item_t *ad_store_get(int idx)
{
    if(idx < 0 || idx >= s_count) return NULL;
    return &s_items[idx];
}

bool ad_store_frame_path(int idx, int frame, char *buf, int bufsz)
{
    const ad_item_t *it = ad_store_get(idx);
    if(it == NULL || frame < 0 || frame >= it->frame_count) return false;
    snprintf(buf, (size_t)bufsz, "%s/media/%s/f_%03d.jpg", s_root, it->name, frame);
    return true;
}

/* ---------------- 底部滚动公告 ---------------- */

/* 文件的"版本"：mtime 秒 + 纳秒 + 大小，任一项变了就算新版本。
   rename() 覆盖文件时 mtime 一定变，所以这个判定足够。 */
static uint64_t file_key(const char *path)
{
    struct stat st;
    if(stat(path, &st) != 0) return 0;

    uint64_t k = (uint64_t)st.st_mtime * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
    return k * 1099511628211ull ^ (uint64_t)st.st_size;
}

void ad_store_refresh_ticker(void)
{
    if(!s_inited) ad_store_init(NULL);

    char path[AD_PATH_LEN];
    snprintf(path, sizeof(path), "%s/ticker.conf", s_root);

    uint64_t k = file_key(path);
    if(k == s_ticker_key && !s_ticker_dirty) return;

    s_ticker_dirty = false;
    s_ticker_key   = k;

    char line[512];
    if(k != 0 && read_first_line(path, line, sizeof(line))) {
        snprintf(s_ticker, sizeof(s_ticker), "%s", line);
    }
    else {
        snprintf(s_ticker, sizeof(s_ticker), "%s", TICKER_DEFAULT);
    }
    s_ticker_gen++;
}

const char *ad_store_ticker(void)
{
    if(!s_inited) ad_store_init(NULL);
    if(s_ticker[0] == '\0') ad_store_refresh_ticker();
    return s_ticker;
}

uint32_t ad_store_ticker_gen(void)
{
    return s_ticker_gen;
}
