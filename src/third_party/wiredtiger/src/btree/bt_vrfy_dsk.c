/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __err_cell_corrupt(WT_SESSION_IMPL *, int, uint32_t, const char *);
static int __err_cell_corrupt_or_eof(WT_SESSION_IMPL *, int, uint32_t, const char *);
static int __err_cell_type(WT_SESSION_IMPL *, uint32_t, const char *, uint8_t, uint8_t);
static int __verify_dsk_chunk(WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *, uint32_t);
static int __verify_dsk_col_fix(WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *);
static int __verify_dsk_col_int(WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *, WT_ADDR *);
static int __verify_dsk_col_var(WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *, WT_ADDR *);
static int __verify_dsk_memsize(WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *, WT_CELL *);
static int __verify_dsk_row(WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *, WT_ADDR *);

#define WT_ERR_VRFY(session, ...)                               \
    do {                                                        \
        if (!(F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE))) \
            __wt_errx(session, __VA_ARGS__);                    \
        goto err;                                               \
    } while (0)

#define WT_RET_VRFY_RETVAL(session, ret, ...)                     \
    do {                                                          \
        if (!(F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE))) { \
            if ((ret) == 0)                                       \
                __wt_errx(session, __VA_ARGS__);                  \
            else                                                  \
                __wt_err(session, ret, __VA_ARGS__);              \
        }                                                         \
        return ((ret) == 0 ? WT_ERROR : ret);                     \
    } while (0)

#define WT_RET_VRFY(session, ...) WT_RET_VRFY_RETVAL(session, 0, __VA_ARGS__)

/*
 * WT_CELL_FOREACH_VRFY --
 *	Iterate through each cell on a page. Verify-specific version of the
 * WT_CELL_FOREACH macro, created because the loop can't simply unpack cells,
 * verify has to do additional work to ensure that unpack is safe.
 */
#define WT_CELL_FOREACH_VRFY(btree, dsk, cell, unpack, i)                           \
    for ((cell) = WT_PAGE_HEADER_BYTE(btree, dsk), (i) = (dsk)->u.entries; (i) > 0; \
         (cell) = (WT_CELL *)((uint8_t *)(cell) + (unpack)->__len), --(i))

/*
 * __wt_verify_dsk_image --
 *     Verify a single block as read from disk.
 */
