/*
 * Copyright 2019 The OpenSSL Project Authors. All Rights Reserved.
 * Copyright (c) 2019, Oracle and/or its affiliates.  All rights reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

/*
 * Refer to https://csrc.nist.gov/publications/detail/sp/800-56c/rev-1/final
 * Section 4.1.
 *
 * The Single Step KDF algorithm is given by:
 *
 * Result(0) = empty bit string (i.e., the null string).
 * For i = 1 to reps, do the following:
 *   Increment counter by 1.
 *   Result(i) = Result(i - 1) || H(counter || Z || FixedInfo).
 * DKM = LeftmostBits(Result(reps), L))
 *
 * NOTES:
 *   Z is a shared secret required to produce the derived key material.
 *   counter is a 4 byte buffer.
 *   FixedInfo is a bit string containing context specific data.
 *   DKM is the output derived key material.
 *   L is the required size of the DKM.
 *   reps = [L / H_outputBits]
 *   H(x) is the auxiliary function that can be either a hash, HMAC or KMAC.
 *   H_outputBits is the length of the output of the auxiliary function H(x).
 *
 * Currently there is not a comprehensive list of test vectors for this
 * algorithm, especially for H(x) = HMAC and H(x) = KMAC.
 * Test vectors for H(x) = Hash are indirectly used by CAVS KAS tests.
 */
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include "internal/cryptlib.h"
#include "internal/numbers.h"
#include "internal/evp_int.h"
#include "internal/provider_ctx.h"
#include "internal/providercommonerr.h"
#include "internal/provider_algs.h"
#include "internal/provider_util.h"

typedef struct {
    void *provctx;
    EVP_MAC *mac;       /* H(x) = HMAC_hash OR H(x) = KMAC */
    PROV_DIGEST digest;
    unsigned char *secret;
    size_t secret_len;
    unsigned char *info;
    size_t info_len;
    unsigned char *salt;
    size_t salt_len;
    size_t out_len; /* optional KMAC parameter */
} KDF_SSKDF;

#define SSKDF_MAX_INLEN (1<<30)
#define SSKDF_KMAC128_DEFAULT_SALT_SIZE (168 - 4)
#define SSKDF_KMAC256_DEFAULT_SALT_SIZE (136 - 4)

/* KMAC uses a Customisation string of 'KDF' */
static const unsigned char kmac_custom_str[] = { 0x4B, 0x44, 0x46 };

static OSSL_OP_kdf_newctx_fn sskdf_new;
static OSSL_OP_kdf_freectx_fn sskdf_free;
static OSSL_OP_kdf_reset_fn sskdf_reset;
static OSSL_OP_kdf_derive_fn sskdf_derive;
static OSSL_OP_kdf_derive_fn x963kdf_derive;
static OSSL_OP_kdf_settable_ctx_params_fn sskdf_settable_ctx_params;
static OSSL_OP_kdf_set_ctx_params_fn sskdf_set_ctx_params;
static OSSL_OP_kdf_gettable_ctx_params_fn sskdf_gettable_ctx_params;
static OSSL_OP_kdf_get_ctx_params_fn sskdf_get_ctx_params;

/*
 * Refer to https://csrc.nist.gov/publications/detail/sp/800-56c/rev-1/final
 * Section 4. One-Step Key Derivation using H(x) = hash(x)
 * Note: X9.63 also uses this code with the only difference being that the
 * counter is appended to the secret 'z'.
 * i.e.
 *   result[i] = Hash(counter || z || info) for One Step OR
 *   result[i] = Hash(z || counter || info) for X9.63.
 */
