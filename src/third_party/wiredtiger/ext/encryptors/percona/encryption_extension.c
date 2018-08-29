/*======
This file is part of Percona Server for MongoDB.

Copyright (c) 2006, 2018, Percona and/or its affiliates. All rights reserved.

    Percona Server for MongoDB is free software: you can redistribute
    it and/or modify it under the terms of the GNU Affero General
    Public License, version 3, as published by the Free Software
    Foundation.

    Percona Server for MongoDB is distributed in the hope that it will
    be useful, but WITHOUT ANY WARRANTY; without even the implied
    warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
    See the GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public
    License along with Percona Server for MongoDB.  If not, see
    <http://www.gnu.org/licenses/>.
======= */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
//#include <assert>

#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/err.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

#include "encryption_keydb_c_api.h"

#define KEY_LEN 32
#define GCM_TAG_LEN 16

typedef struct {
    // WT_ENCRYPTOR must be the first field
    WT_ENCRYPTOR encryptor;
    WT_EXTENSION_API *wt_api;
    const EVP_CIPHER *cipher;
    int iv_len;
    unsigned char key[KEY_LEN];
} PERCONA_ENCRYPTOR;


static const bool printDebugMessages = false;
#define DBG if (printDebugMessages)
#define DBG_MSG(...) DBG pe->wt_api->msg_printf(pe->wt_api, session, __VA_ARGS__)

static int report_error(
    PERCONA_ENCRYPTOR *pe, WT_SESSION *session, int err, const char *msg)
{
    WT_EXTENSION_API *wt_api;

    wt_api = pe->wt_api;
    (void)wt_api->err_printf(wt_api, session,
                             "encryption: %s: %s", msg, wt_api->strerror(wt_api, NULL, err));
    return err;
}

static int handleErrors(PERCONA_ENCRYPTOR *pe, WT_SESSION *session) {
    WT_EXTENSION_API *wt_api = pe->wt_api;
    int ret = 0;
    int e;
    while ((e = ERR_get_error())) {
        wt_api->err_printf(wt_api, session,
                           "libcrypto error: %d", e);
        ret = e;
    }
    return ret;
}

static char value_type_char(int type)
{
    switch (type) {
        case WT_CONFIG_ITEM_STRING:
            return 's';
        case WT_CONFIG_ITEM_BOOL:
            return 'b';
        case WT_CONFIG_ITEM_ID:
            return '#';
        case WT_CONFIG_ITEM_NUM:
            return 'n';
        case WT_CONFIG_ITEM_STRUCT:
            return 'z';
    }
    return 'x';
}

static int dump_config_arg(PERCONA_ENCRYPTOR *pe, WT_SESSION *session, WT_CONFIG_ARG *config) {
    WT_EXTENSION_API *wt_api = pe->wt_api;
    WT_CONFIG_PARSER *parser = NULL;
    int ret = wt_api->config_parser_open_arg(wt_api, session, config, &parser);
    if (ret != 0)
        return ret;
    WT_CONFIG_ITEM k, v;
    while ((ret = parser->next(parser, &k, &v)) == 0) {
        DBG_MSG("%c%c:%.*s:%.*s", value_type_char(k.type), value_type_char(v.type), (int)k.len, k.str, (int)v.len, v.str);
    }
    parser->close(parser);
    return 0;
}

static int parse_customization_config(PERCONA_ENCRYPTOR *pe, WT_SESSION *session, WT_CONFIG_ARG *config) {
    WT_EXTENSION_API *wt_api = pe->wt_api;
    WT_CONFIG_PARSER *parser = NULL;
    int ret = wt_api->config_parser_open_arg(wt_api, session, config, &parser);
    if (ret != 0)
        return ret;
    WT_CONFIG_ITEM k, v;
    while (parser->next(parser, &k, &v) == 0) {
        if (!strncmp("keyid", k.str, (int)k.len)) {
            if (0 != get_key_by_id(v.str, v.len, pe->key)) {
                ret = report_error(pe, session, EINVAL, "cannot get key by keyid");
                break;
            }
        }
    }
    parser->close(parser);
    return ret;
}