int
__wt_verify_dsk_image(WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk,
  size_t size, WT_ADDR *addr, bool empty_page_ok)
{
    uint8_t flags;
    const uint8_t *p, *end;

    /* Check the page type. */
    switch (dsk->type) {
    case WT_PAGE_BLOCK_MANAGER:
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_INT:
    case WT_PAGE_COL_VAR:
    case WT_PAGE_OVFL:
    case WT_PAGE_ROW_INT:
    case WT_PAGE_ROW_LEAF:
        break;
    case WT_PAGE_INVALID:
    default:
        WT_RET_VRFY(session, "page at %s has an invalid type of %" PRIu32, tag, dsk->type);
    }

    /* Check the page record number. */
    switch (dsk->type) {
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_INT:
    case WT_PAGE_COL_VAR:
        if (dsk->recno != WT_RECNO_OOB)
            break;
        WT_RET_VRFY(session, "%s page at %s has an invalid record number of %d",
          __wt_page_type_string(dsk->type), tag, WT_RECNO_OOB);
    case WT_PAGE_BLOCK_MANAGER:
    case WT_PAGE_OVFL:
    case WT_PAGE_ROW_INT:
    case WT_PAGE_ROW_LEAF:
        if (dsk->recno == WT_RECNO_OOB)
            break;
        WT_RET_VRFY(session,
          "%s page at %s has a record number, which is illegal for "
          "this page type",
          __wt_page_type_string(dsk->type), tag);
    }

    /* Check the page flags. */
    flags = dsk->flags;
    if (LF_ISSET(WT_PAGE_COMPRESSED))
        LF_CLR(WT_PAGE_COMPRESSED);
    if (dsk->type == WT_PAGE_ROW_LEAF) {
        if (LF_ISSET(WT_PAGE_EMPTY_V_ALL) && LF_ISSET(WT_PAGE_EMPTY_V_NONE))
            WT_RET_VRFY(
              session, "page at %s has invalid flags combination: 0x%" PRIx8, tag, dsk->flags);
        if (LF_ISSET(WT_PAGE_EMPTY_V_ALL))
            LF_CLR(WT_PAGE_EMPTY_V_ALL);
        if (LF_ISSET(WT_PAGE_EMPTY_V_NONE))
            LF_CLR(WT_PAGE_EMPTY_V_NONE);
    }
    if (LF_ISSET(WT_PAGE_ENCRYPTED))
        LF_CLR(WT_PAGE_ENCRYPTED);
    if (LF_ISSET(WT_PAGE_UNUSED))
        LF_CLR(WT_PAGE_UNUSED);
    if (flags != 0)
        WT_RET_VRFY(session, "page at %s has invalid flags set: 0x%" PRIx8, tag, flags);

    /* Check the unused byte. */
    if (dsk->unused != 0)
        WT_RET_VRFY(session, "page at %s has non-zero unused page header bytes", tag);

    /*
     * Any bytes after the data chunk should be nul bytes; ignore if the size is 0, that allows easy
     * checking of disk images where we don't have the size.
     */
    if (size != 0) {
        p = (uint8_t *)dsk + dsk->mem_size;
        end = (uint8_t *)dsk + size;
        for (; p < end; ++p)
            if (*p != '\0')
                WT_RET_VRFY(session, "%s page at %s has non-zero trailing bytes",
                  __wt_page_type_string(dsk->type), tag);
    }

    /* Check for empty pages, then verify the items on the page. */
    switch (dsk->type) {
    case WT_PAGE_COL_INT:
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_VAR:
    case WT_PAGE_ROW_INT:
    case WT_PAGE_ROW_LEAF:
        if (!empty_page_ok && dsk->u.entries == 0)
            WT_RET_VRFY(
              session, "%s page at %s has no entries", __wt_page_type_string(dsk->type), tag);
        break;
    case WT_PAGE_BLOCK_MANAGER:
    case WT_PAGE_OVFL:
        if (dsk->u.datalen == 0)
            WT_RET_VRFY(
              session, "%s page at %s has no data", __wt_page_type_string(dsk->type), tag);
        break;
    }
    switch (dsk->type) {
    case WT_PAGE_COL_INT:
        return (__verify_dsk_col_int(session, tag, dsk, addr));
    case WT_PAGE_COL_FIX:
        return (__verify_dsk_col_fix(session, tag, dsk));
    case WT_PAGE_COL_VAR:
        return (__verify_dsk_col_var(session, tag, dsk, addr));
    case WT_PAGE_ROW_INT:
    case WT_PAGE_ROW_LEAF:
        return (__verify_dsk_row(session, tag, dsk, addr));
    case WT_PAGE_BLOCK_MANAGER:
    case WT_PAGE_OVFL:
        return (__verify_dsk_chunk(session, tag, dsk, dsk->u.datalen));
    default:
        return (__wt_illegal_value(session, dsk->type));
    }
    /* NOTREACHED */
}

/*
 * __wt_verify_dsk --
 *     Verify a single Btree page as read from disk.
 */
int
__wt_verify_dsk(WT_SESSION_IMPL *session, const char *tag, WT_ITEM *buf)
{
    return (__wt_verify_dsk_image(session, tag, buf->data, buf->size, NULL, false));
}

/*
 * __verify_dsk_ts_addr_cmp --
 *     Do a cell timestamp check against the parent.
 */
static int
__verify_dsk_ts_addr_cmp(WT_SESSION_IMPL *session, uint32_t cell_num, const char *ts1_name,
  wt_timestamp_t ts1, const char *ts2_name, wt_timestamp_t ts2, bool gt, const char *tag)
{
    char ts_string[2][WT_TS_INT_STRING_SIZE];
    const char *ts1_bp, *ts2_bp;

    if (gt && ts1 >= ts2)
        return (0);
    if (!gt && ts1 <= ts2)
        return (0);

    switch (ts1) {
    case WT_TS_MAX:
        ts1_bp = "WT_TS_MAX";
        break;
    case WT_TS_NONE:
        ts1_bp = "WT_TS_NONE";
        break;
    default:
        ts1_bp = __wt_timestamp_to_string(ts1, ts_string[0]);
        break;
    }
    switch (ts2) {
    case WT_TS_MAX:
        ts2_bp = "WT_TS_MAX";
        break;
    case WT_TS_NONE:
        ts2_bp = "WT_TS_NONE";
        break;
    default:
        ts2_bp = __wt_timestamp_to_string(ts2, ts_string[1]);
        break;
    }
    WT_RET_MSG(session, WT_ERROR, "cell %" PRIu32
                                  " on page at %s failed verification with %s "
                                  "timestamp of %s, %s the parent's %s timestamp of %s",
      cell_num, tag, ts1_name, ts1_bp, gt ? "less than" : "greater than", ts2_name, ts2_bp);
}

/*
 * __verify_dsk_txn_addr_cmp --
 *     Do a cell transaction check against the parent.
 */
