/*
 * Copyright (C) 2017 OpenHeadend S.A.R.L.
 *
 * Authors: Arnaud de Turckheim
 *          Rafaël Carré
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "upipe/config.h"
#include "upipe/upipe.h"
#include "upipe/upump.h"
#include "upipe/uclock.h"
#include "upipe/uref_block.h"
#include "upipe/uref_clock.h"

#include "upipe/upipe_helper_upipe.h"
#include "upipe/upipe_helper_urefcount.h"
#include "upipe/upipe_helper_flow.h"
#include "upipe/upipe_helper_output.h"
#include "upipe/upipe_helper_uclock.h"
#include "upipe/upipe_helper_upump_mgr.h"
#include "upipe/upipe_helper_upump.h"
#include "upipe/upipe_helper_input.h"

#include "upipe-dvbcsa/upipe_dvbcsa_decrypt.h"
#include "upipe-dvbcsa/upipe_dvbcsa_common.h"

#include <bitstream/mpeg/ts.h>
#include <dvbcsa/dvbcsa.h>

#ifdef UPIPE_HAVE_GCRYPT
#include <gcrypt.h>
#endif

#include "common.h"

/** expected input flow format */
#define EXPECTED_FLOW_DEF "block.mpegts."
/** Approximative worst dvbcsa decrypt latency on normal hardware (5ms)*/
#define DVBCSA_LATENCY  (UCLOCK_FREQ / 200)

/** @hidden */
static void upipe_dvbcsa_dec_worker(struct upump *upump);

enum mode {
    CSA,
    CSA_BS,
#ifdef UPIPE_HAVE_GCRYPT
    AES,
#endif
};

/** @This is the private structure of dvbcsa decryption pipe. */
struct upipe_dvbcsa_dec {
    /** public pipe structure */
    struct upipe upipe;
    /** urefcount structure */
    struct urefcount urefcount;
    /** output pipe */
    struct upipe *output;
    /** output flow definition */
    struct uref *flow_def;
    /** output state */
    enum upipe_helper_output_state output_state;
    /** request list */
    struct uchain requests;


    /** uclock */
    struct uclock *uclock;
    /** uclock request */
    struct urequest uclock_request;
    /** upump manager */
    struct upump_mgr *upump_mgr;
    /** upump */
    struct upump *upump;
    /** list of retained urefs */
    struct uchain urefs;
    /** number of retained urefs */
    unsigned int nb_urefs;
    /** maximum retained urefs */
    unsigned int max_urefs;
    /** blockers */
    struct uchain blockers;
    union {
        /** dvbcsa key (bs) */
        dvbcsa_bs_key_t *key_bs[2];
        /** dvbcsa key */
        dvbcsa_key_t *key[2];
#ifdef UPIPE_HAVE_GCRYPT
        /** AES handle */
        gcry_cipher_hd_t aes[2];
#endif
    };
    /* active current key */
    bool odd;
    /** maximum number of packet per batch */
    unsigned int batch_size;
    /** batch items */
    struct dvbcsa_bs_batch_s *batch;
    /** mapped urefs */
    struct uref **mapped;
    /** current batch item */
    unsigned current;

    /** batch mode */
    enum mode mode;

    /** common dvbcsa structure */
    struct upipe_dvbcsa_common common;
};

/** @hidden */
static int upipe_dvbcsa_dec_check(struct upipe *upipe,
                                  struct uref *flow_def);

/** @hidden */
UBASE_FROM_TO(upipe_dvbcsa_dec, upipe_dvbcsa_common, common, common);

UPIPE_HELPER_UPIPE(upipe_dvbcsa_dec, upipe, UPIPE_DVBCSA_DEC_SIGNATURE);
UPIPE_HELPER_UREFCOUNT(upipe_dvbcsa_dec, urefcount, upipe_dvbcsa_dec_free);
UPIPE_HELPER_FLOW(upipe_dvbcsa_dec, NULL);
UPIPE_HELPER_OUTPUT(upipe_dvbcsa_dec, output, flow_def, output_state, requests);
UPIPE_HELPER_UCLOCK(upipe_dvbcsa_dec, uclock, uclock_request,
                    upipe_dvbcsa_dec_check,
                    upipe_dvbcsa_dec_register_output_request,
                    upipe_dvbcsa_dec_unregister_output_request);
