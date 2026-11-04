#include "mqtt_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define MQTT_BUF 1024

/* ---------- 基础 I/O ---------- */

static int wait_readable(int fd, int timeout_ms)
{
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return select(fd + 1, &rf, NULL, NULL, &tv);
}

static int read_full(int fd, uint8_t *buf, size_t n, int timeout_ms)
{
    size_t got = 0;
    while (got < n) {
        int r = wait_readable(fd, timeout_ms);
        if (r <= 0) return -1;
        ssize_t k = recv(fd, buf + got, n - got, 0);
        if (k <= 0) return -1;
        got += (size_t)k;
    }
    return 0;
}

static int write_full(int fd, const uint8_t *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t k = send(fd, buf + sent, n - sent, MSG_NOSIGNAL);
        if (k <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)k;
    }
    return 0;
}

/* 剩余长度：MQTT 的变长整数编码 */
static int encode_len(uint8_t *p, int len)
{
    int i = 0;
    do {
        uint8_t d = (uint8_t)(len % 128);
        len /= 128;
        if (len > 0) d |= 0x80;
        p[i++] = d;
    } while (len > 0 && i < 4);
    return i;
}

static int decode_len(int fd, int timeout_ms)
{
    int value = 0, mult = 1;
    for (int i = 0; i < 4; i++) {
        uint8_t d;
        if (read_full(fd, &d, 1, timeout_ms) != 0) return -1;
        value += (d & 0x7F) * mult;
        if ((d & 0x80) == 0) return value;
        mult *= 128;
    }
    return -1;
}

/* ---------- 连接 ---------- */

int mqtt_open(mqtt_t *m, const char *host, int port,
              const char *client_id, const char *user, const char *pass,
              int keepalive_sec)
{
    if (m == NULL || host == NULL) return -1;
    memset(m, 0, sizeof(*m));
    m->fd = -1;
    m->next_id = 1;

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL) {
        fprintf(stderr, "[mqtt] 解析 %s 失败\n", host);
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        fprintf(stderr, "[mqtt] 连不上 %s:%d\n", host, port);
        return -1;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    m->fd = fd;

    /* ---- CONNECT 报文 ---- */
    uint8_t body[MQTT_BUF];
    int n = 0;
    const char *proto = "MQTT";
    body[n++] = 0x00; body[n++] = 4;
    memcpy(body + n, proto, 4); n += 4;
    body[n++] = 4;                                  /* 协议级别 3.1.1 */
    /* flags: username(0x80) | password(0x40) | clean session(0x02) */
    body[n++] = 0x80 | 0x40 | 0x02;
    body[n++] = (uint8_t)(keepalive_sec >> 8);
    body[n++] = (uint8_t)(keepalive_sec & 0xFF);

    size_t clen = strlen(client_id);
    body[n++] = (uint8_t)(clen >> 8); body[n++] = (uint8_t)(clen & 0xFF);
    memcpy(body + n, client_id, clen); n += (int)clen;

    size_t ulen = user ? strlen(user) : 0;
    if (ulen > 0) {
        body[n++] = (uint8_t)(ulen >> 8); body[n++] = (uint8_t)(ulen & 0xFF);
        memcpy(body + n, user, ulen); n += (int)ulen;
    }
    size_t plen = pass ? strlen(pass) : 0;
    if (plen > 0) {
        body[n++] = (uint8_t)(plen >> 8); body[n++] = (uint8_t)(plen & 0xFF);
        memcpy(body + n, pass, plen); n += (int)plen;
    }

    uint8_t pkt[MQTT_BUF + 8];
    int k = 0;
    pkt[k++] = 0x10;
    k += encode_len(pkt + k, n);
    memcpy(pkt + k, body, (size_t)n);
    k += n;
    if (write_full(fd, pkt, (size_t)k) != 0) { close(fd); m->fd = -1; return -1; }

    /* ---- CONNACK ---- */
    /* 10 秒：板子经主机中转时，中转那一侧要先和云端做 TLS 握手，
     * 偶尔会慢到几秒，5 秒会误判成"没收到 CONNACK"。 */
    uint8_t hdr[2];
    if (read_full(fd, hdr, 1, 10000) != 0 || hdr[0] != 0x20) {
        fprintf(stderr, "[mqtt] 没收到 CONNACK\n");
        close(fd); m->fd = -1; return -2;
    }
    int rem = decode_len(fd, 10000);
    if (rem < 2) { close(fd); m->fd = -1; return -2; }
    uint8_t ack[8];
    if (read_full(fd, ack, (size_t)rem, 10000) != 0) { close(fd); m->fd = -1; return -2; }
    if (ack[1] != 0) {
        fprintf(stderr, "[mqtt] 鉴权/连接被拒, CONNACK code=%d"
                        "（1=协议版本 2=clientId 非法 4=用户名或密码错 5=未授权）\n", ack[1]);
        close(fd); m->fd = -1; return -3;
    }
    return 0;
}

int mqtt_sub(mqtt_t *m, const char *topic)
{
    if (m == NULL || m->fd < 0) return -1;
    uint16_t id = m->next_id++;

    uint8_t body[MQTT_BUF];
    int n = 0;
    body[n++] = (uint8_t)(id >> 8); body[n++] = (uint8_t)(id & 0xFF);
    size_t tlen = strlen(topic);
    body[n++] = (uint8_t)(tlen >> 8); body[n++] = (uint8_t)(tlen & 0xFF);
    memcpy(body + n, topic, tlen); n += (int)tlen;
    body[n++] = 0;                                   /* 请求 QoS0 */

    uint8_t pkt[MQTT_BUF + 8];
    int k = 0;
    pkt[k++] = 0x82;
    k += encode_len(pkt + k, n);
    memcpy(pkt + k, body, (size_t)n);
    k += n;
    return write_full(m->fd, pkt, (size_t)k);
}