static int
__verify_dsk_txn_addr_cmp(WT_SESSION_IMPL *session, uint32_t cell_num, const char *txn1_name,
  uint64_t txn1, const char *txn2_name, uint64_t txn2, bool gt, const char *tag,
  const WT_PAGE_HEADER *dsk)
{
    if (gt && txn1 >= txn2)
        return (0);
    if (!gt && txn1 <= txn2)
        return (0);
    /*
     * If we unpack a value that was written as part of a previous startup generation, it may have a
     * later stop time pair than its parent.
     */
    if (dsk->write_gen <= S2C(session)->base_write_gen)
        return (0);

    WT_RET_MSG(session, WT_ERROR, "cell %" PRIu32
                                  " on page at %s failed verification with %s "
                                  "transaction of %" PRIu64
                                  ", %s the parent's %s transaction of "
                                  "%" PRIu64,
      cell_num, tag, txn1_name, txn1, gt ? "less than" : "greater than", txn2_name, txn2);
}

/*
 * __verify_dsk_validity --
 *     Verify a cell's validity window.
 */
static int
__verify_dsk_validity(WT_SESSION_IMPL *session, WT_CELL_UNPACK *unpack, uint32_t cell_num,
  WT_ADDR *addr, const char *tag, const WT_PAGE_HEADER *dsk)
{
    char ts_string[2][WT_TS_INT_STRING_SIZE];

    /*
     * Check timestamp and transaction order, and optionally against parent values. Timestamps and
     * transactions in the parent address aren't necessarily an exact match, but should be within
     * the boundaries of the parent's information.
     *
     * There's no checking if validity information should appear on a page because the
     * cell-unpacking code hides it by always returning durable values if they don't appear on the
     * page.
     */
    switch (unpack->type) {
    case WT_CELL_ADDR_DEL:
    case WT_CELL_ADDR_INT:
    case WT_CELL_ADDR_LEAF:
    case WT_CELL_ADDR_LEAF_NO:
        if (unpack->oldest_start_ts != WT_TS_NONE && unpack->newest_stop_ts == WT_TS_NONE)
            WT_RET_VRFY(session, "cell %" PRIu32
                                 " on page at %s has a newest stop "
                                 "timestamp of 0",
              cell_num - 1, tag);
        if (unpack->oldest_start_ts > unpack->newest_stop_ts)
            WT_RET_VRFY(session, "cell %" PRIu32
                                 " on page at %s has an oldest "
                                 "start timestamp %s newer than its newest stop "
                                 "timestamp %s",
              cell_num - 1, tag, __wt_timestamp_to_string(unpack->oldest_start_ts, ts_string[0]),
              __wt_timestamp_to_string(unpack->newest_stop_ts, ts_string[1]));
        if (unpack->oldest_start_txn > unpack->newest_stop_txn)
            WT_RET_VRFY(session, "cell %" PRIu32
                                 " on page at %s has an oldest "
                                 "start transaction %" PRIu64
                                 " newer than its "
                                 "newest stop transaction %" PRIu64,
              cell_num - 1, tag, unpack->oldest_start_txn, unpack->newest_stop_txn);

        if (addr == NULL)
            break;

        WT_RET(__verify_dsk_ts_addr_cmp(session, cell_num - 1, "start durable",
          unpack->newest_start_durable_ts, "start durable", addr->start_durable_ts, false, tag));
        WT_RET(__verify_dsk_ts_addr_cmp(session, cell_num - 1, "oldest start",
          unpack->oldest_start_ts, "oldest start", addr->oldest_start_ts, true, tag));
        WT_RET(__verify_dsk_txn_addr_cmp(session, cell_num - 1, "oldest start",
          unpack->oldest_start_txn, "oldest start", addr->oldest_start_txn, true, tag, dsk));
        WT_RET(__verify_dsk_ts_addr_cmp(session, cell_num - 1, "stop durable",
          unpack->newest_stop_durable_ts, "stop durable", addr->stop_durable_ts, false, tag));
        WT_RET(__verify_dsk_ts_addr_cmp(session, cell_num - 1, "newest stop",
          unpack->newest_stop_ts, "newest stop", addr->newest_stop_ts, false, tag));
        WT_RET(__verify_dsk_txn_addr_cmp(session, cell_num - 1, "newest stop",
          unpack->newest_stop_txn, "newest stop", addr->newest_stop_txn, false, tag, dsk));
        break;
    case WT_CELL_DEL:
    case WT_CELL_VALUE:
    case WT_CELL_VALUE_COPY:
    case WT_CELL_VALUE_OVFL:
    case WT_CELL_VALUE_OVFL_RM:
    case WT_CELL_VALUE_SHORT:
        if (unpack->start_ts != WT_TS_NONE && unpack->stop_ts == WT_TS_NONE)
            WT_RET_VRFY(session, "cell %" PRIu32
                                 " on page at %s has a stop "
                                 "timestamp of 0",
              cell_num - 1, tag);
        if (unpack->start_ts > unpack->stop_ts)
            WT_RET_VRFY(session, "cell %" PRIu32
                                 " on page at %s has a start "
                                 "timestamp %s newer than its stop timestamp %s",
              cell_num - 1, tag, __wt_timestamp_to_string(unpack->start_ts, ts_string[0]),
              __wt_timestamp_to_string(unpack->stop_ts, ts_string[1]));
        if (unpack->start_txn > unpack->stop_txn)
            WT_RET_VRFY(session, "cell %" PRIu32
                                 " on page at %s has a start "
                                 "transaction %" PRIu64
                                 " newer than its stop "
                                 "transaction %" PRIu64,
              cell_num - 1, tag, unpack->start_txn, unpack->stop_txn);

        if (addr == NULL)
            break;

        WT_RET(__verify_dsk_ts_addr_cmp(session, cell_num - 1, "start", unpack->start_ts,
          "oldest start", addr->oldest_start_ts, true, tag));
        WT_RET(__verify_dsk_txn_addr_cmp(session, cell_num - 1, "start", unpack->start_txn,
          "oldest start", addr->oldest_start_txn, true, tag, dsk));
        WT_RET(__verify_dsk_ts_addr_cmp(session, cell_num - 1, "stop", unpack->stop_ts,
          "newest stop", addr->newest_stop_ts, false, tag));
        WT_RET(__verify_dsk_txn_addr_cmp(session, cell_num - 1, "stop", unpack->stop_txn,
          "newest stop", addr->newest_stop_txn, false, tag, dsk));
        break;
    }

    return (0);
}