UPIPE_HELPER_UPUMP_MGR(upipe_dvbcsa_dec, upump_mgr);
UPIPE_HELPER_UPUMP(upipe_dvbcsa_dec, upump, upump_mgr);
UPIPE_HELPER_INPUT(upipe_dvbcsa_dec, urefs, nb_urefs, max_urefs, blockers,
                   NULL);

/** @internal @This frees a decryption key structure
 *
 * @param upipe description structure of the pipe
 */
static void upipe_dvbcsa_dec_free_key(struct upipe *upipe)
{
    struct upipe_dvbcsa_dec *upipe_dvbcsa_dec =
        upipe_dvbcsa_dec_from_upipe(upipe);

    switch (upipe_dvbcsa_dec->mode) {
    case CSA:
        for (int i = 0; i < 2; i++)
            dvbcsa_key_free(upipe_dvbcsa_dec->key[i]);
        break;
    case CSA_BS:
        for (int i = 0; i < 2; i++)
            dvbcsa_bs_key_free(upipe_dvbcsa_dec->key_bs[i]);
        break;
#ifdef UPIPE_HAVE_GCRYPT
    case AES:
        for (int i = 0; i < 2; i++)
            if (upipe_dvbcsa_dec->aes[i])
                gcry_cipher_close(upipe_dvbcsa_dec->aes[i]);
        break;
#endif
    }

    for (int i = 0; i < 2; i++)
        upipe_dvbcsa_dec->key[i] = NULL;
}

/** @internal @This frees a dvbcsa decryption pipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_dvbcsa_dec_free(struct upipe *upipe)
{
    struct upipe_dvbcsa_dec *upipe_dvbcsa_dec =
        upipe_dvbcsa_dec_from_upipe(upipe);
    struct upipe_dvbcsa_common *common =
        upipe_dvbcsa_dec_to_common(upipe_dvbcsa_dec);

    upipe_throw_dead(upipe);

    for (unsigned i = 0; i < upipe_dvbcsa_dec->current; i++)
        uref_block_unmap(upipe_dvbcsa_dec->mapped[i], 0);

    upipe_dvbcsa_dec_free_key(upipe);
    free(upipe_dvbcsa_dec->mapped);
    free(upipe_dvbcsa_dec->batch);
    upipe_dvbcsa_common_clean(common);
    upipe_dvbcsa_dec_clean_upump(upipe);
    upipe_dvbcsa_dec_clean_upump_mgr(upipe);
    upipe_dvbcsa_dec_clean_uclock(upipe);
    upipe_dvbcsa_dec_clean_input(upipe);
    upipe_dvbcsa_dec_clean_output(upipe);
    upipe_dvbcsa_dec_clean_urefcount(upipe);
    upipe_dvbcsa_dec_free_flow(upipe);
}

/** @internal @This allocates and initializes a dvbcsa decryption pipe.
 *
 * @param mgr pointer to pipe manager
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return an allocated and initialized pipe or NULL
 */
