/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Monkey HTTP Server
 *  ==================
 *  Copyright 2001-2015 Monkey Software LLC <eduardo@monkey.io>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pthread.h>

#include <polarssl/version.h>
#include <polarssl/error.h>
#include <polarssl/net.h>
#include <polarssl/ssl.h>
#include <polarssl/bignum.h>
#include <polarssl/entropy.h>
#include <polarssl/ctr_drbg.h>
#include <polarssl/certs.h>
#include <polarssl/x509.h>
#include <polarssl/ssl_cache.h>
#include <polarssl/pk.h>

#include <monkey/mk_api.h>

#ifndef SENDFILE_BUF_SIZE
#define SENDFILE_BUF_SIZE SSL_MAX_CONTENT_LEN
#endif

#ifndef POLAR_DEBUG_LEVEL
#define POLAR_DEBUG_LEVEL 0
#endif

#if (POLARSSL_VERSION_NUMBER < 0x01030000)
#error "Require tls 1.3.10 or higher."
#endif

#if (!defined(POLARSSL_BIGNUM_C) || !defined(POLARSSL_ENTROPY_C) || \
        !defined(POLARSSL_SSL_TLS_C) || !defined(POLARSSL_SSL_SRV_C) || \
        !defined(POLARSSL_NET_C) || !defined(POLARSSL_RSA_C) || \
        !defined(POLARSSL_CTR_DRBG_C))
#error "One or more required POLARSSL modules not built."
#endif

struct polar_config {
    char *cert_file;
    char *cert_chain_file;
    char *key_file;
    char *dh_param_file;
};

#if defined(POLARSSL_SSL_CACHE_C)
struct polar_sessions {
    pthread_mutex_t _mutex;
    ssl_cache_context cache;
};

static struct polar_sessions global_sessions = {
    ._mutex = PTHREAD_MUTEX_INITIALIZER,
};

#endif

struct polar_context_head {
    ssl_context context;
    int fd;
    struct polar_context_head *_next;
};

struct polar_thread_context {

    struct polar_context_head *contexts;

    ctr_drbg_context ctr_drbg;
    pk_context pkey;

    struct mk_list _head;
};

struct polar_server_context {

    struct polar_config config;
    x509_crt cert;
    x509_crt ca_cert;
    pthread_mutex_t mutex;
    dhm_context dhm;
    entropy_context entropy;

    struct polar_thread_context threads;
};

struct polar_server_context *server_context;
static const char *my_dhm_P = POLARSSL_DHM_RFC5114_MODP_1024_P;
static const char *my_dhm_G = POLARSSL_DHM_RFC5114_MODP_1024_G;

static pthread_key_t local_context;

/*
 * The following function is taken from PolarSSL sources to get
 * the number of available bytes to read from a buffer.
 *
 * We copy this to make it inline and avoid extra context switches
 * on each read routine.
 */
static inline size_t polar_get_bytes_avail(const ssl_context *ssl)
{
    return (ssl->in_offt == NULL ? 0 : ssl->in_msglen);
}

static struct polar_thread_context *local_thread_context(void)
{
    return pthread_getspecific(local_context);
}

static int entropy_func_safe(void *data, unsigned char *output, size_t len)
{
    int ret;

    pthread_mutex_lock(&server_context->mutex);
    ret = entropy_func(data, output, len);
    pthread_mutex_unlock(&server_context->mutex);

    return ret;
}

#if (POLAR_DEBUG_LEVEL > 0)
static void polar_debug(void *ctx, int level, const char *str)
{
    (void)ctx;

    if (level < POLAR_DEBUG_LEVEL) {
        mk_warn("%.*s", (int)strlen(str) - 1, str);
    }
}
#endif

static int handle_return(int ret)
{
#if defined(TRACE)
    char err_buf[72];
    if (ret < 0) {
        error_strerror(ret, err_buf, sizeof(err_buf));
        PLUGIN_TRACE("[tls] SSL error: %s", err_buf);
    }
#endif
    if (ret < 0) {
        switch( ret )
        {
            case POLARSSL_ERR_NET_WANT_READ:
            case POLARSSL_ERR_NET_WANT_WRITE:
                if (errno != EAGAIN)
                    errno = EAGAIN;
		return -1;
            case POLARSSL_ERR_SSL_CONN_EOF:
                return 0;
            default:
                if (errno == EAGAIN)
		    errno = 0;
                return -1;
        }
    }
    else {
        return ret;
    }
}