/*
 * __verify_dsk_row --
 *     Walk a WT_PAGE_ROW_INT or WT_PAGE_ROW_LEAF disk page and verify it.
 */
static int
__verify_dsk_row(
  WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk, WT_ADDR *addr)
{
    WT_BM *bm;
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK *unpack, _unpack;
    WT_DECL_ITEM(current);
    WT_DECL_ITEM(last_ovfl);
    WT_DECL_ITEM(last_pfx);
    WT_DECL_ITEM(tmp1);
    WT_DECL_ITEM(tmp2);
    WT_DECL_RET;
    WT_ITEM *last;
    enum { FIRST, WAS_KEY, WAS_VALUE } last_cell_type;
    void *huffman;
    size_t prefix;
    uint32_t cell_num, cell_type, i, key_cnt;
    uint8_t *end;
    int cmp;

    btree = S2BT(session);
    bm = btree->bm;
    unpack = &_unpack;
    huffman = dsk->type == WT_PAGE_ROW_INT ? NULL : btree->huffman_key;

    WT_ERR(__wt_scr_alloc(session, 0, &current));
    WT_ERR(__wt_scr_alloc(session, 0, &last_pfx));
    WT_ERR(__wt_scr_alloc(session, 0, &last_ovfl));
    WT_ERR(__wt_scr_alloc(session, 0, &tmp1));
    WT_ERR(__wt_scr_alloc(session, 0, &tmp2));
    last = last_ovfl;

    end = (uint8_t *)dsk + dsk->mem_size;

    last_cell_type = FIRST;
    cell_num = 0;
    key_cnt = 0;
    WT_CELL_FOREACH_VRFY (btree, dsk, cell, unpack, i) {
        ++cell_num;

        /* Carefully unpack the cell. */
        ret = __wt_cell_unpack_safe(session, dsk, cell, unpack, end);
        if (ret != 0) {
            (void)__err_cell_corrupt(session, ret, cell_num, tag);
            goto err;
        }

        /* Check the raw and collapsed cell types. */
        WT_ERR(__err_cell_type(session, cell_num, tag, unpack->raw, dsk->type));
        WT_ERR(__err_cell_type(session, cell_num, tag, unpack->type, dsk->type));
        cell_type = unpack->type;

        /*
         * Check ordering relationships between the WT_CELL entries.
         * For row-store internal pages, check for:
         *	two values in a row,
         *	two keys in a row,
         *	a value as the first cell on a page.
         * For row-store leaf pages, check for:
         *	two values in a row,
         *	a value as the first cell on a page.
         */
        switch (cell_type) {
        case WT_CELL_KEY:
        case WT_CELL_KEY_OVFL:
            ++key_cnt;
            switch (last_cell_type) {
            case FIRST:
            case WAS_VALUE:
                break;
            case WAS_KEY:
                if (dsk->type == WT_PAGE_ROW_LEAF)
                    break;
                WT_ERR_VRFY(session, "cell %" PRIu32
                                     " on page at %s is the "
                                     "first of two adjacent keys",
                  cell_num - 1, tag);
            }
            last_cell_type = WAS_KEY;
            break;
        case WT_CELL_ADDR_DEL:
        case WT_CELL_ADDR_INT:
        case WT_CELL_ADDR_LEAF:
        case WT_CELL_ADDR_LEAF_NO:
        case WT_CELL_VALUE:
        case WT_CELL_VALUE_OVFL:
            switch (last_cell_type) {
            case FIRST:
                WT_ERR_VRFY(session, "page at %s begins with a value", tag);
            case WAS_KEY:
                break;
            case WAS_VALUE:
                WT_ERR_VRFY(session, "cell %" PRIu32
                                     " on page at %s is the "
                                     "first of two adjacent values",
                  cell_num - 1, tag);
            }
            last_cell_type = WAS_VALUE;
            break;
        }

        /* Check the validity window. */
        WT_ERR(__verify_dsk_validity(session, unpack, cell_num, addr, tag, dsk));

        /* Check if any referenced item has an invalid address. */
        switch (cell_type) {
        case WT_CELL_ADDR_DEL:
        case WT_CELL_ADDR_INT:
        case WT_CELL_ADDR_LEAF:
        case WT_CELL_ADDR_LEAF_NO:
        case WT_CELL_KEY_OVFL:
        case WT_CELL_VALUE_OVFL:
            if ((ret = bm->addr_invalid(bm, session, unpack->data, unpack->size)) == EINVAL)
                (void)__err_cell_corrupt_or_eof(session, ret, cell_num, tag);
            WT_ERR(ret);
            break;
        }

        /*
         * Remaining checks are for key order and prefix compression. If this cell isn't a key,
         * we're done, move to the next cell. If this cell is an overflow item, instantiate the key
         * and compare it with the last key. Otherwise, we have to deal with prefix compression.
         */
        switch (cell_type) {
        case WT_CELL_KEY:
            break;
        case WT_CELL_KEY_OVFL:
            WT_ERR(__wt_dsk_cell_data_ref(session, dsk->type, unpack, current));
            goto key_compare;
        default:
            /* Not a key -- continue with the next cell. */
            continue;
        }

        /*
         * Prefix compression checks.
         *
         * Confirm the first non-overflow key on a page has a zero prefix compression count.
         */
        prefix = unpack->prefix;
        if (last_pfx->size == 0 && prefix != 0)
            WT_ERR_VRFY(session, "the %" PRIu32
                                 " key on page at %s is the first "
                                 "non-overflow key on the page and has a non-zero "
                                 "prefix compression value",
              cell_num, tag);

        /* Confirm the prefix compression count is possible. */
        if (cell_num > 1 && prefix > last->size)
            WT_ERR_VRFY(session, "key %" PRIu32
                                 " on page at %s has a prefix "
                                 "compression count of %" WT_SIZET_FMT
                                 ", larger than the length of the previous key, %" WT_SIZET_FMT,
              cell_num, tag, prefix, last->size);

        /*
         * If Huffman decoding required, unpack the cell to build the key, then resolve the prefix.
         * Else, we can do it faster internally because we don't have to shuffle memory around as
         * much.
         */
        if (huffman != NULL) {
            WT_ERR(__wt_dsk_cell_data_ref(session, dsk->type, unpack, current));

            /*
             * If there's a prefix, make sure there's enough buffer space, then shift the decoded
             * data past the prefix and copy the prefix into place. Take care with the pointers:
             * current->data may be pointing inside the buffer.
             */
            if (prefix != 0) {
                WT_ERR(__wt_buf_grow(session, current, prefix + current->size));
                memmove((uint8_t *)current->mem + prefix, current->data, current->size);
                memcpy(current->mem, last->data, prefix);
                current->data = current->mem;
                current->size += prefix;
            }
        } else {
            /*
             * Get the cell's data/length and make sure we have enough buffer space.
             */
            WT_ERR(__wt_buf_init(session, current, prefix + unpack->size));

            /* Copy the prefix then the data into place. */
            if (prefix != 0)
                memcpy(current->mem, last->data, prefix);
            memcpy((uint8_t *)current->mem + prefix, unpack->data, unpack->size);
            current->size = prefix + unpack->size;
        }

key_compare:
        /*
         * Compare the current key against the last key.
         *
         * Be careful about the 0th key on internal pages: we only store the first byte and custom
         * collators may not be able to handle truncated keys.
         */
        if ((dsk->type == WT_PAGE_ROW_INT && cell_num > 3) ||
          (dsk->type != WT_PAGE_ROW_INT && cell_num > 1)) {
            WT_ERR(__wt_compare(session, btree->collator, last, current, &cmp));
            if (cmp >= 0)
                WT_ERR_VRFY(session, "the %" PRIu32 " and %" PRIu32
                                     " keys on "
                                     "page at %s are incorrectly sorted: %s, %s",
                  cell_num - 2, cell_num, tag,
                  __wt_buf_set_printable(session, last->data, last->size, tmp1),
                  __wt_buf_set_printable(session, current->data, current->size, tmp2));
        }

        /*
         * Swap the buffers: last always references the last key entry, last_pfx and last_ovfl
         * reference the last prefix-compressed and last overflow key entries. Current gets pointed
         * to the buffer we're not using this time around, which is where the next key goes.
         */
        last = current;
        if (cell_type == WT_CELL_KEY) {
            current = last_pfx;
            last_pfx = last;
        } else {
            current = last_ovfl;
            last_ovfl = last;
        }
        WT_ASSERT(session, last != current);
    }
    WT_ERR(__verify_dsk_memsize(session, tag, dsk, cell));

    /*
     * On row-store internal pages, and on row-store leaf pages, where the
     * "no empty values" flag is set, the key count should be equal to half
     * the number of physical entries.  On row-store leaf pages where the
     * "all empty values" flag is set, the key count should be equal to the
     * number of physical entries.
     */
    if (dsk->type == WT_PAGE_ROW_INT && key_cnt * 2 != dsk->u.entries)
        WT_ERR_VRFY(session, "%s page at %s has a key count of %" PRIu32
                             " and a "
                             "physical entry count of %" PRIu32,
          __wt_page_type_string(dsk->type), tag, key_cnt, dsk->u.entries);
    if (dsk->type == WT_PAGE_ROW_LEAF && F_ISSET(dsk, WT_PAGE_EMPTY_V_ALL) &&
      key_cnt != dsk->u.entries)
        WT_ERR_VRFY(session,
          "%s page at %s with the 'all empty values' flag set has a "
          "key count of %" PRIu32 " and a physical entry count of %" PRIu32,
          __wt_page_type_string(dsk->type), tag, key_cnt, dsk->u.entries);
    if (dsk->type == WT_PAGE_ROW_LEAF && F_ISSET(dsk, WT_PAGE_EMPTY_V_NONE) &&
      key_cnt * 2 != dsk->u.entries)
        WT_ERR_VRFY(session,
          "%s page at %s with the 'no empty values' flag set has a "
          "key count of %" PRIu32 " and a physical entry count of %" PRIu32,
          __wt_page_type_string(dsk->type), tag, key_cnt, dsk->u.entries);

    if (0) {
err:
        if (ret == 0)
            ret = WT_ERROR;
    }
    __wt_scr_free(session, &current);
    __wt_scr_free(session, &last_pfx);
    __wt_scr_free(session, &last_ovfl);
    __wt_scr_free(session, &tmp1);
    __wt_scr_free(session, &tmp2);
    return (ret);
}