static struct upipe *upipe_dvbcsa_dec_alloc(struct upipe_mgr *mgr,
                                            struct uprobe *uprobe,
                                            uint32_t signature,
                                            va_list args)
{
    struct uref *flow_def;
    struct upipe *upipe =
        upipe_dvbcsa_dec_alloc_flow(mgr, uprobe, signature, args, &flow_def);
    if (unlikely(!upipe)) {
        return NULL;
    }
    struct upipe_dvbcsa_dec *upipe_dvbcsa_dec =
        upipe_dvbcsa_dec_from_upipe(upipe);
    struct upipe_dvbcsa_common *common =
        upipe_dvbcsa_dec_to_common(upipe_dvbcsa_dec);

#ifdef UPIPE_HAVE_GCRYPT
    if (!gcry_control(GCRYCTL_INITIALIZATION_FINISHED_P)) {
        uprobe_err(uprobe, upipe, "Application did not initialize libgcrypt, see "
                "https://www.gnupg.org/documentation/manuals/gcrypt/Initializing-the-library.html");
        upipe_dvbcsa_dec_free_flow(upipe);
        return NULL;
    }
#endif

    upipe_dvbcsa_dec_init_urefcount(upipe);
    upipe_dvbcsa_dec_init_output(upipe);
    upipe_dvbcsa_dec_init_input(upipe);
    upipe_dvbcsa_dec_init_uclock(upipe);
    upipe_dvbcsa_dec_init_upump_mgr(upipe);
    upipe_dvbcsa_dec_init_upump(upipe);
    upipe_dvbcsa_common_init(common);
    for (int i = 0; i < 2; i++)
        upipe_dvbcsa_dec->key[i] = NULL;
    unsigned bs_size = dvbcsa_bs_batch_size();
    upipe_dvbcsa_dec->batch_size = bs_size;
    upipe_dvbcsa_dec->batch = malloc((bs_size + 1) *
                                        sizeof (struct dvbcsa_bs_batch_s));
    upipe_dvbcsa_dec->mapped = malloc(bs_size * sizeof (struct uref *));
    upipe_dvbcsa_dec->current = 0;

    if (flow_def) {
        uint64_t latency;
        if (!ubase_check(uref_clock_get_latency(flow_def, &latency)))
            latency = 0;
        uref_free(flow_def);
        upipe_dvbcsa_dec->mode = CSA_BS;
        upipe_dvbcsa_common_set_max_latency(&upipe_dvbcsa_dec->common, latency);
    } else {
        upipe_dvbcsa_dec->mode = CSA;
    }

    upipe_throw_ready(upipe);

    if (unlikely(!upipe_dvbcsa_dec->batch ||
                 !upipe_dvbcsa_dec->mapped)) {
        upipe_err(upipe, "allocation failed");
        upipe_release(upipe);
        return NULL;
    }

    return upipe;
}

/** @internal @This sets the flow def for real.
 *
 * @param upipe description structure of the pipe
 * @param flow_def new flow definition
 * @return an error code
 */
static int upipe_dvbcsa_dec_set_flow_def_real(struct upipe *upipe,
                                              struct uref *flow_def)
{
    struct upipe_dvbcsa_dec *upipe_dvbcsa_dec =
        upipe_dvbcsa_dec_from_upipe(upipe);
    struct upipe_dvbcsa_common *common =
        upipe_dvbcsa_dec_to_common(upipe_dvbcsa_dec);
    if (upipe_dvbcsa_dec->mode == CSA_BS) {
        uint64_t latency = 0;
        uref_clock_get_latency(flow_def, &latency);
        latency += common->latency + DVBCSA_LATENCY;
        uref_clock_set_latency(flow_def, latency);
    }
    upipe_dvbcsa_dec_store_flow_def(upipe, flow_def);
    return UBASE_ERR_NONE;
}