static int config_parse(const char *confdir, struct polar_config *conf)
{
    long unsigned int len;
    char *conf_path = NULL;
    struct mk_rconf_section *section;
    struct mk_rconf *conf_head;
    struct mk_list *head;

    mk_api->str_build(&conf_path, &len, "%stls.conf", confdir);
    conf_head = mk_api->config_create(conf_path);
    mk_api->mem_free(conf_path);

    if (conf_head == NULL) {
        goto fallback;
    }

    mk_list_foreach(head, &conf_head->sections) {
        section = mk_list_entry(head, struct mk_rconf_section, _head);

        if (strcasecmp(section->name, "TLS")) {
            continue;
        }
        conf->cert_file = mk_api->config_section_get_key(section,
                "CertificateFile",
                MK_RCONF_STR);
        conf->cert_chain_file = mk_api->config_section_get_key(section,
                "CertificateChainFile",
                MK_RCONF_STR);
        conf->key_file = mk_api->config_section_get_key(section,
                "RSAKeyFile",
                MK_RCONF_STR);
        conf->dh_param_file = mk_api->config_section_get_key(section,
                "DHParameterFile",
                MK_RCONF_STR);
    }
    mk_api->config_free(conf_head);

fallback:
    if (conf->cert_file == NULL) {
        mk_api->str_build(&conf->cert_file, &len,
                          "%ssrv_cert.pem", confdir);
    }
    if (conf->key_file == NULL) {
        mk_api->str_build(&conf->key_file, &len,
                          "%srsa.pem", confdir);
    }
    if (conf->dh_param_file == NULL) {
        mk_api->str_build(&conf->dh_param_file, &len,
                          "%sdhparam.pem", confdir);
    }

    return 0;
}

static int polar_load_certs(const struct polar_config *conf)
{
    char err_buf[72];
    int ret = -1;

    ret = x509_crt_parse_file(&server_context->cert, conf->cert_file);
    if (ret < 0) {
        error_strerror(ret, err_buf, sizeof(err_buf));
        mk_warn("[tls] Load cert '%s' failed: %s",
               conf->cert_file,
               err_buf);

#if defined(POLARSSL_CERTS_C)
        mk_warn("[tls] Using test certificates, "
                "please set 'CertificateFile' in tls.conf");

        ret = x509_crt_parse(&server_context->cert,
                             (unsigned char *)test_srv_crt, strlen(test_srv_crt));

        if (ret) {
            error_strerror(ret, err_buf, sizeof(err_buf));
            mk_err("[tls] Load built-in cert failed: %s", err_buf);
            return -1;
        }

        return 0;
#else
        return -1;
#endif // defined(POLARSSL_CERTS_C)
    }
    else if (conf->cert_chain_file != NULL) {
        ret = x509_crt_parse_file(server_context->ca_cert.next,
                                  conf->cert_chain_file);

        if (ret) {
            error_strerror(ret, err_buf, sizeof(err_buf));
            mk_warn("[tls] Load cert chain '%s' failed: %s",
                    conf->cert_chain_file,
                    err_buf);
        }
    }

    return 0;
}

static int polar_load_key(struct polar_thread_context *thread_context,
                          const struct polar_config *conf)
{
    char err_buf[72];
    int ret;

    assert(conf->key_file);

    ret = pk_parse_keyfile(&thread_context->pkey, conf->key_file, NULL);
    if (ret < 0) {
        error_strerror(ret, err_buf, sizeof(err_buf));
        MK_TRACE("[tls] Load key '%s' failed: %s",
                conf->key_file,
                err_buf);

#if defined(POLARSSL_CERTS_C)

        ret = pk_parse_key(&thread_context->pkey,
                           (unsigned char *)test_srv_key,
                           strlen(test_srv_key), NULL, 0);
        if (ret) {
            error_strerror(ret, err_buf, sizeof(err_buf));
            mk_err("[tls] Failed to load built-in RSA key: %s", err_buf);
            return -1;
        }
#else
        return -1;
#endif // defined(POLARSSL_CERTS_C)
    }
    return 0;
}