static int SSKDF_hash_kdm(const EVP_MD *kdf_md,
                          const unsigned char *z, size_t z_len,
                          const unsigned char *info, size_t info_len,
                          unsigned int append_ctr,
                          unsigned char *derived_key, size_t derived_key_len)
{
    int ret = 0, hlen;
    size_t counter, out_len, len = derived_key_len;
    unsigned char c[4];
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned char *out = derived_key;
    EVP_MD_CTX *ctx = NULL, *ctx_init = NULL;

    if (z_len > SSKDF_MAX_INLEN || info_len > SSKDF_MAX_INLEN
            || derived_key_len > SSKDF_MAX_INLEN
            || derived_key_len == 0)
        return 0;

    hlen = EVP_MD_size(kdf_md);
    if (hlen <= 0)
        return 0;
    out_len = (size_t)hlen;

    ctx = EVP_MD_CTX_create();
    ctx_init = EVP_MD_CTX_create();
    if (ctx == NULL || ctx_init == NULL)
        goto end;

    if (!EVP_DigestInit(ctx_init, kdf_md))
        goto end;

    for (counter = 1;; counter++) {
        c[0] = (unsigned char)((counter >> 24) & 0xff);
        c[1] = (unsigned char)((counter >> 16) & 0xff);
        c[2] = (unsigned char)((counter >> 8) & 0xff);
        c[3] = (unsigned char)(counter & 0xff);

        if (!(EVP_MD_CTX_copy_ex(ctx, ctx_init)
                && (append_ctr || EVP_DigestUpdate(ctx, c, sizeof(c)))
                && EVP_DigestUpdate(ctx, z, z_len)
                && (!append_ctr || EVP_DigestUpdate(ctx, c, sizeof(c)))
                && EVP_DigestUpdate(ctx, info, info_len)))
            goto end;
        if (len >= out_len) {
            if (!EVP_DigestFinal_ex(ctx, out, NULL))
                goto end;
            out += out_len;
            len -= out_len;
            if (len == 0)
                break;
        } else {
            if (!EVP_DigestFinal_ex(ctx, mac, NULL))
                goto end;
            memcpy(out, mac, len);
            break;
        }
    }
    ret = 1;
end:
    EVP_MD_CTX_destroy(ctx);
    EVP_MD_CTX_destroy(ctx_init);
    OPENSSL_cleanse(mac, sizeof(mac));
    return ret;
}

static int kmac_init(EVP_MAC_CTX *ctx, const unsigned char *custom,
                     size_t custom_len, size_t kmac_out_len,
                     size_t derived_key_len, unsigned char **out)
{
    OSSL_PARAM params[2];

    /* Only KMAC has custom data - so return if not KMAC */
    if (custom == NULL)
        return 1;

    params[0] = OSSL_PARAM_construct_octet_string(OSSL_MAC_PARAM_CUSTOM,
                                                  (void *)custom, custom_len);
    params[1] = OSSL_PARAM_construct_end();

    if (!EVP_MAC_CTX_set_params(ctx, params))
        return 0;

    /* By default only do one iteration if kmac_out_len is not specified */
    if (kmac_out_len == 0)
        kmac_out_len = derived_key_len;
    /* otherwise check the size is valid */
    else if (!(kmac_out_len == derived_key_len
            || kmac_out_len == 20
            || kmac_out_len == 28
            || kmac_out_len == 32
            || kmac_out_len == 48
            || kmac_out_len == 64))
        return 0;

    params[0] = OSSL_PARAM_construct_size_t(OSSL_MAC_PARAM_SIZE,
                                            &kmac_out_len);

    if (EVP_MAC_CTX_set_params(ctx, params) <= 0)
        return 0;

    /*
     * For kmac the output buffer can be larger than EVP_MAX_MD_SIZE: so
     * alloc a buffer for this case.
     */
    if (kmac_out_len > EVP_MAX_MD_SIZE) {
        *out = OPENSSL_zalloc(kmac_out_len);
        if (*out == NULL)
            return 0;
    }
    return 1;
}

/*
 * Refer to https://csrc.nist.gov/publications/detail/sp/800-56c/rev-1/final
 * Section 4. One-Step Key Derivation using MAC: i.e either
 *     H(x) = HMAC-hash(salt, x) OR
 *     H(x) = KMAC#(salt, x, outbits, CustomString='KDF')
 */
