/**
 * @file server.c
 * @brief LADP 板端接收服务（Lvgl Ad Daemon Protocol）
 *
 * 目标平台：GEC6818（Linux 3.4.39 / armel / glibc 2.23），只用到 POSIX，
 * 在 WSL 里也能直接跑，方便先在本机自测再上板。
 *
 * ── 为什么不用 FTP/SFTP ──────────────────────────────────────────
 * 板上是 Buildroot 的 dropbear + 一个静态 sftp-server，能传但有两个麻烦：
 *   1) 每个文件一次认证会话，几十帧小文件的握手开销比数据本身还大；
 *   2) 传完还得有人去改清单，客户端要额外知道板上的目录状态。
 * 所以自己定一个"一条连接、顺序多条命令"的极简协议。
 *
 * ── 协议 ────────────────────────────────────────────────────────
 * 全文本行（\n 结尾，UTF-8），PUT 后面跟二进制体：
 *
 *   客户端 → 服务端                           服务端 → 客户端
 *   HELLO                                   OK LADP/1 root=...
 *   PUT <字节数> <crc32> <相对路径>\n<body>  OK 写入 <n> 字节 -> <path>
 *   MKDIR <相对路径>                         OK
 *   RM <相对路径>                            OK 已删除 <path>
 *   LIST                                    OK <条数>\n<name>\t<清单行>\n...
 *   PING                                    OK PONG
 *   BYE                                     OK BYE，然后关闭
 *
 * 出错统一回 ERR <原因>\n，连接不关，客户端可以继续发下一条。
 *
 * ── 写入的原子性 ────────────────────────────────────────────────
 * 数据先进 <root>/.tmp/ 的临时文件，CRC32 校验通过才 rename() 到目标路径。
 * rename 在同一文件系统内是原子的，所以板上永远看不到"写了一半的封面"，
 * LVGL 也不会因为读到半截 JPEG 而花屏。
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define LADP_PORT_DEF   9500
/* 素材根目录：启动时若当前目录下有 advert/ 就用它（板上的标准布局是
 * /root/lvgl-demo/{demo,ladpd,advert}），否则退到 /root/lvgl-demo/advert。
 * 以前写死 /root/lvgl-weather/advert，广告机一换目录，ladpd 就在旧地方
 * 自说自话建一套空目录。 */
#define LADP_ROOT_DEF   "/root/lvgl-demo/advert"
#define LADP_MAX_FILE   (64u * 1024u * 1024u)   /* 单文件上限 64MB */
#define LADP_LINE_MAX   1024
#define LADP_BUF_SIZE   8192

/* ============ CRC32（IEEE 802.3，与 Python zlib.crc32 完全一致） ============ */

static uint32_t g_crc_table[256];
static bool     g_crc_ready = false;