static void store_IV(PERCONA_ENCRYPTOR *pe, uint8_t *dst) {
    uint8_t buf[pe->iv_len];
    store_pseudo_bytes(buf, pe->iv_len);
    //TODO: encrypt pseudo bytes same way as PS does
    memcpy(dst, buf, pe->iv_len);
}

static int percona_encrypt_cbc(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
    int ret = 0;
    int encrypted_len = 0;
    PERCONA_ENCRYPTOR *pe = (PERCONA_ENCRYPTOR*)encryptor;
    DBG_MSG("entering encrypt %lu %lu", src_len, dst_len);
    if (dst_len < pe->iv_len + src_len + EVP_CIPHER_block_size(pe->cipher))
        return (report_error(pe, session,
                ENOMEM, "encrypt buffer not big enough"));

    *result_lenp = 0;
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    EVP_CIPHER_CTX ctx_value;
    EVP_CIPHER_CTX *ctx= &ctx_value;
    EVP_CIPHER_CTX_init(ctx);
#else
    EVP_CIPHER_CTX *ctx= EVP_CIPHER_CTX_new();
    if (unlikely(!ctx))
        goto err;
#endif

    store_IV(pe, dst);
    *result_lenp += pe->iv_len;

    if(1 != EVP_EncryptInit_ex(ctx, pe->cipher, NULL, pe->key, dst))
        goto err;

    if(1 != EVP_EncryptUpdate(ctx, dst + *result_lenp, &encrypted_len, src, src_len))
        goto err;
    *result_lenp += encrypted_len;

    if(1 != EVP_EncryptFinal_ex(ctx, dst + *result_lenp, &encrypted_len))
        goto err;
    *result_lenp += encrypted_len;

    ret = 0;
    goto cleanup;

err:
    ret = handleErrors(pe, session);

cleanup:
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    EVP_CIPHER_CTX_cleanup(ctx);
#else
    EVP_CIPHER_CTX_free(ctx);
#endif
    DBG_MSG("exiting encrypt %lu", *result_lenp);
    return ret;
}

static int percona_encrypt_gcm(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
    int ret = 0;
    int encrypted_len = 0;
    PERCONA_ENCRYPTOR *pe = (PERCONA_ENCRYPTOR*)encryptor;
    DBG_MSG("entering encrypt %lu %lu", src_len, dst_len);
    if (dst_len < pe->iv_len + src_len + GCM_TAG_LEN)
        return (report_error(pe, session,
                ENOMEM, "encrypt buffer not big enough"));

    *result_lenp = 0;
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    EVP_CIPHER_CTX ctx_value;
    EVP_CIPHER_CTX *ctx= &ctx_value;
    EVP_CIPHER_CTX_init(ctx);
#else
    EVP_CIPHER_CTX *ctx= EVP_CIPHER_CTX_new();
    if (unlikely(!ctx))
        goto err;
#endif

    if (0 != get_iv_gcm(dst, pe->iv_len)) {
        ret = report_error(pe, session, EINVAL, "failed generating IV for GCM");
        goto cleanup;
    }
    *result_lenp += pe->iv_len;

    if(1 != EVP_EncryptInit_ex(ctx, pe->cipher, NULL, pe->key, dst))
        goto err;

    // we don't provide any AAD data yet

    if(1 != EVP_EncryptUpdate(ctx, dst + *result_lenp, &encrypted_len, src, src_len))
        goto err;
    *result_lenp += encrypted_len;

    if(1 != EVP_EncryptFinal_ex(ctx, dst + *result_lenp, &encrypted_len))
        goto err;
    *result_lenp += encrypted_len;

    // get the tag
    if(1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, GCM_TAG_LEN, dst + *result_lenp))
        goto err;
    *result_lenp += GCM_TAG_LEN;

    ret = 0;
    goto cleanup;

err:
    ret = handleErrors(pe, session);

cleanup:
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    EVP_CIPHER_CTX_cleanup(ctx);
#else
    EVP_CIPHER_CTX_free(ctx);