static int SSKDF_mac_kdm(EVP_MAC *kdf_mac, const EVP_MD *hmac_md,
                         const unsigned char *kmac_custom,
                         size_t kmac_custom_len, size_t kmac_out_len,
                         const unsigned char *salt, size_t salt_len,
                         const unsigned char *z, size_t z_len,
                         const unsigned char *info, size_t info_len,
                         unsigned char *derived_key, size_t derived_key_len)
{
    int ret = 0;
    size_t counter, out_len, len;
    unsigned char c[4];
    unsigned char mac_buf[EVP_MAX_MD_SIZE];
    unsigned char *out = derived_key;
    EVP_MAC_CTX *ctx = NULL, *ctx_init = NULL;
    unsigned char *mac = mac_buf, *kmac_buffer = NULL;
    OSSL_PARAM params[3];
    size_t params_n = 0;

    if (z_len > SSKDF_MAX_INLEN || info_len > SSKDF_MAX_INLEN
            || derived_key_len > SSKDF_MAX_INLEN
            || derived_key_len == 0)
        return 0;

    ctx_init = EVP_MAC_CTX_new(kdf_mac);
    if (ctx_init == NULL)
        goto end;

    if (hmac_md != NULL) {
        const char *mdname = EVP_MD_name(hmac_md);
        params[params_n++] =
            OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST,
                                             (char *)mdname, 0);
    }
    params[params_n++] =
        OSSL_PARAM_construct_octet_string(OSSL_MAC_PARAM_KEY, (void *)salt,
                                          salt_len);
    params[params_n] = OSSL_PARAM_construct_end();

    if (!EVP_MAC_CTX_set_params(ctx_init, params))
        goto end;

    if (!kmac_init(ctx_init, kmac_custom, kmac_custom_len, kmac_out_len,
                   derived_key_len, &kmac_buffer))
        goto end;
    if (kmac_buffer != NULL)
        mac = kmac_buffer;

    if (!EVP_MAC_init(ctx_init))
        goto end;

    out_len = EVP_MAC_size(ctx_init); /* output size */
    if (out_len <= 0)
        goto end;
    len = derived_key_len;

    for (counter = 1;; counter++) {
        c[0] = (unsigned char)((counter >> 24) & 0xff);
        c[1] = (unsigned char)((counter >> 16) & 0xff);
        c[2] = (unsigned char)((counter >> 8) & 0xff);
        c[3] = (unsigned char)(counter & 0xff);

        ctx = EVP_MAC_CTX_dup(ctx_init);
        if (!(ctx != NULL
                && EVP_MAC_update(ctx, c, sizeof(c))
                && EVP_MAC_update(ctx, z, z_len)
                && EVP_MAC_update(ctx, info, info_len)))
            goto end;
        if (len >= out_len) {
            if (!EVP_MAC_final(ctx, out, NULL, len))
                goto end;
            out += out_len;
            len -= out_len;
            if (len == 0)
                break;
        } else {
            if (!EVP_MAC_final(ctx, mac, NULL, len))
                goto end;
            memcpy(out, mac, len);
            break;
        }
        EVP_MAC_CTX_free(ctx);
        ctx = NULL;
    }
    ret = 1;
end:
    if (kmac_buffer != NULL)
        OPENSSL_clear_free(kmac_buffer, kmac_out_len);
    else
        OPENSSL_cleanse(mac_buf, sizeof(mac_buf));

    EVP_MAC_CTX_free(ctx);
    EVP_MAC_CTX_free(ctx_init);
    return ret;
}

static void *sskdf_new(void *provctx)
{
    KDF_SSKDF *ctx;

    if ((ctx = OPENSSL_zalloc(sizeof(*ctx))) == NULL)
        ERR_raise(ERR_LIB_PROV, ERR_R_MALLOC_FAILURE);
    ctx->provctx = provctx;
    return ctx;
}

static void sskdf_reset(void *vctx)
{
    KDF_SSKDF *ctx = (KDF_SSKDF *)vctx;

    ossl_prov_digest_reset(&ctx->digest);
    EVP_MAC_free(ctx->mac);
    OPENSSL_clear_free(ctx->secret, ctx->secret_len);
    OPENSSL_clear_free(ctx->info, ctx->info_len);
    OPENSSL_clear_free(ctx->salt, ctx->salt_len);
    memset(ctx, 0, sizeof(*ctx));
}

static void sskdf_free(void *vctx)
{
    KDF_SSKDF *ctx = (KDF_SSKDF *)vctx;

    sskdf_reset(ctx);
    OPENSSL_free(ctx);
}

static int sskdf_set_buffer(unsigned char **out, size_t *out_len,
                            const OSSL_PARAM *p)
{
    if (p->data == NULL || p->data_size == 0)
        return 1;
    OPENSSL_free(*out);
    *out = NULL;
    return OSSL_PARAM_get_octet_string(p, (void **)out, 0, out_len);
}

static size_t sskdf_size(KDF_SSKDF *ctx)
{
    int len;
    const EVP_MD *md = ossl_prov_digest_md(&ctx->digest);

    if (md == NULL) {
        ERR_raise(ERR_LIB_PROV, PROV_R_MISSING_MESSAGE_DIGEST);
        return 0;
    }
    len = EVP_MD_size(md);
    return (len <= 0) ? 0 : (size_t)len;
}