/*
 * __verify_dsk_col_int --
 *     Walk a WT_PAGE_COL_INT disk page and verify it.
 */
static int
__verify_dsk_col_int(
  WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk, WT_ADDR *addr)
{
    WT_BM *bm;
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK *unpack, _unpack;
    WT_DECL_RET;
    uint32_t cell_num, i;
    uint8_t *end;

    btree = S2BT(session);
    bm = btree->bm;
    unpack = &_unpack;
    end = (uint8_t *)dsk + dsk->mem_size;

    cell_num = 0;
    WT_CELL_FOREACH_VRFY (btree, dsk, cell, unpack, i) {
        ++cell_num;

        /* Carefully unpack the cell. */
        ret = __wt_cell_unpack_safe(session, dsk, cell, unpack, end);
        if (ret != 0)
            return (__err_cell_corrupt(session, ret, cell_num, tag));

        /* Check the raw and collapsed cell types. */
        WT_RET(__err_cell_type(session, cell_num, tag, unpack->raw, dsk->type));
        WT_RET(__err_cell_type(session, cell_num, tag, unpack->type, dsk->type));

        /* Check the validity window. */
        WT_RET(__verify_dsk_validity(session, unpack, cell_num, addr, tag, dsk));

        /* Check if any referenced item is entirely in the file. */
        ret = bm->addr_invalid(bm, session, unpack->data, unpack->size);
        WT_RET_ERROR_OK(ret, EINVAL);
        if (ret == EINVAL)
            return (__err_cell_corrupt_or_eof(session, ret, cell_num, tag));
    }
    WT_RET(__verify_dsk_memsize(session, tag, dsk, cell));

    return (0);
}