#endif
    DBG_MSG("exiting encrypt %lu", *result_lenp);
    return ret;
}

static int percona_decrypt_cbc(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
    int ret = 0;
    int decrypted_len = 0;
    PERCONA_ENCRYPTOR *pe = (PERCONA_ENCRYPTOR*)encryptor;
    DBG_MSG("entering decrypt %lu %lu", src_len, dst_len);

    *result_lenp = 0;
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    EVP_CIPHER_CTX ctx_value;
    EVP_CIPHER_CTX *ctx= &ctx_value;
    EVP_CIPHER_CTX_init(ctx);
#else
    EVP_CIPHER_CTX *ctx= EVP_CIPHER_CTX_new();
    if (unlikely(!ctx))
        goto err;
#endif

    if(1 != EVP_DecryptInit_ex(ctx, pe->cipher, NULL, pe->key, src))
        goto err;
    src += pe->iv_len;
    src_len -= pe->iv_len;

    if(1 != EVP_DecryptUpdate(ctx, dst, &decrypted_len, src, src_len))
        goto err;
    *result_lenp += decrypted_len;
    dst += decrypted_len;

    if(1 != EVP_DecryptFinal_ex(ctx, dst, &decrypted_len))
        goto err;
    *result_lenp += decrypted_len;

    ret = 0;
    goto cleanup;

err:
    ret = handleErrors(pe, session);

cleanup:
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    EVP_CIPHER_CTX_cleanup(ctx);
#else
    EVP_CIPHER_CTX_free(ctx);
#endif
    DBG_MSG("exiting decrypt %lu", *result_lenp);
    return ret;
}

static int percona_decrypt_gcm(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
    int ret = 0;
    int decrypted_len = 0;
    PERCONA_ENCRYPTOR *pe = (PERCONA_ENCRYPTOR*)encryptor;
    DBG_MSG("entering decrypt %lu %lu", src_len, dst_len);

    *result_lenp = 0;
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    EVP_CIPHER_CTX ctx_value;
    EVP_CIPHER_CTX *ctx= &ctx_value;
    EVP_CIPHER_CTX_init(ctx);
#else
    EVP_CIPHER_CTX *ctx= EVP_CIPHER_CTX_new();
    if (unlikely(!ctx))
        goto err;
#endif

    if(1 != EVP_DecryptInit_ex(ctx, pe->cipher, NULL, pe->key, src))
        goto err;
    src += pe->iv_len;
    src_len -= pe->iv_len;

    // we have no AAD yet

    if(1 != EVP_DecryptUpdate(ctx, dst, &decrypted_len, src, src_len - GCM_TAG_LEN))
        goto err;
    *result_lenp += decrypted_len;
    dst += decrypted_len;
    src += src_len - GCM_TAG_LEN;
    src_len = GCM_TAG_LEN;

    // Set expected tag value. Works in OpenSSL 1.0.1d and later
    if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, GCM_TAG_LEN, src))
        goto err;

    if(1 != EVP_DecryptFinal_ex(ctx, dst, &decrypted_len))
        goto err;
    *result_lenp += decrypted_len;

    ret = 0;
    goto cleanup;

err:
    ret = handleErrors(pe, session);

cleanup:
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    EVP_CIPHER_CTX_cleanup(ctx);
#else
    EVP_CIPHER_CTX_free(ctx);
#endif
    DBG_MSG("exiting decrypt %lu", *result_lenp);
    return ret;
}

static int percona_sizing_cbc(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    size_t *expansion_constantp)
{
    PERCONA_ENCRYPTOR *pe = (PERCONA_ENCRYPTOR*)encryptor;
    DBG_MSG("entering sizing");
    (void)session;              /* Unused parameters */

    //*expansion_constantp = CHKSUM_LEN + pe->iv_len;
    *expansion_constantp = pe->iv_len + EVP_CIPHER_block_size(pe->cipher);
    return 0;
}

static int percona_sizing_gcm(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    size_t *expansion_constantp)
{
    PERCONA_ENCRYPTOR *pe = (PERCONA_ENCRYPTOR*)encryptor;
    DBG_MSG("entering sizing");
    (void)session;              /* Unused parameters */

    *expansion_constantp = pe->iv_len + GCM_TAG_LEN;
    return 0;
}