static void crc32_init(void)
{
    for(uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for(int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_crc_table[i] = c;
    }
    g_crc_ready = true;
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    if(!g_crc_ready) crc32_init();

    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for(size_t i = 0; i < len; i++) {
        crc = g_crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

/* ====================== 连接缓冲 ====================== */

typedef struct {
    int     fd;
    uint8_t buf[LADP_BUF_SIZE];
    size_t  len;    /* 缓冲区里现有字节数 */
    size_t  pos;    /* 已消费位置 */
} conn_t;

/* 再灌一批数据进缓冲；>0 读到的字节数，0 对端关闭，-1 出错 */
static ssize_t conn_fill(conn_t *c)
{
    if(c->pos > 0) {
        memmove(c->buf, c->buf + c->pos, c->len - c->pos);
        c->len -= c->pos;
        c->pos  = 0;
    }

    ssize_t n = recv(c->fd, c->buf + c->len, sizeof(c->buf) - c->len, 0);
    if(n > 0) c->len += (size_t)n;
    return n;
}

/* 读一行（不含 \n）；false = 连接断了或行超长 */
static bool conn_readline(conn_t *c, char *out, size_t cap)
{
    size_t n = 0;

    for(;;) {
        if(c->pos >= c->len) {
            if(conn_fill(c) <= 0) return false;
        }

        while(c->pos < c->len) {
            uint8_t ch = c->buf[c->pos++];
            if(ch == '\n') {
                out[n] = '\0';
                if(n > 0 && out[n - 1] == '\r') out[n - 1] = '\0';
                return true;
            }
            if(n + 1 >= cap) return false;
            out[n++] = (char)ch;
        }
    }
}

/* 精确读 n 字节；fd_out >= 0 时同时落盘，crc 累积校验 */
static bool conn_read_exact(conn_t *c, int fd_out, size_t n, uint32_t *crc)
{
    while(n > 0) {
        if(c->pos >= c->len) {
            if(conn_fill(c) <= 0) return false;
        }

        size_t avail = c->len - c->pos;
        size_t take  = (avail < n) ? avail : n;

        if(crc != NULL) *crc = crc32_update(*crc, c->buf + c->pos, take);

        if(fd_out >= 0) {
            size_t done = 0;
            while(done < take) {
                ssize_t w = write(fd_out, c->buf + c->pos + done, take - done);
                if(w <= 0) {
                    if(errno == EINTR) continue;
                    return false;
                }
                done += (size_t)w;
            }
        }

        c->pos += take;
        n      -= take;
    }
    return true;
}

/* ====================== 回复 ====================== */

static void reply(conn_t *c, const char *fmt, ...)
{
    char   line[LADP_LINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);

    size_t l = strlen(line);
    line[l++] = '\n';
    line[l]   = '\0';

    size_t done = 0;
    while(done < l) {
        ssize_t w = send(c->fd, line + done, l - done, MSG_NOSIGNAL);
        if(w <= 0) {
            if(errno == EINTR) continue;
            return;
        }
        done += (size_t)w;
    }
}

/* ====================== 路径与目录 ====================== */

/* 只接受相对路径且不含 ".."，防止客户端把文件写到 root 之外 */
static bool safe_rel(const char *p)
{
    if(p == NULL || p[0] == '\0') return false;
    if(p[0] == '/') return false;
    if(strstr(p, "..") != NULL) return false;
    if(strlen(p) >= 200) return false;

    for(const char *s = p; *s; s++) {
        if(*s == '\\') return false;
    }
    return true;
}

/* 逐级创建目录（含中间层），已存在不算错 */
static bool mkdir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);

    size_t l = strlen(tmp);
    if(l == 0) return false;
    if(tmp[l - 1] == '/') tmp[l - 1] = '\0';

    for(char *p = tmp + 1; *p; p++) {
        if(*p != '/') continue;
        *p = '\0';
        if(mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    if(mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

/* 创建文件所在的所有父目录 */
static bool mkdir_parent(const char *file)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", file);

    char *slash = strrchr(tmp, '/');
    if(slash == NULL) return true;
    *slash = '\0';
    return mkdir_p(tmp);
}

static bool rm_recursive(const char *path, int depth)
{
    if(depth > 12) return false;

    struct stat st;
    if(lstat(path, &st) != 0) return false;
    if(!S_ISDIR(st.st_mode)) return unlink(path) == 0;

    DIR *d = opendir(path);
    if(d == NULL) return rmdir(path) == 0;

    struct dirent *e;
    while((e = readdir(d)) != NULL) {
        if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;

        char sub[512];
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        rm_recursive(sub, depth + 1);
    }
    closedir(d);
    return rmdir(path) == 0;
}

/* ====================== 命令处理 ====================== */

static const char *g_root = LADP_ROOT_DEF;
static const char *g_tmp  = NULL;

/* PUT <size> <crc32hex> <relpath> */
static void cmd_put(conn_t *c, char *args)
{
    char *save = NULL;
    char *sz_s = strtok_r(args, " ", &save);
    char *cr_s = strtok_r(NULL, " ", &save);
    char *path = strtok_r(NULL, " ", &save);

    if(sz_s == NULL || cr_s == NULL || path == NULL) {
        reply(c, "ERR 参数不足，用法 PUT <size> <crc32> <path>");
        return;
    }
    if(!safe_rel(path)) { reply(c, "ERR 非法路径"); return; }

    unsigned long long size = strtoull(sz_s, NULL, 10);
    if(size == 0 || size > LADP_MAX_FILE) { reply(c, "ERR 文件大小越界"); return; }

    uint32_t want_crc = (uint32_t)strtoul(cr_s, NULL, 16);

    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s/%s", g_root, path);

    if(!mkdir_parent(final_path)) { reply(c, "ERR 无法创建目录"); return; }

    char tmp_path[512];
    const char *base = strrchr(path, '/');
    base = (base != NULL) ? base + 1 : path;
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s.part", g_tmp, base);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd < 0) { reply(c, "ERR 无法写临时文件"); return; }

    uint32_t crc = 0;
    bool ok = conn_read_exact(c, fd, (size_t)size, &crc);
    close(fd);

    if(!ok) {
        unlink(tmp_path);
        reply(c, "ERR 数据传输中断");
        return;
    }

    if(crc != want_crc) {
        unlink(tmp_path);
        reply(c, "ERR CRC 校验失败 期望 %08x 实际 %08x", want_crc, crc);
        return;
    }

    if(rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        reply(c, "ERR 重命名失败 %s", strerror(errno));
        return;
    }

    reply(c, "OK 写入 %llu 字节 -> %s", size, path);
}

static void cmd_mkdir(conn_t *c, char *args)
{
    if(!safe_rel(args)) { reply(c, "ERR 非法路径"); return; }

    char p[512];
    snprintf(p, sizeof(p), "%s/%s", g_root, args);

    if(!mkdir_p(p)) { reply(c, "ERR 建目录失败"); return; }
    reply(c, "OK");
}

static void cmd_rm(conn_t *c, char *args)
{
    if(!safe_rel(args)) { reply(c, "ERR 非法路径"); return; }

    char p[512];
    snprintf(p, sizeof(p), "%s/%s", g_root, args);

    if(!rm_recursive(p, 0)) { reply(c, "ERR 删除失败"); return; }
    reply(c, "OK 已删除 %s", args);
}

/**
 * TIME <unix 时间戳>
 *
 * 6818 没有 RTC 电池，断电后系统时间从 1970/2015 重新走，界面上的钟完全是错的，
 * 而板上又没有外网可以走 NTP。最靠谱的授时源就是"客户端所在的那台电脑"：
 * 客户端每次连上来顺手发一条 TIME，把现在的绝对时间写进内核。
 *
 * 时间戳是 UTC 秒，时区由显示端处理（demo 启动时设 TZ=CST-8）。
 */
static void cmd_time(conn_t *c, char *args)
{
    long long ts = strtoll(args, NULL, 10);

    /* 2020-01-01 ~ 2100 年，超出范围基本就是客户端算错了 */
    if(ts < 1577836800LL || ts > 4102444800LL) {
        reply(c, "ERR 时间戳不合理");
        return;
    }

    struct timeval tv;
    tv.tv_sec  = (time_t)ts;
    tv.tv_usec = 0;

    if(settimeofday(&tv, NULL) != 0) {
        reply(c, "ERR 设置系统时间失败 %s", strerror(errno));
        return;
    }

    reply(c, "OK 系统时间已校准");
}

static void cmd_list(conn_t *c)
{
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/meta", g_root);

    /* 先数一遍，好让客户端按条数读 */
    int  count = 0;
    DIR *d = opendir(dir_path);

    if(d != NULL) {
        struct dirent *e;
        while((e = readdir(d)) != NULL) {
            size_t l = strlen(e->d_name);
            if(l > 5 && strcmp(e->d_name + l - 5, ".conf") == 0) count++;
        }
        rewinddir(d);
    }

    reply(c, "OK %d", count);
    if(d == NULL) return;

    struct dirent *e;
    while((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if(l <= 5 || strcmp(e->d_name + l - 5, ".conf") != 0) continue;

        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir_path, e->d_name);

        char  line[512] = "";
        FILE *fp = fopen(full, "r");
        if(fp != NULL) {
            if(fgets(line, sizeof(line), fp) == NULL) line[0] = '\0';
            fclose(fp);
            char *nl = strpbrk(line, "\r\n");
            if(nl != NULL) *nl = '\0';
        }

        char out[700];
        snprintf(out, sizeof(out), "%.*s\t%s\n", (int)(l - 5), e->d_name, line);
        send(c->fd, out, strlen(out), MSG_NOSIGNAL);
    }
    closedir(d);
}

/* 处理一条命令行；false = 断开连接 */
static bool handle_line(conn_t *c, char *line)
{
    char *sp   = strchr(line, ' ');
    char *args = NULL;

    if(sp != NULL) { *sp = '\0'; args = sp + 1; }
    else           { args = line + strlen(line); }

    if(strcmp(line, "HELLO") == 0) { reply(c, "OK LADP/1 root=%s", g_root); return true; }
    if(strcmp(line, "PING") == 0)  { reply(c, "OK PONG"); return true; }

    if(strcmp(line, "PUT") == 0)   { cmd_put(c, args);   return true; }
    if(strcmp(line, "MKDIR") == 0) { cmd_mkdir(c, args); return true; }
    if(strcmp(line, "RM") == 0)    { cmd_rm(c, args);    return true; }
    if(strcmp(line, "LIST") == 0)  { cmd_list(c);        return true; }
    if(strcmp(line, "TIME") == 0)  { cmd_time(c, args);  return true; }

    if(strcmp(line, "BYE") == 0) { reply(c, "OK BYE"); return false; }

    reply(c, "ERR 未知命令 %s", line);
    return true;
}

/* ====================== 主循环 ====================== */

static void serve_client(int fd, const char *peer)
{
    conn_t c;
    memset(&c, 0, sizeof(c));
    c.fd = fd;

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    printf("[ladpd] 客户端接入: %s\n", peer);
    fflush(stdout);

    char line[LADP_LINE_MAX];
    while(conn_readline(&c, line, sizeof(line))) {
        if(line[0] == '\0') continue;
        if(!handle_line(&c, line)) break;
    }

    printf("[ladpd] 客户端断开: %s\n", peer);
    fflush(stdout);
}

int ladp_server_main(int argc, char **argv);

int ladp_server_main(int argc, char **argv)
{
    int         port   = LADP_PORT_DEF;
    bool        daemon = false;
    const char *root   = NULL;          /* NULL = 自动判断，见下 */

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "-p") == 0 && i + 1 < argc)      port = atoi(argv[++i]);
        else if(strcmp(argv[i], "-r") == 0 && i + 1 < argc) root = argv[++i];
        else if(strcmp(argv[i], "-d") == 0)                 daemon = true;
        else if(strcmp(argv[i], "-h") == 0) {
            printf("用法: %s [-p 端口] [-r 根目录] [-d 后台运行]\n", argv[0]);
            return 0;
        }
    }

    /* 没显式指定 -r：优先用当前目录下的 advert/（和可执行文件同级的标准布局），
     * 这样把整个目录搬走也不会跑到老地方建空目录。 */
    if(root == NULL) {
        struct stat st;
        root = (stat("./advert", &st) == 0 && S_ISDIR(st.st_mode)) ? "./advert"
                                                                  : LADP_ROOT_DEF;
    }

    /* 落成绝对路径：日志和 HELLO 应答里都写 "./advert" 太含糊，
     * 客户端拿到的 root= 也应该是个明确的地方。 */
    {
        static char abs_root[PATH_MAX];
        if(realpath(root, abs_root) != NULL) root = abs_root;
    }

    g_root = root;

    char meta_dir[512], media_dir[512], tmp_dir[512];
    snprintf(meta_dir,  sizeof(meta_dir),  "%s/meta",  root);
    snprintf(media_dir, sizeof(media_dir), "%s/media", root);
    snprintf(tmp_dir,   sizeof(tmp_dir),   "%s/.tmp",  root);

    if(!mkdir_p(meta_dir) || !mkdir_p(media_dir) || !mkdir_p(tmp_dir)) {
        fprintf(stderr, "[ladpd] 无法准备目录 %s: %s\n", root, strerror(errno));
        return 1;
    }
    g_tmp = tmp_dir;

    if(daemon) {
        pid_t pid = fork();
        if(pid < 0) { perror("fork"); return 1; }
        if(pid > 0) return 0;       /* 父进程退出 */
        setsid();
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if(lfd < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(lfd);
        return 1;
    }
    if(listen(lfd, 4) != 0) {
        perror("listen");
        close(lfd);
        return 1;
    }

    printf("[ladpd] LADP/1 已启动\n");
    printf("[ladpd]   监听端口  : %d\n", port);
    printf("[ladpd]   素材根目录: %s\n", root);
    printf("[ladpd]   临时目录  : %s\n", tmp_dir);
    fflush(stdout);

    for(;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);

        int cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
        if(cfd < 0) {
            if(errno == EINTR) continue;
            perror("accept");
            break;
        }

        char ip[64];
        snprintf(ip, sizeof(ip), "%s:%d", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
        serve_client(cfd, ip);
        close(cfd);
    }

    close(lfd);
    return 0;
}

#ifdef LADP_STANDALONE
int main(int argc, char **argv)
{
    return ladp_server_main(argc, argv);
}
#endif