static int sskdf_derive(void *vctx, unsigned char *key, size_t keylen)
{
    KDF_SSKDF *ctx = (KDF_SSKDF *)vctx;
    const EVP_MD *md = ossl_prov_digest_md(&ctx->digest);

    if (ctx->secret == NULL) {
        ERR_raise(ERR_LIB_PROV, PROV_R_MISSING_SECRET);
        return 0;
    }

    if (ctx->mac != NULL) {
        /* H(x) = KMAC or H(x) = HMAC */
        int ret;
        const unsigned char *custom = NULL;
        size_t custom_len = 0;
        int default_salt_len;

        /*
         * TODO(3.0) investigate the necessity to have all these controls.
         * Why does KMAC require a salt length that's shorter than the MD
         * block size?
         */
        if (EVP_MAC_is_a(ctx->mac, OSSL_MAC_NAME_HMAC)) {
            /* H(x) = HMAC(x, salt, hash) */
            if (md == NULL) {
                ERR_raise(ERR_LIB_PROV, PROV_R_MISSING_MESSAGE_DIGEST);
                return 0;
            }
            default_salt_len = EVP_MD_block_size(md);
            if (default_salt_len <= 0)
                return 0;
        } else if (EVP_MAC_is_a(ctx->mac, OSSL_MAC_NAME_KMAC128)
                   || EVP_MAC_is_a(ctx->mac, OSSL_MAC_NAME_KMAC256)) {
            /* H(x) = KMACzzz(x, salt, custom) */
            custom = kmac_custom_str;
            custom_len = sizeof(kmac_custom_str);
            if (EVP_MAC_is_a(ctx->mac, OSSL_MAC_NAME_KMAC128))
                default_salt_len = SSKDF_KMAC128_DEFAULT_SALT_SIZE;
            else
                default_salt_len = SSKDF_KMAC256_DEFAULT_SALT_SIZE;
        } else {
            ERR_raise(ERR_LIB_PROV, PROV_R_UNSUPPORTED_MAC_TYPE);
            return 0;
        }
        /* If no salt is set then use a default_salt of zeros */
        if (ctx->salt == NULL || ctx->salt_len <= 0) {
            ctx->salt = OPENSSL_zalloc(default_salt_len);
            if (ctx->salt == NULL) {
                ERR_raise(ERR_LIB_PROV, ERR_R_MALLOC_FAILURE);
                return 0;
            }
            ctx->salt_len = default_salt_len;
        }
        ret = SSKDF_mac_kdm(ctx->mac, md,
                            custom, custom_len, ctx->out_len,
                            ctx->salt, ctx->salt_len,
                            ctx->secret, ctx->secret_len,
                            ctx->info, ctx->info_len, key, keylen);
        return ret;
    } else {
        /* H(x) = hash */
        if (md == NULL) {
            ERR_raise(ERR_LIB_PROV, PROV_R_MISSING_MESSAGE_DIGEST);
            return 0;
        }
        return SSKDF_hash_kdm(md, ctx->secret, ctx->secret_len,
                              ctx->info, ctx->info_len, 0, key, keylen);
    }
}

static int x963kdf_derive(void *vctx, unsigned char *key, size_t keylen)
{
    KDF_SSKDF *ctx = (KDF_SSKDF *)vctx;
    const EVP_MD *md = ossl_prov_digest_md(&ctx->digest);

    if (ctx->secret == NULL) {
        ERR_raise(ERR_LIB_PROV, PROV_R_MISSING_SECRET);
        return 0;
    }

    if (ctx->mac != NULL) {
        ERR_raise(ERR_LIB_PROV, PROV_R_NOT_SUPPORTED);
        return 0;
    } else {
        /* H(x) = hash */
        if (md == NULL) {
            ERR_raise(ERR_LIB_PROV, PROV_R_MISSING_MESSAGE_DIGEST);
            return 0;
        }
        return SSKDF_hash_kdm(md, ctx->secret, ctx->secret_len,
                              ctx->info, ctx->info_len, 1, key, keylen);
    }
}

static int sskdf_set_ctx_params(void *vctx, const OSSL_PARAM params[])
{
    const OSSL_PARAM *p;
    KDF_SSKDF *ctx = vctx;
    OPENSSL_CTX *provctx = PROV_LIBRARY_CONTEXT_OF(ctx->provctx);
    EVP_MAC *mac;
    size_t sz;
    const char *properties = NULL;

    if (!ossl_prov_digest_load_from_params(&ctx->digest, params, provctx))
        return 0;

    if ((p = OSSL_PARAM_locate_const(params, OSSL_KDF_PARAM_MAC)) != NULL) {
        EVP_MAC_free(ctx->mac);
        ctx->mac = NULL;

        mac = EVP_MAC_fetch(PROV_LIBRARY_CONTEXT_OF(ctx->provctx), p->data,
                            properties);
        if (mac == NULL)
            return 0;
        EVP_MAC_free(ctx->mac);
        ctx->mac = mac;
    }

    if ((p = OSSL_PARAM_locate_const(params, OSSL_KDF_PARAM_SECRET)) != NULL
        || (p = OSSL_PARAM_locate_const(params, OSSL_KDF_PARAM_KEY)) != NULL)
        if (!sskdf_set_buffer(&ctx->secret, &ctx->secret_len, p))
            return 0;

    if ((p = OSSL_PARAM_locate_const(params, OSSL_KDF_PARAM_INFO)) != NULL)
        if (!sskdf_set_buffer(&ctx->info, &ctx->info_len, p))
            return 0;

    if ((p = OSSL_PARAM_locate_const(params, OSSL_KDF_PARAM_SALT)) != NULL)
        if (!sskdf_set_buffer(&ctx->salt, &ctx->salt_len, p))
            return 0;

    if ((p = OSSL_PARAM_locate_const(params, OSSL_KDF_PARAM_MAC_SIZE))
        != NULL) {
        if (!OSSL_PARAM_get_size_t(p, &sz) || sz == 0)
            return 0;
        ctx->out_len = sz;
    }
    return 1;
}