/*
 * __verify_dsk_col_fix --
 *     Walk a WT_PAGE_COL_FIX disk page and verify it.
 */
static int
__verify_dsk_col_fix(WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk)
{
    WT_BTREE *btree;
    uint32_t datalen;

    btree = S2BT(session);

    datalen = __bitstr_size(btree->bitcnt * dsk->u.entries);
    return (__verify_dsk_chunk(session, tag, dsk, datalen));
}

/*
 * __verify_dsk_col_var --
 *     Walk a WT_PAGE_COL_VAR disk page and verify it.
 */
static int
__verify_dsk_col_var(
  WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk, WT_ADDR *addr)
{
    struct {
        const void *data;
        size_t size;
        wt_timestamp_t start_ts;
        uint64_t start_txn;
        wt_timestamp_t stop_ts;
        uint64_t stop_txn;
        bool deleted;
    } last;
    WT_BM *bm;
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK *unpack, _unpack;
    WT_DECL_RET;
    uint32_t cell_num, cell_type, i;
    uint8_t *end;

    btree = S2BT(session);
    bm = btree->bm;
    unpack = &_unpack;
    end = (uint8_t *)dsk + dsk->mem_size;

    last.data = NULL;
    last.size = 0;
    last.start_ts = WT_TS_NONE;
    last.start_txn = WT_TXN_NONE;
    last.stop_ts = WT_TS_NONE;
    last.stop_txn = WT_TXN_NONE;
    last.deleted = false;

    cell_num = 0;
    WT_CELL_FOREACH_VRFY (btree, dsk, cell, unpack, i) {
        ++cell_num;

        /* Carefully unpack the cell. */
        ret = __wt_cell_unpack_safe(session, dsk, cell, unpack, end);
        if (ret != 0)
            return (__err_cell_corrupt(session, ret, cell_num, tag));

        /* Check the raw and collapsed cell types. */
        WT_RET(__err_cell_type(session, cell_num, tag, unpack->raw, dsk->type));
        WT_RET(__err_cell_type(session, cell_num, tag, unpack->type, dsk->type));
        cell_type = unpack->type;

        /* Check the validity window. */
        WT_RET(__verify_dsk_validity(session, unpack, cell_num, addr, tag, dsk));

        /* Check if any referenced item is entirely in the file. */
        if (cell_type == WT_CELL_VALUE_OVFL) {
            ret = bm->addr_invalid(bm, session, unpack->data, unpack->size);
            WT_RET_ERROR_OK(ret, EINVAL);
            if (ret == EINVAL)
                return (__err_cell_corrupt_or_eof(session, ret, cell_num, tag));
        }

        /*
         * Compare the last two items and see if reconciliation missed a chance for RLE encoding. We
         * don't have to care about data encoding or anything else, a byte comparison is enough.
         */
        if (unpack->start_ts != last.start_ts || unpack->start_txn != last.start_txn ||
          unpack->stop_ts != last.stop_ts || unpack->stop_txn != last.stop_txn)
            ;
        else if (last.deleted) {
            if (cell_type == WT_CELL_DEL)
                goto match_err;
        } else if (cell_type == WT_CELL_VALUE && last.data != NULL && last.size == unpack->size &&
          memcmp(last.data, unpack->data, last.size) == 0)
match_err:
        WT_RET_VRFY(session, "data entries %" PRIu32 " and %" PRIu32
                             " on page at %s are identical and should "
                             "have been run-length encoded",
          cell_num - 1, cell_num, tag);

        last.start_ts = unpack->start_ts;
        last.start_txn = unpack->start_txn;
        last.stop_ts = unpack->stop_ts;
        last.stop_txn = unpack->stop_txn;
        switch (cell_type) {
        case WT_CELL_DEL:
            last.data = NULL;
            last.deleted = true;
            break;
        case WT_CELL_VALUE_OVFL:
            last.data = NULL;
            last.deleted = false;
            break;
        case WT_CELL_VALUE:
            last.data = unpack->data;
            last.size = unpack->size;
            last.deleted = false;
            break;
        }
    }
    WT_RET(__verify_dsk_memsize(session, tag, dsk, cell));

    return (0);
}