/** @internal @This flushes the retained urefs.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_dvbcsa_dec_flush(struct upipe *upipe,
                                   struct upump **upump_p)
{
    struct upipe_dvbcsa_dec *upipe_dvbcsa_dec =
        upipe_dvbcsa_dec_from_upipe(upipe);

    upipe_dvbcsa_dec_set_upump(upipe, NULL);

    /* descramble remaining packets */
    unsigned current = upipe_dvbcsa_dec->current;
    if (current) {
        upipe_dvbcsa_dec->current = 0;
        upipe_dvbcsa_dec->batch[current].data = NULL;
        upipe_dvbcsa_dec->batch[current].len = 0;
        uint64_t before = uclock_now(upipe_dvbcsa_dec->uclock);
        dvbcsa_bs_decrypt(upipe_dvbcsa_dec->key_bs[upipe_dvbcsa_dec->odd],
                          upipe_dvbcsa_dec->batch, 184);
        uint64_t after = uclock_now(upipe_dvbcsa_dec->uclock);
        if ((after - before) > DVBCSA_LATENCY)
            upipe_warn_va(upipe, "dvbcsa latency too high %"PRIu64 "ms",
                          (after - before) / (UCLOCK_FREQ / 1000));
        for (unsigned i = 0; i < current; i++)
            uref_block_unmap(upipe_dvbcsa_dec->mapped[i], 0);
    }

    /* output */
    struct uref *uref;
    while ((uref = upipe_dvbcsa_dec_pop_input(upipe)))
        if (unlikely(ubase_check(uref_flow_get_def(uref, NULL))))
            /* handle flow format */
            upipe_dvbcsa_dec_set_flow_def_real(upipe, uref);
        else
            upipe_dvbcsa_dec_output(upipe, uref, upump_p);

    /* no more buffered urefs */
    upipe_release(upipe);
}

/** @internal @This is called when the upump triggers.
 *
 * @param upump timer
 */
static void upipe_dvbcsa_dec_worker(struct upump *upump)
{
    struct upipe *upipe = upump_get_opaque(upump, struct upipe *);
    return upipe_dvbcsa_dec_flush(upipe, &upump);
}

/** @internal @This handles the input buffers.
 *
 * @param upipe description structure of the pipe
 * @param uref input buffer to handle
 * @param upump_p reference to the pump that generated the buffer
 */
static void upipe_dvbcsa_dec_input(struct upipe *upipe,
                                   struct uref *uref,
                                   struct upump **upump_p)
{
    struct upipe_dvbcsa_dec *upipe_dvbcsa_dec =
        upipe_dvbcsa_dec_from_upipe(upipe);
    struct upipe_dvbcsa_common *common =
        upipe_dvbcsa_dec_to_common(upipe_dvbcsa_dec);
    bool first = upipe_dvbcsa_dec_check_input(upipe);

    /* handle new flow definition */
    if (unlikely(ubase_check(uref_flow_get_def(uref, NULL)))) {
        if (first)
            upipe_dvbcsa_dec_set_flow_def_real(upipe, uref);
        else
            upipe_dvbcsa_dec_hold_input(upipe, uref);
        return;
    }

    /* output if no dvbcsa key set */
    if (unlikely(!upipe_dvbcsa_dec->key[0])) {
        if (unlikely(!first))
            upipe_dvbcsa_dec_flush(upipe, upump_p);
        upipe_dvbcsa_dec_output(upipe, uref, upump_p);
        return;
    }

    /* get TS header */
    uint32_t ts_header_size = TS_HEADER_SIZE;
    uint8_t buf[TS_HEADER_SIZE];
    const uint8_t *ts_header = uref_block_peek(uref, 0, sizeof (buf), buf);
    if (unlikely(!ts_header)) {
        upipe_err(upipe, "fail to read TS header");
        uref_free(uref);
        return;
    }
    uint8_t scrambling = ts_get_scrambling(ts_header);
    bool has_payload = ts_has_payload(ts_header);
    bool has_adaptation = ts_has_adaptation(ts_header);
    uint16_t pid = ts_get_pid(ts_header);
    uref_block_peek_unmap(uref, 0, buf, ts_header);

    bool odd, valid = false;
    switch (scrambling) {
        case TS_SCRAMBLING_EVEN:
            odd = false;
            valid = true;
            break;
        case TS_SCRAMBLING_ODD:
            odd = true;
            valid = upipe_dvbcsa_dec->key[1];
            break;
    }

    if (!valid || !has_payload || !upipe_dvbcsa_common_check_pid(common, pid)) {
        if (first)
            upipe_dvbcsa_dec_output(upipe, uref, upump_p);
        else
            upipe_dvbcsa_dec_hold_input(upipe, uref);
        return;
    }