static const OSSL_PARAM *sskdf_settable_ctx_params(void)
{
    static const OSSL_PARAM known_settable_ctx_params[] = {
        OSSL_PARAM_octet_string(OSSL_KDF_PARAM_SECRET, NULL, 0),
        OSSL_PARAM_octet_string(OSSL_KDF_PARAM_KEY, NULL, 0),
        OSSL_PARAM_octet_string(OSSL_KDF_PARAM_INFO, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_KDF_PARAM_PROPERTIES, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_KDF_PARAM_DIGEST, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_KDF_PARAM_MAC, NULL, 0),
        OSSL_PARAM_octet_string(OSSL_KDF_PARAM_SALT, NULL, 0),
        OSSL_PARAM_size_t(OSSL_KDF_PARAM_MAC_SIZE, NULL),
        OSSL_PARAM_END
    };
    return known_settable_ctx_params;
}

static int sskdf_get_ctx_params(void *vctx, OSSL_PARAM params[])
{
    KDF_SSKDF *ctx = (KDF_SSKDF *)vctx;
    OSSL_PARAM *p;

    if ((p = OSSL_PARAM_locate(params, OSSL_KDF_PARAM_SIZE)) != NULL)
        return OSSL_PARAM_set_size_t(p, sskdf_size(ctx));
    return -2;
}

static const OSSL_PARAM *sskdf_gettable_ctx_params(void)
{
    static const OSSL_PARAM known_gettable_ctx_params[] = {
        OSSL_PARAM_size_t(OSSL_KDF_PARAM_SIZE, NULL),
        OSSL_PARAM_END
    };
    return known_gettable_ctx_params;
}

const OSSL_DISPATCH kdf_sskdf_functions[] = {
    { OSSL_FUNC_KDF_NEWCTX, (void(*)(void))sskdf_new },
    { OSSL_FUNC_KDF_FREECTX, (void(*)(void))sskdf_free },
    { OSSL_FUNC_KDF_RESET, (void(*)(void))sskdf_reset },
    { OSSL_FUNC_KDF_DERIVE, (void(*)(void))sskdf_derive },
    { OSSL_FUNC_KDF_SETTABLE_CTX_PARAMS,
      (void(*)(void))sskdf_settable_ctx_params },
    { OSSL_FUNC_KDF_SET_CTX_PARAMS, (void(*)(void))sskdf_set_ctx_params },
    { OSSL_FUNC_KDF_GETTABLE_CTX_PARAMS,
      (void(*)(void))sskdf_gettable_ctx_params },
    { OSSL_FUNC_KDF_GET_CTX_PARAMS, (void(*)(void))sskdf_get_ctx_params },
    { 0, NULL }
};

const OSSL_DISPATCH kdf_x963_kdf_functions[] = {
    { OSSL_FUNC_KDF_NEWCTX, (void(*)(void))sskdf_new },
    { OSSL_FUNC_KDF_FREECTX, (void(*)(void))sskdf_free },
    { OSSL_FUNC_KDF_RESET, (void(*)(void))sskdf_reset },
    { OSSL_FUNC_KDF_DERIVE, (void(*)(void))x963kdf_derive },
    { OSSL_FUNC_KDF_SETTABLE_CTX_PARAMS,
      (void(*)(void))sskdf_settable_ctx_params },
    { OSSL_FUNC_KDF_SET_CTX_PARAMS, (void(*)(void))sskdf_set_ctx_params },
    { OSSL_FUNC_KDF_GETTABLE_CTX_PARAMS,
      (void(*)(void))sskdf_gettable_ctx_params },
    { OSSL_FUNC_KDF_GET_CTX_PARAMS, (void(*)(void))sskdf_get_ctx_params },
    { 0, NULL }
};