static int polar_load_dh_param(const struct polar_config *conf)
{
    char err_buf[72];
    int ret;

    assert(conf->dh_param_file);

    ret = dhm_parse_dhmfile(&server_context->dhm, conf->dh_param_file);
    if (ret < 0) {
        error_strerror(ret, err_buf, sizeof(err_buf));

        ret = mpi_read_string(&server_context->dhm.P, 16, my_dhm_P);
        if (ret < 0) {
            error_strerror(ret, err_buf, sizeof(err_buf));
            mk_err("[tls] Load DH parameter failed: %s", err_buf);
            return -1;
        }
        ret = mpi_read_string(&server_context->dhm.G, 16, my_dhm_G);
        if (ret < 0) {
            error_strerror(ret, err_buf, sizeof(err_buf));
            mk_err("[tls] Load DH parameter failed: %s", err_buf);
            return -1;
        }
    }

    return 0;
}

static int polar_init(void)
{
    pthread_key_create(&local_context, NULL);

#if defined(POLARSSL_SSL_CACHE_C)
    ssl_cache_init(&global_sessions.cache);
#endif

    pthread_mutex_lock(&server_context->mutex);
    mk_list_init(&server_context->threads._head);
    entropy_init(&server_context->entropy);
    pthread_mutex_unlock(&server_context->mutex);

    PLUGIN_TRACE("[tls] Load certificates.");
    if (polar_load_certs(&server_context->config)) {
        return -1;
    }
    PLUGIN_TRACE("[tls] Load DH parameters.");
    if (polar_load_dh_param(&server_context->config)) {
        return -1;
    }

    return 0;
}

static int polar_thread_init(const struct polar_config *conf)
{
    struct polar_thread_context *thctx;
    int ret;

    PLUGIN_TRACE("[tls] Init thread context.");

    thctx = mk_api->mem_alloc(sizeof(*thctx));
    if (thctx == NULL) {
        return -1;
    }
    thctx->contexts = NULL;
    mk_list_init(&thctx->_head);

    pthread_mutex_lock(&server_context->mutex);
    mk_list_add(&thctx->_head, &server_context->threads._head);
    pthread_mutex_unlock(&server_context->mutex);

    ret = ctr_drbg_init(&thctx->ctr_drbg,
            entropy_func_safe, &server_context->entropy,
            NULL, 0);
    if (ret) {
        mk_err("crt_drbg_init failed: %d", ret);
        mk_api->mem_free(thctx);
        return -1;
    }

    pk_init(&thctx->pkey);

    PLUGIN_TRACE("[tls] Load RSA key.");
    if (polar_load_key(thctx, conf)) {
        return -1;
    }

    PLUGIN_TRACE("[tls] Set local thread context.");
    pthread_setspecific(local_context, thctx);

    return 0;
}

static void contexts_free(struct polar_context_head *ctx)
{
    struct polar_context_head *cur, *next;

    if (ctx != NULL) {
        cur  = ctx;
        next = cur->_next;

        for (; next; cur = next, next = next->_next) {
            ssl_free(&cur->context);
            memset(cur, 0, sizeof(*cur));
            mk_api->mem_free(cur);
        }

        ssl_free(&cur->context);
        memset(cur, 0, sizeof(*cur));
        mk_api->mem_free(cur);
    }
}

static void config_free(struct polar_config *conf)
{
    if (conf->cert_file) mk_api->mem_free(conf->cert_file);
    if (conf->cert_chain_file) mk_api->mem_free(conf->cert_chain_file);
    if (conf->key_file) mk_api->mem_free(conf->key_file);
    if (conf->dh_param_file) mk_api->mem_free(conf->dh_param_file);
}

static int polar_exit(void)
{
    struct mk_list *cur, *tmp;
    struct polar_thread_context *thctx;

    x509_crt_free(&server_context->cert);
    x509_crt_free(&server_context->ca_cert);
    dhm_free(&server_context->dhm);

    mk_list_foreach_safe(cur, tmp, &server_context->threads._head) {
        thctx = mk_list_entry(cur, struct polar_thread_context, _head);
        contexts_free(thctx->contexts);
        mk_api->mem_free(thctx);

        pk_free(&thctx->pkey);
    }
    pthread_mutex_destroy(&server_context->mutex);

#if defined(POLARSSL_SSL_CACHE_C)
    ssl_cache_free(&global_sessions.cache);
#endif

    config_free(&server_context->config);
    mk_api->mem_free(server_context);

    return 0;
}