static void handle_incoming(mqtt_t *m, uint8_t hdr, const uint8_t *body, int len)
{
    uint8_t type = (uint8_t)(hdr >> 4);

    if (type == 3) {                                  /* PUBLISH */
        int qos = (hdr >> 1) & 0x03;
        int pos = 0;
        int tlen = (body[pos] << 8) | body[pos + 1];
        pos += 2;
        if (tlen <= 0 || pos + tlen > len) return;

        char topic[192];
        int copy = tlen < (int)sizeof(topic) - 1 ? tlen : (int)sizeof(topic) - 1;
        memcpy(topic, body + pos, (size_t)copy);
        topic[copy] = '\0';
        pos += tlen;

        uint16_t pid = 0;
        if (qos > 0 && pos + 2 <= len) {
            pid = (uint16_t)((body[pos] << 8) | body[pos + 1]);
            pos += 2;
        }

        if (m->on_msg && pos <= len)
            m->on_msg(topic, (const char *)body + pos, len - pos);

        if (qos == 1 && pid != 0) {                   /* 回 PUBACK，不然平台会重发 */
            uint8_t ack[4] = { 0x40, 0x02, (uint8_t)(pid >> 8), (uint8_t)(pid & 0xFF) };
            write_full(m->fd, ack, 4);
        }
    }
    /* PUBACK(4) / PINGRESP(13) / SUBACK(9) 这里不需要额外处理 */
}

int mqtt_drain(mqtt_t *m, int timeout_ms)
{
    if (m == NULL || m->fd < 0) return -1;
    int handled = 0;

    while (wait_readable(m->fd, timeout_ms) > 0) {
        uint8_t hdr;
        if (read_full(m->fd, &hdr, 1, 1000) != 0) return -1;
        int rem = decode_len(m->fd, 1000);
        if (rem < 0) return -1;

        uint8_t body[MQTT_BUF];
        if (rem > (int)sizeof(body)) {                /* 超长报文直接丢弃 */
            uint8_t tmp[256];
            int left = rem;
            while (left > 0) {
                int take = left > (int)sizeof(tmp) ? (int)sizeof(tmp) : left;
                if (read_full(m->fd, tmp, (size_t)take, 1000) != 0) return -1;
                left -= take;
            }
        } else if (rem > 0) {
            if (read_full(m->fd, body, (size_t)rem, 1000) != 0) return -1;
            handle_incoming(m, hdr, body, rem);
        }
        handled++;
        timeout_ms = 0;                               /* 后续只取已到达的，不等待 */
    }
    return handled;
}

int mqtt_pub(mqtt_t *m, const char *topic, const char *payload, int timeout_ms)
{
    if (m == NULL || m->fd < 0) return -1;
    uint16_t id = m->next_id++;

    uint8_t body[MQTT_BUF];
    int n = 0;
    size_t tlen = strlen(topic);
    body[n++] = (uint8_t)(tlen >> 8); body[n++] = (uint8_t)(tlen & 0xFF);
    memcpy(body + n, topic, tlen); n += (int)tlen;
    body[n++] = (uint8_t)(id >> 8); body[n++] = (uint8_t)(id & 0xFF);   /* QoS1 需要 packetId */

    size_t plen = payload ? strlen(payload) : 0;
    if (plen > 0) { memcpy(body + n, payload, plen); n += (int)plen; }

    uint8_t pkt[MQTT_BUF + 8];
    int k = 0;
    pkt[k++] = 0x32;                                  /* PUBLISH, QoS1 */
    k += encode_len(pkt + k, n);
    memcpy(pkt + k, body, (size_t)n);
    k += n;
    if (write_full(m->fd, pkt, (size_t)k) != 0) return -1;

    /* 等 PUBACK */
    int waited = 0;
    while (waited < timeout_ms) {
        if (wait_readable(m->fd, 200) <= 0) { waited += 200; continue; }
        uint8_t hdr;
        if (read_full(m->fd, &hdr, 1, 1000) != 0) return -1;
        int rem = decode_len(m->fd, 1000);
        if (rem < 0) return -1;
        uint8_t body2[MQTT_BUF];
        if (rem > (int)sizeof(body2)) return -1;
        if (rem > 0 && read_full(m->fd, body2, (size_t)rem, 1000) != 0) return -1;

        if ((hdr >> 4) == 4) return 0;                /* PUBACK：平台已收下 */
        handle_incoming(m, hdr, body2, rem);
        waited += 50;
    }
    fprintf(stderr, "[mqtt] 等 PUBACK 超时\n");
    return -1;
}

int mqtt_ping(mqtt_t *m)
{
    if (m == NULL || m->fd < 0) return -1;
    uint8_t pkt[2] = { 0xC0, 0x00 };
    return write_full(m->fd, pkt, 2);
}

void mqtt_close(mqtt_t *m)
{
    if (m == NULL) return;
    if (m->fd >= 0) {
        uint8_t pkt[2] = { 0xE0, 0x00 };              /* DISCONNECT */
        write_full(m->fd, pkt, 2);
        close(m->fd);
    }
    m->fd = -1;
}