    /* get adaption field length */
    if (unlikely(has_adaptation)) {
        uint8_t af_length;
        int ret = uref_block_extract(uref, ts_header_size, 1, &af_length);
        if (unlikely(!ubase_check(ret))) {
            upipe_err(upipe, "fail to get adaptation field length");
            uref_free(uref);
            return;
        }
        if (unlikely(af_length >= 183)) {
            upipe_warn(upipe, "invalid adaptation field received");
            uref_free(uref);
            return;
        }
        ts_header_size += af_length + 1;
    }

    /* copy TS packet */
    struct ubuf *ubuf = ubuf_block_copy(uref->ubuf->mgr, uref->ubuf, 0, -1);
    if (unlikely(!ubuf)) {
        upipe_err(upipe, "fail to copy TS packet");
        uref_free(uref);
        return;
    }
    uref_attach_ubuf(uref, ubuf);

    /* add to descramble list  */
    int size = -1;
    uint8_t *ts;
    int ret = ubuf_block_write(ubuf, 0, &size, &ts);
    if (unlikely(!ubase_check(ret))) {
        upipe_err(upipe, "fail to scramble TS payload");
        uref_free(uref);
        return;
    }

    ts_set_scrambling(ts, 0);

#ifdef UPIPE_HAVE_GCRYPT
    if (upipe_dvbcsa_dec->mode == AES) {
        /* from biss2 spec */
        static const uint8_t cissa_iv[16] = {
            0x44, 0x56, 0x42, 0x54, 0x4d, 0x43, 0x50, 0x54,
            0x41, 0x45, 0x53, 0x43, 0x49, 0x53, 0x53, 0x41,
        };
        gcry_cipher_setiv(upipe_dvbcsa_dec->aes[odd], cissa_iv, 16);
        unsigned aes_len = (size - ts_header_size) & ~0xf;
        gcry_error_t err = gcry_cipher_decrypt(upipe_dvbcsa_dec->aes[odd],
                ts + ts_header_size, aes_len, NULL, 0);
        if (err) {
            upipe_err_va(upipe, "AES decryption failed");
        }
        return upipe_dvbcsa_dec_output(upipe, uref, upump_p);
    }
#endif

    if (upipe_dvbcsa_dec->mode == CSA) {
        dvbcsa_decrypt(upipe_dvbcsa_dec->key[odd],
                ts + ts_header_size,
                size - ts_header_size);
        return upipe_dvbcsa_dec_output(upipe, uref, upump_p);
    }

    /* biss mode */

    if (!first && upipe_dvbcsa_dec->odd != odd)
        upipe_dvbcsa_dec_flush(upipe, upump_p);
    upipe_dvbcsa_dec->odd = odd;

    unsigned current = upipe_dvbcsa_dec->current;
    upipe_dvbcsa_dec->batch[current].data = ts + ts_header_size;
    upipe_dvbcsa_dec->batch[current].len = size - ts_header_size;
    upipe_dvbcsa_dec->mapped[current] = uref;
    upipe_dvbcsa_dec->current++;

    /* hold uref */
    upipe_dvbcsa_dec_hold_input(upipe, uref);
    if (unlikely(first)) {
        /* make sure to send all buffered urefs */
        upipe_use(upipe);
        upipe_dvbcsa_dec_wait_upump(upipe, common->latency,
                                    upipe_dvbcsa_dec_worker);
    }

    /* descramble if we have enough buffered scrambled TS packets */
    if (upipe_dvbcsa_dec->current >= upipe_dvbcsa_dec->batch_size)
        upipe_dvbcsa_dec_flush(upipe, upump_p);
}

/** @internal @This allocates a new pump if needed.
 *
 * @param upipe description structure of the pipe
 * @return an error code
 */
static int upipe_dvbcsa_dec_check(struct upipe *upipe, struct uref *flow_def)
{
    struct upipe_dvbcsa_dec *upipe_dvbcsa_dec =
        upipe_dvbcsa_dec_from_upipe(upipe);

    if (unlikely(!upipe_dvbcsa_dec->uclock))
        upipe_dvbcsa_dec_require_uclock(upipe);

    UBASE_RETURN(upipe_dvbcsa_dec_check_upump_mgr(upipe));
    if (unlikely(!upipe_dvbcsa_dec->upump_mgr))
        return UBASE_ERR_NONE;

    return UBASE_ERR_NONE;
}