/* Contexts may be requested from outside workers on exit so we should
 * be prepared for an empty context.
 */
static ssl_context *context_get(int fd)
{
    struct polar_thread_context *thctx = local_thread_context();
    struct polar_context_head **cur = &thctx->contexts;

    if (cur == NULL) {
        return NULL;
    }

    for (; *cur; cur = &(*cur)->_next) {
        if ((*cur)->fd == fd) {
            return &(*cur)->context;
        }
    }

    return NULL;
}

static int polar_cache_get(void *p, ssl_session *session)
{
    struct polar_sessions *session_cache;
    int ret;

    session_cache = p;
    pthread_mutex_lock(&session_cache->_mutex);
    ret = ssl_cache_get(&session_cache->cache, session);
    pthread_mutex_unlock(&session_cache->_mutex);

    return ret;
}

static int polar_cache_set(void *p, const ssl_session *session)
{
    struct polar_sessions *session_cache;
    int ret;

    session_cache = p;
    pthread_mutex_lock(&session_cache->_mutex);
    ret = ssl_cache_set(&session_cache->cache, session);
    pthread_mutex_unlock(&session_cache->_mutex);

    return ret;
}

static ssl_context *context_new(int fd)
{
    struct polar_thread_context *thctx = local_thread_context();
    struct polar_context_head **cur = &thctx->contexts;
    ssl_context *ssl = NULL;

    assert(cur != NULL);

    for (; *cur; cur = &(*cur)->_next) {
        if ((*cur)->fd == -1) {
            break;
        }
    }

    if (*cur == NULL) {
        PLUGIN_TRACE("[polarssl %d] New ssl context.", fd);

        *cur = mk_api->mem_alloc(sizeof(**cur));
        if (*cur == NULL) {
            return NULL;
        }
        (*cur)->_next = NULL;

        ssl = &(*cur)->context;

        ssl_init(ssl);
        ssl_set_endpoint(ssl, SSL_IS_SERVER);
        ssl_set_authmode(ssl, SSL_VERIFY_NONE);

        ssl_set_rng(ssl, ctr_drbg_random, &thctx->ctr_drbg);

#if (POLAR_DEBUG_LEVEL > 0)
        ssl_set_dbg(ssl, polar_debug, 0);
#endif

        ssl_set_own_cert(ssl, &server_context->cert, &thctx->pkey);
        ssl_set_session_tickets(ssl, SSL_SESSION_TICKETS_ENABLED);
        ssl_set_ca_chain(ssl, &server_context->ca_cert, NULL, NULL);
        ssl_set_dh_param_ctx(ssl, &server_context->dhm);

        ssl_set_session_cache(ssl, polar_cache_get, &global_sessions,
                              polar_cache_set, &global_sessions);

        ssl_set_bio(ssl, net_recv, &(*cur)->fd, net_send, &(*cur)->fd);
    }
    else {
        ssl = &(*cur)->context;
    }

    (*cur)->fd = fd;

    return ssl;
}

static int context_unset(int fd, ssl_context *ssl)
{
    struct polar_context_head *head;

    head = container_of(ssl, struct polar_context_head, context);

    if (head->fd == fd) {
        head->fd = -1;
        ssl_session_reset(ssl);
    }
    else {
        mk_err("[polarssl %d] Context already unset.", fd);
    }

    return 0;
}

int mk_tls_read(int fd, void *buf, int count)
{
    size_t avail;
    ssl_context *ssl = context_get(fd);

    if (!ssl) {
        ssl = context_new(fd);
    }

    int ret =  handle_return(ssl_read(ssl, buf, count));
    PLUGIN_TRACE("IN: %i SSL READ: %i ; CORE COUNT: %i",
                 ssl->in_msglen,
                 ret, count);

    /* Check if the caller read less than the available data */
    if (ret > 0) {
        avail = polar_get_bytes_avail(ssl);
        if (avail > 0) {
            /*
             * A read callback would never read in buffer more than
             * the size specified in 'count', but it aims to return
             * as value the total information read in the buffer plugin
             */
            ret += avail;
        }
    }
    return ret;
}