static int percona_customize(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    WT_CONFIG_ARG *encrypt_config, WT_ENCRYPTOR **customp)
{
    PERCONA_ENCRYPTOR *pe = (PERCONA_ENCRYPTOR*)encryptor;
    DBG_MSG("entering customize");
    DBG dump_config_arg(pe, session, encrypt_config);
    PERCONA_ENCRYPTOR *cpe;
    if ((cpe = calloc(1, sizeof(PERCONA_ENCRYPTOR))) == NULL)
            return errno;
    *cpe = *pe;
    // new instance passed to parse_customization_config because it should fill encryption key field
    int ret = parse_customization_config(cpe, session, encrypt_config);
    if (ret != 0) {
        free(cpe);
        return ret;
    }
    *customp = (WT_ENCRYPTOR*)cpe;
    return 0;
}

static int percona_terminate(WT_ENCRYPTOR *encryptor, WT_SESSION *session)
{
    PERCONA_ENCRYPTOR *pe = (PERCONA_ENCRYPTOR*)encryptor;
    DBG_MSG("entering terminate");
    free(encryptor);
    return 0;
}

static int init_from_config(PERCONA_ENCRYPTOR *pe, WT_CONFIG_ARG *config)
{
    WT_EXTENSION_API *wt_api = pe->wt_api;
    bool cipherMode = false;
    WT_CONFIG_PARSER *parser = NULL;
    int ret = wt_api->config_parser_open_arg(wt_api, NULL, config, &parser);
    if (ret != 0)
        return ret;

    WT_CONFIG_ITEM k, v;
    while ((ret = parser->next(parser, &k, &v)) == 0) {
        if (!strncmp("cipher", k.str, (int)k.len)) {
            if (!strncmp("AES256-CBC", v.str, (int)v.len)) {
                cipherMode = true;
                pe->cipher = EVP_aes_256_cbc();
                pe->encryptor.encrypt = percona_encrypt_cbc;
                pe->encryptor.decrypt = percona_decrypt_cbc;
                pe->encryptor.sizing = percona_sizing_cbc;
            }
            else if (!strncmp("AES256-GCM", v.str, (int)v.len)) {
                cipherMode = true;
                pe->cipher = EVP_aes_256_gcm();
                pe->encryptor.encrypt = percona_encrypt_gcm;
                pe->encryptor.decrypt = percona_decrypt_gcm;
                pe->encryptor.sizing = percona_sizing_gcm;
            }
            else
                return (report_error(pe, NULL, EINVAL, "specified cipher mode is not supported"));
        }
    }
    parser->close(parser);

    if (!cipherMode)
        return (report_error(pe, NULL, EINVAL, "cipher mode is not specified"));
    return 0;
}

int percona_encryption_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config) {
    int ret = 0;
    PERCONA_ENCRYPTOR *pe;
    WT_SESSION *session = NULL; // NULL session pointer for the DBG_MSG macro

    if ((pe = calloc(1, sizeof(PERCONA_ENCRYPTOR))) == NULL)
            return errno;

    pe->wt_api = connection->get_extension_api(connection);
    DBG_MSG("hello from the percona_encryption_extension_init");

    DBG dump_config_arg(pe, NULL, config);

    pe->encryptor.customize = percona_customize;
    pe->encryptor.terminate = percona_terminate;

    ret = init_from_config(pe, config);
    if (ret != 0)
        goto failure;

    pe->iv_len = EVP_CIPHER_iv_length(pe->cipher);
    DBG_MSG("IV len is %d", pe->iv_len);
    DBG_MSG("key len is %d", EVP_CIPHER_key_length(pe->cipher));

    // calloc initializes all allocated memory to zero
    // thus pe->key is filled with zeros
    // actual encryption keys are loaded by 'customize' callback

    return connection->add_encryptor(connection, "percona", (WT_ENCRYPTOR*)pe, NULL);

failure:
    free(pe);
    return ret;
}