/** @internal @This set the output flow format.
 *
 * @param upipe description structure of the pipe
 * @param flow_def new flow format to set
 * @return an error code
 */
static int upipe_dvbcsa_dec_set_flow_def(struct upipe *upipe,
                                         struct uref *flow_def)
{
    UBASE_RETURN(uref_flow_match_def(flow_def, EXPECTED_FLOW_DEF));
    struct uref *flow_def_dup = uref_dup(flow_def);
    UBASE_ALLOC_RETURN(flow_def_dup);
    upipe_input(upipe, flow_def_dup, NULL);
    return UBASE_ERR_NONE;
}

/** @internal @This sets the decryption key.
 *
 * @param upipe description structure of the pipe
 * @param key decryption key
 * @return an error code
 */
static int upipe_dvbcsa_dec_set_key(struct upipe *upipe, const char *even_key, const char *odd_key)
{
    struct upipe_dvbcsa_dec *upipe_dvbcsa_dec =
        upipe_dvbcsa_dec_from_upipe(upipe);

    upipe_dvbcsa_dec_free_key(upipe);

    struct ustring_dvbcsa_cw even_cw = ustring_to_dvbcsa_cw(ustring_from_str(even_key));
    if (unlikely(ustring_is_empty(even_cw.str) || strlen(even_key) != even_cw.str.len))
        return UBASE_ERR_INVALID;

    /* odd key, optional */
    struct ustring_dvbcsa_cw odd_cw = ustring_to_dvbcsa_cw(ustring_from_str(odd_key));
    if (unlikely(!ustring_is_empty(odd_cw.str) && strlen(even_key) != odd_cw.str.len))
        return UBASE_ERR_INVALID;

    upipe_notice(upipe, "key changed");
    if (upipe_dvbcsa_dec->mode == CSA_BS) {
        upipe_dvbcsa_dec->key_bs[0] = dvbcsa_bs_key_alloc();
        UBASE_ALLOC_RETURN(upipe_dvbcsa_dec->key_bs[0]);
        dvbcsa_bs_key_set(even_cw.value, upipe_dvbcsa_dec->key_bs[0]);
        if (ustring_is_empty(odd_cw.str))
            return UBASE_ERR_NONE;

        upipe_dvbcsa_dec->key_bs[1] = dvbcsa_bs_key_alloc();
        if (!upipe_dvbcsa_dec->key_bs[1])
            upipe_dvbcsa_dec_free_key(upipe);
        UBASE_ALLOC_RETURN(upipe_dvbcsa_dec->key_bs[1]);
        dvbcsa_bs_key_set(odd_cw.value, upipe_dvbcsa_dec->key_bs[1]);
#ifdef UPIPE_HAVE_GCRYPT
    } else if (even_cw.str.len >= 32) {
        upipe_dvbcsa_dec->mode = AES;
        gcry_error_t err;
        err = gcry_cipher_open(&upipe_dvbcsa_dec->aes[0], GCRY_CIPHER_AES,
                GCRY_CIPHER_MODE_CBC, 0);
        if (err)
            goto error;

        err = gcry_cipher_setkey(upipe_dvbcsa_dec->aes[0], even_cw.aes, 16);
        if (err)
            goto error;

        if (ustring_is_empty(odd_cw.str))
            return UBASE_ERR_NONE;

        err = gcry_cipher_open(&upipe_dvbcsa_dec->aes[1], GCRY_CIPHER_AES,
                GCRY_CIPHER_MODE_CBC, 0);
        if (err)
            goto error;

        err = gcry_cipher_setkey(upipe_dvbcsa_dec->aes[1], odd_cw.aes, 16);
        if (err)
            goto error;

        return UBASE_ERR_NONE;
error:
            upipe_dvbcsa_dec_free_key(upipe);
            upipe_dvbcsa_dec->aes[0] = upipe_dvbcsa_dec->aes[1] = NULL;
            return UBASE_ERR_EXTERNAL;
#endif
    } else {
        upipe_dvbcsa_dec->mode = CSA;
        upipe_dvbcsa_dec->key[0] = dvbcsa_key_alloc();
        UBASE_ALLOC_RETURN(upipe_dvbcsa_dec->key[0]);
        dvbcsa_key_set(even_cw.value, upipe_dvbcsa_dec->key[0]);

        if (ustring_is_empty(odd_cw.str))
            return UBASE_ERR_NONE;

        upipe_dvbcsa_dec->key[1] = dvbcsa_key_alloc();
        if (!upipe_dvbcsa_dec->key[1])
            upipe_dvbcsa_dec_free_key(upipe);
        UBASE_ALLOC_RETURN(upipe_dvbcsa_dec->key[1]);
        dvbcsa_key_set(odd_cw.value, upipe_dvbcsa_dec->key[1]);
    }

    return UBASE_ERR_NONE;
}