int mk_tls_write(int fd, const void *buf, size_t count)
{
    ssl_context *ssl = context_get(fd);
    if (!ssl) {
        ssl = context_new(fd);
    }

    return handle_return(ssl_write(ssl, buf, count));
}

int mk_tls_writev(int fd, struct mk_iov *mk_io)
{
    ssl_context *ssl = context_get(fd);
    const int iov_len = mk_io->iov_idx;
    const struct iovec *io = mk_io->io;
    const size_t len = mk_io->total_len;
    unsigned char *buf;
    size_t used = 0;
    int ret = 0, i;

    if (!ssl) {
        ssl = context_new(fd);
    }

    buf = mk_api->mem_alloc(len);
    if (buf == NULL) {
        mk_err("malloc failed: %s", strerror(errno));
        return -1;
    }

    for (i = 0; i < iov_len; i++) {
        memcpy(buf + used, io[i].iov_base, io[i].iov_len);
        used += io[i].iov_len;
    }

    assert(used == len);
    ret = ssl_write(ssl, buf, len);
    mk_api->mem_free(buf);

    return handle_return(ret);
}

int mk_tls_send_file(int fd, int file_fd, off_t *file_offset,
        size_t file_count)
{
    ssl_context *ssl = context_get(fd);
    unsigned char *buf;
    ssize_t used, remain = file_count, sent = 0;
    int ret;

    if (!ssl) {
        ssl = context_new(fd);
    }

    buf = mk_api->mem_alloc(SENDFILE_BUF_SIZE);
    if (buf == NULL) {
        return -1;
    }

    do {
        used = pread(file_fd, buf, SENDFILE_BUF_SIZE, *file_offset);
        if (used == 0) {
            ret = 0;
        }
        else if (used < 0) {
            mk_err("[tls] Read from file failed: %s", strerror(errno));
            ret = -1;
        }
        else if (remain > 0) {
            ret = ssl_write(ssl, buf, used < remain ? used : remain);
        }
        else {
            ret = ssl_write(ssl, buf, used);
        }

        if (ret > 0) {
            if (remain > 0) {
                remain -= ret;
            }
            sent += ret;
            *file_offset += ret;
        }
    } while (ret > 0);

    mk_api->mem_free(buf);

    if (sent > 0) {
        return sent;
    }
    else {
        return handle_return(ret);
    }
}

int mk_tls_close(int fd)
{
    ssl_context *ssl = context_get(fd);

    PLUGIN_TRACE("[fd %d] Closing connection", fd);

    if (ssl) {
        ssl_close_notify(ssl);
        context_unset(fd, ssl);
    }

    net_close(fd);

    return 0;
}

int mk_tls_plugin_init(struct plugin_api **api, char *confdir)
{
    int ret = 0;

    /* Evil global config stuff */
    mk_api = *api;

    server_context = mk_api->mem_alloc_z(sizeof(struct polar_server_context));
    if (config_parse(confdir, &server_context->config)) {
        ret = -1;
    }
    polar_init();

    return ret;
}

void mk_tls_worker_init(void)
{
    if (polar_thread_init(&server_context->config)) {
        abort();
    }
}

int mk_tls_plugin_exit()
{
    return polar_exit();
}


/* Network Layer plugin Callbacks */
struct mk_plugin_network mk_plugin_network_tls = {
    .read          = mk_tls_read,
    .write         = mk_tls_write,
    .writev        = mk_tls_writev,
    .close         = mk_tls_close,
    .send_file     = mk_tls_send_file,
    .buffer_size   = SSL_MAX_CONTENT_LEN
};

struct mk_plugin mk_plugin_tls = {
    /* Identification */
    .shortname     = "tls",
    .name          = "mbedTLS",
    .version       = MK_VERSION_STR,
    .hooks         = MK_PLUGIN_NETWORK_LAYER,

    /* Init / Exit */
    .init_plugin   = mk_tls_plugin_init,
    .exit_plugin   = mk_tls_plugin_exit,

    /* Init Levels */
    .master_init   = NULL,
    .worker_init   = mk_tls_worker_init,

    /* Type */
    .network       = &mk_plugin_network_tls,
    .capabilities  = MK_CAP_SOCK_SSL
};
