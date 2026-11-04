#if WEATHER_ENABLE_HTTP

#include "http_client.h"
#include <stdlib.h>
#include <string.h>
#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>

int https_get(const char *host, int port, const char *path, char **out, size_t *out_len)
{
    *out = NULL;
    if (out_len) *out_len = 0;

    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_algorithms();

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) return -1;
    /* 演示环境跳过对端证书校验；生产环境应改为 SSL_VERIFY_PEER 并加载根证书。 */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    BIO *bio = BIO_new_ssl_connect(ctx);
    if (bio == NULL) { SSL_CTX_free(ctx); return -1; }

    char conn[256];
    snprintf(conn, sizeof(conn), "%s:%d", host, port);
    BIO_set_conn_hostname(bio, conn);

    SSL *ssl = NULL;
    BIO_get_ssl(bio, &ssl);
    if (ssl == NULL) { BIO_free_all(bio); SSL_CTX_free(ctx); return -1; }
    SSL_set_tlsext_host_name(ssl, host);

    if (BIO_do_connect(bio) <= 0) { BIO_free_all(bio); SSL_CTX_free(ctx); return -1; }
    if (BIO_do_handshake(bio) <= 0) { BIO_free_all(bio); SSL_CTX_free(ctx); return -1; }

    char req[1024];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: weather-demo/1.0\r\n"
        "Accept: */*\r\nConnection: close\r\n\r\n",
        path, host);
    if (BIO_write(bio, req, n) <= 0) { BIO_free_all(bio); SSL_CTX_free(ctx); return -1; }
    BIO_flush(bio);

    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (buf == NULL) { BIO_free_all(bio); SSL_CTX_free(ctx); return -1; }
    char tmp[2048];
    int r;
    while ((r = BIO_read(bio, tmp, sizeof(tmp))) > 0) {
        if (len + (size_t)r + 1 > cap) {
            cap = (len + (size_t)r + 1) * 2;
            char *nb = (char *)realloc(buf, cap);
            if (nb == NULL) { free(buf); BIO_free_all(bio); SSL_CTX_free(ctx); return -1; }
            buf = nb;
        }
        memcpy(buf + len, tmp, (size_t)r);
        len += (size_t)r;
    }
    BIO_free_all(bio);
    SSL_CTX_free(ctx);

    /* 去掉 HTTP 响应头，仅保留 body（JSON）。 */
    char *body = buf;
    char *sep = strstr(buf, "\r\n\r\n");
    if (sep != NULL) {
        body = sep + 4;
        len -= (size_t)(body - buf);
    }
    char *outbuf = (char *)malloc(len + 1);
    if (outbuf == NULL) { free(buf); return -1; }
    memcpy(outbuf, body, len);
    outbuf[len] = '\0';
    free(buf);

    *out = outbuf;
    if (out_len) *out_len = len;
    return 0;
}

#endif /* WEATHER_ENABLE_HTTP */