/*
 * __verify_dsk_memsize --
 *     Verify the last cell on the page matches the page's memory size.
 */
static int
__verify_dsk_memsize(
  WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk, WT_CELL *cell)
{
    size_t len;

    /*
     * We use the fact that cells exactly fill a page to detect the case of a row-store leaf page
     * where the last cell is a key (that is, there's no subsequent value cell). Check for any page
     * type containing cells.
     */
    len = WT_PTRDIFF((uint8_t *)dsk + dsk->mem_size, cell);
    if (len == 0)
        return (0);
    WT_RET_VRFY(session, "%s page at %s has %" WT_SIZET_FMT
                         " unexpected bytes of data "
                         "after the last cell",
      __wt_page_type_string(dsk->type), tag, len);
}

/*
 * __verify_dsk_chunk --
 *     Verify a Chunk O' Data on a Btree page.
 */
static int
__verify_dsk_chunk(
  WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk, uint32_t datalen)
{
    WT_BTREE *btree;
    uint8_t *p, *end;

    btree = S2BT(session);
    end = (uint8_t *)dsk + dsk->mem_size;

    /*
     * Fixed-length column-store and overflow pages are simple chunks of data. Verify the data
     * doesn't overflow the end of the page.
     */
    p = WT_PAGE_HEADER_BYTE(btree, dsk);
    if (p + datalen > end)
        WT_RET_VRFY(session, "data on page at %s extends past the end of the page", tag);

    /* Any bytes after the data chunk should be nul bytes. */
    for (p += datalen; p < end; ++p)
        if (*p != '\0')
            WT_RET_VRFY(session, "%s page at %s has non-zero trailing bytes",
              __wt_page_type_string(dsk->type), tag);

    return (0);
}