/** @internal @This handles the pipe control commands.
 *
 * @param upipe description structure of the pipe
 * @param command control command to handle
 * @param args optional arguments
 * @return an error code
 */
static int upipe_dvbcsa_dec_control_real(struct upipe *upipe,
                                         int command,
                                         va_list args)
{
    struct upipe_dvbcsa_dec *upipe_dvbcsa_dec =
        upipe_dvbcsa_dec_from_upipe(upipe);
    struct upipe_dvbcsa_common *common =
        upipe_dvbcsa_dec_to_common(upipe_dvbcsa_dec);
    UBASE_HANDLED_RETURN(upipe_dvbcsa_dec_control_output(upipe, command, args));

    switch (command) {
        case UPIPE_ATTACH_UPUMP_MGR:
            return upipe_dvbcsa_dec_attach_upump_mgr(upipe);

        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_dvbcsa_dec_set_flow_def(upipe, flow_def);
        }

        case UPIPE_DVBCSA_SET_KEY: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_DVBCSA_COMMON_SIGNATURE);
            const char *even_key = va_arg(args, const char *);
            const char *odd_key = va_arg(args, const char *);
            return upipe_dvbcsa_dec_set_key(upipe, even_key, odd_key);
        }
        case UPIPE_DVBCSA_ADD_PID:
        case UPIPE_DVBCSA_DEL_PID:
            return upipe_dvbcsa_common_control(common, command, args);
    }
    return UBASE_ERR_UNHANDLED;
}

/** @internal @This handles a pipe control command and checks the upump
 * manager.
 *
 * @param upipe description structure of the pipe
 * @param command control command to handle
 * @param args optional arguments
 * @return an error code
 */
static int upipe_dvbcsa_dec_control(struct upipe *upipe,
                                    int command, va_list args)
{
    UBASE_RETURN(upipe_dvbcsa_dec_control_real(upipe, command, args));
    return upipe_dvbcsa_dec_check(upipe, NULL);
}

/** @internal @This is the management structure for dvbcsa decryption pipes. */
static struct upipe_mgr upipe_dvbcsa_dec_mgr = {
    /** pipe signature */
    .signature = UPIPE_DVBCSA_DEC_SIGNATURE,
    /** no refcounting needed */
    .refcount = NULL,
    /** pipe allocation */
    .upipe_alloc = upipe_dvbcsa_dec_alloc,
    /** input handler */
    .upipe_input = upipe_dvbcsa_dec_input,
    /** control command handler */
    .upipe_control = upipe_dvbcsa_dec_control,
};

/** @This returns the dvbcsa decrypt pipe management structure.
 *
 * @return a pointer to the manager
 */
struct upipe_mgr *upipe_dvbcsa_dec_mgr_alloc(void)
{
    return &upipe_dvbcsa_dec_mgr;
}