/*
 * __err_cell_corrupt --
 *     Generic corrupted cell, we couldn't read it.
 */
static int
__err_cell_corrupt(WT_SESSION_IMPL *session, int retval, uint32_t entry_num, const char *tag)
{
    WT_RET_VRFY_RETVAL(
      session, retval, "item %" PRIu32 " on page at %s is a corrupted cell", entry_num, tag);
}

/*
 * __err_cell_corrupt_or_eof --
 *     Generic corrupted cell or item references non-existent file pages error.
 */
static int
__err_cell_corrupt_or_eof(WT_SESSION_IMPL *session, int retval, uint32_t entry_num, const char *tag)
{
    WT_RET_VRFY_RETVAL(session, retval, "item %" PRIu32
                                        " on page at %s is a corrupted cell or references "
                                        "non-existent file pages",
      entry_num, tag);
}

/*
 * __err_cell_type --
 *     Generic illegal cell type for a particular page type error.
 */
static int
__err_cell_type(WT_SESSION_IMPL *session, uint32_t entry_num, const char *tag, uint8_t cell_type,
  uint8_t dsk_type)
{
    switch (cell_type) {
    case WT_CELL_ADDR_DEL:
    case WT_CELL_ADDR_INT:
    case WT_CELL_ADDR_LEAF:
    case WT_CELL_ADDR_LEAF_NO:
        if (dsk_type == WT_PAGE_COL_INT || dsk_type == WT_PAGE_ROW_INT)
            return (0);
        break;
    case WT_CELL_DEL:
        if (dsk_type == WT_PAGE_COL_VAR)
            return (0);
        break;
    case WT_CELL_KEY:
    case WT_CELL_KEY_OVFL:
    case WT_CELL_KEY_SHORT:
        if (dsk_type == WT_PAGE_ROW_INT || dsk_type == WT_PAGE_ROW_LEAF)
            return (0);
        break;
    case WT_CELL_KEY_PFX:
    case WT_CELL_KEY_SHORT_PFX:
        if (dsk_type == WT_PAGE_ROW_LEAF)
            return (0);
        break;
    case WT_CELL_KEY_OVFL_RM:
    case WT_CELL_VALUE_OVFL_RM:
        /*
         * Removed overflow cells are in-memory only, it's an error to ever see one on a disk page.
         */
        break;
    case WT_CELL_VALUE:
    case WT_CELL_VALUE_COPY:
    case WT_CELL_VALUE_OVFL:
    case WT_CELL_VALUE_SHORT:
        if (dsk_type == WT_PAGE_COL_VAR || dsk_type == WT_PAGE_ROW_LEAF)
            return (0);
        break;
    default:
        break;
    }

    WT_RET_VRFY(session, "illegal cell and page type combination: cell %" PRIu32
                         " on page at %s is a %s cell on a %s page",
      entry_num, tag, __wt_cell_type_string(cell_type), __wt_page_type_string(dsk_type));
}
