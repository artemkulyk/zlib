/* infcover.c -- test zlib's inflate routines with full code coverage
 * Copyright (C) 2011, 2016, 2024, 2026 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* to use, do: ./configure --cover && make cover */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "zlib.h"

/* get definition of internal structure so we can mess with it (see pull()),
   and so we can call inflate_trees() (see cover5()) */
#define ZLIB_INTERNAL
#include "inftrees.h"
#include "inflate.h"

#define local static

/* -- memory tracking routines -- */

/*
   These memory tracking routines are provided to zlib and track all of zlib's
   allocations and deallocations, check for LIFO operations, keep a current
   and high water mark of total bytes requested, optionally set a limit on the
   total memory that can be allocated, and when done check for memory leaks.

   They are used as follows:

   z_stream strm;
   mem_setup(&strm)         initializes the memory tracking and sets the
                            zalloc, zfree, and opaque members of strm to use
                            memory tracking for all zlib operations on strm
   mem_limit(&strm, limit)  sets a limit on the total bytes requested -- a
                            request that exceeds this limit will result in an
                            allocation failure (returns NULL) -- setting the
                            limit to zero means no limit, which is the default
                            after mem_setup()
   mem_used(&strm, "msg")   prints to stderr "msg" and the total bytes used
   mem_high(&strm, "msg")   prints to stderr "msg" and the high water mark
   mem_done(&strm, "msg")   ends memory tracking, releases all allocations
                            for the tracking as well as leaked zlib blocks, if
                            any.  If there was anything unusual, such as leaked
                            blocks, non-FIFO frees, or frees of addresses not
                            allocated, then "msg" and information about the
                            problem is printed to stderr.  If everything is
                            normal, nothing is printed. mem_done resets the
                            strm members to Z_NULL to use the default memory
                            allocation routines on the next zlib initialization
                            using strm.
 */

/* these items are strung together in a linked list, one for each allocation */
struct mem_item {
    void *ptr;                  /* pointer to allocated memory */
    size_t size;                /* requested size of allocation */
    struct mem_item *next;      /* pointer to next item in list, or NULL */
};

/* this structure is at the root of the linked list, and tracks statistics */
struct mem_zone {
    struct mem_item *first;     /* pointer to first item in list, or NULL */
    size_t total, highwater;    /* total allocations, and largest total */
    size_t limit;               /* memory allocation limit, or 0 if no limit */
    int notlifo, rogue;         /* counts of non-LIFO frees and rogue frees */
};

/* memory allocation routine to pass to zlib */
local void *mem_alloc(void *mem, unsigned count, unsigned size)
{
    void *ptr;
    struct mem_item *item;
    struct mem_zone *zone = mem;
    size_t len = count * (size_t)size;

    /* induced allocation failure */
    if (zone == NULL || (zone->limit && zone->total + len > zone->limit))
        return NULL;

    /* perform allocation using the standard library, fill memory with a
       non-zero value to make sure that the code isn't depending on zeros */
    ptr = malloc(len);
    if (ptr == NULL)
        return NULL;
    memset(ptr, 0xa5, len);

    /* create a new item for the list */
    item = malloc(sizeof(struct mem_item));
    if (item == NULL) {
        free(ptr);
        return NULL;
    }
    item->ptr = ptr;
    item->size = len;

    /* insert item at the beginning of the list */
    item->next = zone->first;
    zone->first = item;

    /* update the statistics */
    zone->total += item->size;
    if (zone->total > zone->highwater)
        zone->highwater = zone->total;

    /* return the allocated memory */
    return ptr;
}

/* memory free routine to pass to zlib */
local void mem_free(void *mem, void *ptr)
{
    struct mem_item *item, *next;
    struct mem_zone *zone = mem;

    /* if no zone, just do a free */
    if (zone == NULL) {
        free(ptr);
        return;
    }

    /* point next to the item that matches ptr, or NULL if not found -- remove
       the item from the linked list if found */
    next = zone->first;
    if (next) {
        if (next->ptr == ptr)
            zone->first = next->next;   /* first one is it, remove from list */
        else {
            do {                        /* search the linked list */
                item = next;
                next = item->next;
            } while (next != NULL && next->ptr != ptr);
            if (next) {                 /* if found, remove from linked list */
                item->next = next->next;
                zone->notlifo++;        /* not a LIFO free */
            }

        }
    }

    /* if found, update the statistics and free the item */
    if (next) {
        zone->total -= next->size;
        free(next);
    }

    /* if not found, update the rogue count */
    else
        zone->rogue++;

    /* in any case, do the requested free with the standard library function */
    free(ptr);
}

/* set up a controlled memory allocation space for monitoring, set the stream
   parameters to the controlled routines, with opaque pointing to the space */
local void mem_setup(z_stream *strm)
{
    struct mem_zone *zone;

    zone = malloc(sizeof(struct mem_zone));
    assert(zone != NULL);
    zone->first = NULL;
    zone->total = 0;
    zone->highwater = 0;
    zone->limit = 0;
    zone->notlifo = 0;
    zone->rogue = 0;
    strm->opaque = zone;
    strm->zalloc = mem_alloc;
    strm->zfree = mem_free;
}

/* set a limit on the total memory allocation, or 0 to remove the limit */
local void mem_limit(z_stream *strm, size_t limit)
{
    struct mem_zone *zone = strm->opaque;

    zone->limit = limit;
}

/* show the current total requested allocations in bytes */
local void mem_used(z_stream *strm, char *prefix)
{
    struct mem_zone *zone = strm->opaque;

    fprintf(stderr, "%s: %zu allocated\n", prefix, zone->total);
}

/* show the high water allocation in bytes */
local void mem_high(z_stream *strm, char *prefix)
{
    struct mem_zone *zone = strm->opaque;

    fprintf(stderr, "%s: %zu high water mark\n", prefix, zone->highwater);
}

/* release the memory allocation zone -- if there are any surprises, notify */
local void mem_done(z_stream *strm, char *prefix)
{
    int count = 0;
    struct mem_item *item, *next;
    struct mem_zone *zone = strm->opaque;

    /* show high water mark */
    mem_high(strm, prefix);

    /* free leftover allocations and item structures, if any */
    item = zone->first;
    while (item != NULL) {
        free(item->ptr);
        next = item->next;
        free(item);
        item = next;
        count++;
    }

    /* issue alerts about anything unexpected */
    if (count || zone->total)
        fprintf(stderr, "** %s: %zu bytes in %d blocks not freed\n",
                prefix, zone->total, count);
    if (zone->notlifo)
        fprintf(stderr, "** %s: %d frees not LIFO\n", prefix, zone->notlifo);
    if (zone->rogue)
        fprintf(stderr, "** %s: %d frees not recognized\n",
                prefix, zone->rogue);

    /* free the zone and delete from the stream */
    free(zone);
    strm->opaque = Z_NULL;
    strm->zalloc = Z_NULL;
    strm->zfree = Z_NULL;
}

/* -- inflate test routines -- */

/* Decode a hexadecimal string, set *len to length, in[] to the bytes.  This
   decodes liberally, in that hex digits can be adjacent, in which case two in
   a row writes a byte.  Or they can be delimited by any non-hex character,
   where the delimiters are ignored except when a single hex digit is followed
   by a delimiter, where that single digit writes a byte.  The returned data is
   allocated and must eventually be freed.  NULL is returned if out of memory.
   If the length is not needed, then len can be NULL. */
local unsigned char *h2b(const char *hex, unsigned *len)
{
    unsigned char *in, *re;
    unsigned next, val;

    in = malloc((strlen(hex) + 1) >> 1);
    if (in == NULL)
        return NULL;
    next = 0;
    val = 1;
    do {
        if (*hex >= '0' && *hex <= '9')
            val = (val << 4) + *hex - '0';
        else if (*hex >= 'A' && *hex <= 'F')
            val = (val << 4) + *hex - 'A' + 10;
        else if (*hex >= 'a' && *hex <= 'f')
            val = (val << 4) + *hex - 'a' + 10;
        else if (val != 1 && val < 32)  /* one digit followed by delimiter */
            val += 240;                 /* make it look like two digits */
        if (val > 255) {                /* have two digits */
            in[next++] = val & 0xff;    /* save the decoded byte */
            val = 1;                    /* start over */
        }
    } while (*hex++);       /* go through the loop with the terminating null */
    if (len != NULL)
        *len = next;
    re = realloc(in, next);
    return re == NULL ? in : re;
}

/* generic inflate() run, where hex is the hexadecimal input data, what is the
   text to include in an error message, step is how much input data to feed
   inflate() on each call, or zero to feed it all, win is the window bits
   parameter to inflateInit2(), len is the size of the output buffer, and err
   is the error code expected from the first inflate() call (the second
   inflate() call is expected to return Z_STREAM_END).  If win is 47, then
   header information is collected with inflateGetHeader().  If a zlib stream
   is looking for a dictionary, then an empty dictionary is provided.
   inflate() is run until all of the input data is consumed. */
local void inf(char *hex, char *what, unsigned step, int win, unsigned len,
               int err)
{
    int ret;
    unsigned have;
    unsigned char *in, *out;
    z_stream strm, copy;
    gz_header head;

    mem_setup(&strm);
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit2(&strm, win);
    if (ret != Z_OK) {
        mem_done(&strm, what);
        return;
    }
    out = malloc(len);                          assert(out != NULL);
    if (win == 47) {
        head.extra = out;
        head.extra_max = len;
        head.name = out;
        head.name_max = len;
        head.comment = out;
        head.comm_max = len;
        ret = inflateGetHeader(&strm, &head);   assert(ret == Z_OK);
    }
    in = h2b(hex, &have);                       assert(in != NULL);
    if (step == 0 || step > have)
        step = have;
    strm.avail_in = step;
    have -= step;
    strm.next_in = in;
    do {
        strm.avail_out = len;
        strm.next_out = out;
        ret = inflate(&strm, Z_NO_FLUSH);       assert(err == 9 || ret == err);
        if (ret != Z_OK && ret != Z_BUF_ERROR && ret != Z_NEED_DICT)
            break;
        if (ret == Z_NEED_DICT) {
            ret = inflateSetDictionary(&strm, in, 1);
                                                assert(ret == Z_DATA_ERROR);
            mem_limit(&strm, 1);
            ret = inflateSetDictionary(&strm, out, 0);
                                                assert(ret == Z_MEM_ERROR);
            mem_limit(&strm, 0);
            ((struct inflate_state *)strm.state)->mode = DICT;
            ret = inflateSetDictionary(&strm, out, 0);
                                                assert(ret == Z_OK);
            ret = inflate(&strm, Z_NO_FLUSH);   assert(ret == Z_BUF_ERROR);
        }
        ret = inflateCopy(&copy, &strm);        assert(ret == Z_OK);
        ret = inflateEnd(&copy);                assert(ret == Z_OK);
        err = 9;                        /* don't care next time around */
        have += strm.avail_in;
        strm.avail_in = step > have ? have : step;
        have -= strm.avail_in;
    } while (strm.avail_in);
    free(in);
    free(out);
    ret = inflateReset2(&strm, -8);             assert(ret == Z_OK);
    ret = inflateEnd(&strm);                    assert(ret == Z_OK);
    mem_done(&strm, what);
}

/* cover all of the lines in inflate.c up to inflate() */
local void cover_support(void)
{
    int ret;
    z_stream strm;

    mem_setup(&strm);
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit(&strm);                   assert(ret == Z_OK);
    mem_used(&strm, "inflate init");
    ret = inflatePrime(&strm, 5, 31);           assert(ret == Z_OK);
    ret = inflatePrime(&strm, -1, 0);           assert(ret == Z_OK);
    ret = inflateSetDictionary(&strm, Z_NULL, 0);
                                                assert(ret == Z_STREAM_ERROR);
    ret = inflateEnd(&strm);                    assert(ret == Z_OK);
    mem_done(&strm, "prime");

    inf("63 0", "force window allocation", 0, -15, 1, Z_OK);
    inf("63 18 5", "force window replacement", 0, -8, 259, Z_OK);
    inf("63 18 68 30 d0 0 0", "force split window update", 4, -8, 259, Z_OK);
    inf("3 0", "use fixed blocks", 0, -15, 1, Z_STREAM_END);
    inf("", "bad window size", 0, 1, 0, Z_STREAM_ERROR);

    mem_setup(&strm);
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit_(&strm, "!", (int)sizeof(z_stream));
                                                assert(ret == Z_VERSION_ERROR);
    mem_done(&strm, "wrong version");

    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit(&strm);                   assert(ret == Z_OK);
    ret = inflateEnd(&strm);                    assert(ret == Z_OK);
    fputs("inflate built-in memory routines\n", stderr);
}

/* cover all inflate() header and trailer cases and code after inflate() */
local void cover_wrap(void)
{
    int ret;
    z_stream strm, copy;
    unsigned char dict[257];

    ret = inflate(Z_NULL, 0);                   assert(ret == Z_STREAM_ERROR);
    ret = inflateEnd(Z_NULL);                   assert(ret == Z_STREAM_ERROR);
    ret = inflateCopy(Z_NULL, Z_NULL);          assert(ret == Z_STREAM_ERROR);
    fputs("inflate bad parameters\n", stderr);

    inf("1f 8b 0 0", "bad gzip method", 0, 31, 0, Z_DATA_ERROR);
    inf("1f 8b 8 80", "bad gzip flags", 0, 31, 0, Z_DATA_ERROR);
    inf("77 85", "bad zlib method", 0, 15, 0, Z_DATA_ERROR);
    inf("8 99", "set window size from header", 0, 0, 0, Z_OK);
    inf("78 9c", "bad zlib window size", 0, 8, 0, Z_DATA_ERROR);
    inf("78 9c 63 0 0 0 1 0 1", "check adler32", 0, 15, 1, Z_STREAM_END);
    inf("1f 8b 8 1e 0 0 0 0 0 0 1 0 0 0 0 0 0", "bad header crc", 0, 47, 1,
        Z_DATA_ERROR);
    inf("1f 8b 8 2 0 0 0 0 0 0 1d 26 3 0 0 0 0 0 0 0 0 0", "check gzip length",
        0, 47, 0, Z_STREAM_END);
    inf("78 90", "bad zlib header check", 0, 47, 0, Z_DATA_ERROR);
    inf("8 b8 0 0 0 1", "need dictionary", 0, 8, 0, Z_NEED_DICT);
    inf("78 9c 63 0", "compute adler32", 0, 15, 1, Z_OK);

    mem_setup(&strm);
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit2(&strm, -8);
    strm.avail_in = 2;
    strm.next_in = (void *)"\x63";
    strm.avail_out = 1;
    strm.next_out = (void *)&ret;
    mem_limit(&strm, 1);
    ret = inflate(&strm, Z_NO_FLUSH);           assert(ret == Z_MEM_ERROR);
    ret = inflate(&strm, Z_NO_FLUSH);           assert(ret == Z_MEM_ERROR);
    mem_limit(&strm, 0);
    memset(dict, 0, 257);
    ret = inflateSetDictionary(&strm, dict, 257);
                                                assert(ret == Z_OK);
    mem_limit(&strm, (sizeof(struct inflate_state) << 1) + 256);
    ret = inflatePrime(&strm, 16, 0);           assert(ret == Z_OK);
    strm.avail_in = 2;
    strm.next_in = (void *)"\x80";
    ret = inflateSync(&strm);                   assert(ret == Z_DATA_ERROR);
    ret = inflate(&strm, Z_NO_FLUSH);           assert(ret == Z_STREAM_ERROR);
    strm.avail_in = 4;
    strm.next_in = (void *)"\0\0\xff\xff";
    ret = inflateSync(&strm);                   assert(ret == Z_OK);
    (void)inflateSyncPoint(&strm);
    ret = inflateCopy(&copy, &strm);            assert(ret == Z_MEM_ERROR);
    mem_limit(&strm, 0);
    ret = inflateUndermine(&strm, 1);           assert(ret == Z_DATA_ERROR);
    (void)inflateMark(&strm);
    ret = inflateEnd(&strm);                    assert(ret == Z_OK);
    mem_done(&strm, "miscellaneous, force memory errors");
}

/* input and output functions for inflateBack() */
local unsigned pull(void *desc, unsigned char z_const **buf)
{
    static unsigned int next = 0;
    static unsigned char dat[] = {0x63, 0, 2, 0};
    struct inflate_state *state;

    if (desc == Z_NULL) {
        next = 0;
        return 0;   /* no input (already provided at next_in) */
    }
    state = (void *)((z_stream *)desc)->state;
    if (state != Z_NULL)
        state->mode = SYNC;     /* force an otherwise impossible situation */
    return next < sizeof(dat) ? (*buf = dat + next++, 1) : 0;
}

local int push(void *desc, unsigned char *buf, unsigned len)
{
    (void)buf;
    (void)len;
    return desc != Z_NULL;      /* force error if desc not null */
}

/* cover inflateBack() up to common deflate data cases and after those */
local void cover_back(void)
{
    int ret;
    z_stream strm;
    unsigned char win[32768];

    ret = inflateBackInit_(Z_NULL, 0, win, 0, 0);
                                                assert(ret == Z_VERSION_ERROR);
    ret = inflateBackInit(Z_NULL, 0, win);      assert(ret == Z_STREAM_ERROR);
    ret = inflateBack(Z_NULL, Z_NULL, Z_NULL, Z_NULL, Z_NULL);
                                                assert(ret == Z_STREAM_ERROR);
    ret = inflateBackEnd(Z_NULL);               assert(ret == Z_STREAM_ERROR);
    fputs("inflateBack bad parameters\n", stderr);

    mem_setup(&strm);
    ret = inflateBackInit(&strm, 15, win);      assert(ret == Z_OK);
    strm.avail_in = 2;
    strm.next_in = (void *)"\x03";
    ret = inflateBack(&strm, pull, Z_NULL, push, Z_NULL);
                                                assert(ret == Z_STREAM_END);
        /* force output error */
    strm.avail_in = 3;
    strm.next_in = (void *)"\x63\x00";
    ret = inflateBack(&strm, pull, Z_NULL, push, &strm);
                                                assert(ret == Z_BUF_ERROR);
        /* force mode error by mucking with state */
    ret = inflateBack(&strm, pull, &strm, push, Z_NULL);
                                                assert(ret == Z_STREAM_ERROR);
    ret = inflateBackEnd(&strm);                assert(ret == Z_OK);
    mem_done(&strm, "inflateBack bad state");

    ret = inflateBackInit(&strm, 15, win);      assert(ret == Z_OK);
    ret = inflateBackEnd(&strm);                assert(ret == Z_OK);
    fputs("inflateBack built-in memory routines\n", stderr);
}

/* do a raw inflate of data in hexadecimal with both inflate and inflateBack */
local int try(char *hex, char *id, int err)
{
    int ret;
    unsigned len, size;
    unsigned char *in, *out, *win;
    char *prefix;
    z_stream strm;

    /* convert to hex */
    in = h2b(hex, &len);
    assert(in != NULL);

    /* allocate work areas */
    size = len << 3;
    out = malloc(size);
    assert(out != NULL);
    win = malloc(32768);
    assert(win != NULL);
    prefix = malloc(strlen(id) + 6);
    assert(prefix != NULL);

    /* first with inflate */
    strcpy(prefix, id);
    strcat(prefix, "-late");
    mem_setup(&strm);
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit2(&strm, err < 0 ? 47 : -15);
    assert(ret == Z_OK);
    strm.avail_in = len;
    strm.next_in = in;
    do {
        strm.avail_out = size;
        strm.next_out = out;
        ret = inflate(&strm, Z_TREES);
        assert(ret != Z_STREAM_ERROR && ret != Z_MEM_ERROR);
        if (ret == Z_DATA_ERROR || ret == Z_NEED_DICT)
            break;
    } while (strm.avail_in || strm.avail_out == 0);
    if (err) {
        assert(ret == Z_DATA_ERROR);
        assert(strcmp(id, strm.msg) == 0);
    }
    inflateEnd(&strm);
    mem_done(&strm, prefix);

    /* then with inflateBack */
    if (err >= 0) {
        strcpy(prefix, id);
        strcat(prefix, "-back");
        mem_setup(&strm);
        ret = inflateBackInit(&strm, 15, win);
        assert(ret == Z_OK);
        strm.avail_in = len;
        strm.next_in = in;
        ret = inflateBack(&strm, pull, Z_NULL, push, Z_NULL);
        assert(ret != Z_STREAM_ERROR);
        if (err) {
            assert(ret == Z_DATA_ERROR);
            assert(strcmp(id, strm.msg) == 0);
        }
        inflateBackEnd(&strm);
        mem_done(&strm, prefix);
    }

    /* clean up */
    free(prefix);
    free(win);
    free(out);
    free(in);
    return ret;
}

/* cover deflate data cases in both inflate() and inflateBack() */
local void cover_inflate(void)
{
    try("0 0 0 0 0", "invalid stored block lengths", 1);
    try("3 0", "fixed", 0);
    try("6", "invalid block type", 1);
    try("1 1 0 fe ff 0", "stored", 0);
    try("fc 0 0", "too many length or distance symbols", 1);
    try("4 0 fe ff", "invalid code lengths set", 1);
    try("4 0 24 49 0", "invalid bit length repeat", 1);
    try("4 0 24 e9 ff ff", "invalid bit length repeat", 1);
    try("4 0 24 e9 ff 6d", "invalid code -- missing end-of-block", 1);
    try("4 80 49 92 24 49 92 24 71 ff ff 93 11 0",
        "invalid literal/lengths set", 1);
    try("4 80 49 92 24 49 92 24 f b4 ff ff c3 84", "invalid distances set", 1);
    try("4 c0 81 8 0 0 0 0 20 7f eb b 0 0", "invalid literal/length code", 1);
    try("2 7e ff ff", "invalid distance code", 1);
    try("c c0 81 0 0 0 0 0 90 ff 6b 4 0", "invalid distance too far back", 1);

    /* also trailer mismatch just in inflate() */
    try("1f 8b 8 0 0 0 0 0 0 0 3 0 0 0 0 1", "incorrect data check", -1);
    try("1f 8b 8 0 0 0 0 0 0 0 3 0 0 0 0 0 0 0 0 1",
        "incorrect length check", -1);
    try("5 c0 21 d 0 0 0 80 b0 fe 6d 2f 91 6c", "pull 17", 0);
    try("5 e0 81 91 24 cb b2 2c 49 e2 f 2e 8b 9a 47 56 9f fb fe ec d2 ff 1f",
        "long code", 0);
    try("ed c0 1 1 0 0 0 40 20 ff 57 1b 42 2c 4f", "length extra", 0);
    try("ed cf c1 b1 2c 47 10 c4 30 fa 6f 35 1d 1 82 59 3d fb be 2e 2a fc f c",
        "long distance and extra", 0);
    try("ed c0 81 0 0 0 0 80 a0 fd a9 17 a9 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 "
        "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 6", "window end", 0);
    inf("2 8 20 80 0 3 0", "inflate_fast TYPE return", 0, -15, 258,
        Z_STREAM_END);
    inf("63 18 5 40 c 0", "window wrap", 3, -8, 300, Z_OK);
}

/* cover remaining lines in inftrees.c */
local void cover_trees(void)
{
    int ret;
    unsigned bits;
    unsigned short lens[16], work[16];
    code *next, table[ENOUGH_DISTS];

    /* we need to call inflate_table() directly in order to manifest not-
       enough errors, since zlib insures that enough is always enough */
    for (bits = 0; bits < 15; bits++)
        lens[bits] = (unsigned short)(bits + 1);
    lens[15] = 15;
    next = table;
    bits = 15;
    ret = inflate_table(DISTS, lens, 16, &next, &bits, work);
                                                assert(ret == 1);
    next = table;
    bits = 1;
    ret = inflate_table(DISTS, lens, 16, &next, &bits, work);
                                                assert(ret == 1);
    fputs("inflate_table not enough errors\n", stderr);
}

/* inflateBack() in/out for golden-output tests: all input is already in
   next_in, and output is captured for memcmp. */
local unsigned pull_none(void *desc, unsigned char z_const **buf)
{
    (void)desc;
    (void)buf;
    return 0;
}

struct back_buf {
    unsigned char *out;
    unsigned got;
    unsigned cap;
};

local int push_copy(void *desc, unsigned char *buf, unsigned len)
{
    struct back_buf *d = desc;

    assert(d->got + len <= d->cap);
    memcpy(d->out + d->got, buf, len);
    d->got += len;
    return 0;
}

/* Fill dst[0..n) with a repeating dist-byte pattern. */
local void fill_repeat(unsigned char *dst, unsigned n, unsigned dist)
{
    unsigned i;

    assert(dist > 0);
    for (i = 0; i < dist && i < n; i++)
        dst[i] = (unsigned char)(i * 17 + 3);
    for (; i < n; i++)
        dst[i] = dst[i - dist];
}

/* Raw deflate then inflate and inflateBack, comparing bytes.  avail_out is
   large enough that inflate_fast() runs. */
local void roundtrip_raw(unsigned char *src, unsigned len, int windowBits,
                         int strategy, char *what)
{
    int ret;
    unsigned have, bound, wsize;
    unsigned char *comp, *out, *win;
    z_stream strm;
    struct back_buf back;

    mem_setup(&strm);
    ret = deflateInit2(&strm, Z_BEST_COMPRESSION, Z_DEFLATED, -windowBits, 8,
                       strategy);                assert(ret == Z_OK);
    bound = (unsigned)deflateBound(&strm, len) + 64;
    comp = malloc(bound);                        assert(comp != NULL);
    strm.next_in = src;
    strm.avail_in = len;
    strm.next_out = comp;
    strm.avail_out = bound;
    ret = deflate(&strm, Z_FINISH);              assert(ret == Z_STREAM_END);
    have = bound - strm.avail_out;
    ret = deflateEnd(&strm);                     assert(ret == Z_OK);

    out = malloc(len + 258);                     assert(out != NULL);
    memset(out, 0x5a, len + 258);
    ret = inflateInit2(&strm, -windowBits);      assert(ret == Z_OK);
    strm.next_in = comp;
    strm.avail_in = have;
    strm.next_out = out;
    strm.avail_out = len + 258;
    ret = inflate(&strm, Z_FINISH);              assert(ret == Z_STREAM_END);
    assert(strm.total_out == len);
    assert(memcmp(out, src, len) == 0);
    ret = inflateEnd(&strm);                     assert(ret == Z_OK);

    wsize = 1U << windowBits;
    win = malloc(wsize);                         assert(win != NULL);
    ret = inflateBackInit(&strm, windowBits, win); assert(ret == Z_OK);
    memset(out, 0x5a, len + 258);
    back.out = out;
    back.got = 0;
    back.cap = len + 258;
    strm.next_in = comp;
    strm.avail_in = have;
    ret = inflateBack(&strm, pull_none, Z_NULL, push_copy, &back);
                                                 assert(ret == Z_STREAM_END);
    assert(back.got == len);
    assert(memcmp(out, src, len) == 0);
    ret = inflateBackEnd(&strm);                 assert(ret == Z_OK);

    free(win);
    free(out);
    free(comp);
    mem_done(&strm, what);
    fputs(what, stderr);
    fputc('\n', stderr);
}

/* Deflate src in nchunk successive chunks (Z_SYNC_FLUSH between, Z_FINISH at
   the end) and inflate into equally-sized output buffers, so that later calls
   source matches from the window.  Also inflateBack the whole stream and
   compare. */
local void split_inflate(unsigned char *src, unsigned *chunk, int nchunk,
                         int windowBits, char *what)
{
    int ret, k;
    unsigned have, bound, wsize, total, off;
    unsigned char *comp, **out, *win, *all;
    z_stream strm;
    struct back_buf back;

    total = 0;
    for (k = 0; k < nchunk; k++)
        total += chunk[k];

    mem_setup(&strm);
    ret = deflateInit2(&strm, Z_BEST_COMPRESSION, Z_DEFLATED, -windowBits, 8,
                       Z_DEFAULT_STRATEGY);      assert(ret == Z_OK);
    bound = (unsigned)deflateBound(&strm, total) + 64;
    comp = malloc(bound);                        assert(comp != NULL);
    strm.next_out = comp;
    strm.avail_out = bound;
    off = 0;
    for (k = 0; k < nchunk; k++) {
        strm.next_in = src + off;
        strm.avail_in = chunk[k];
        ret = deflate(&strm, k == nchunk - 1 ? Z_FINISH : Z_SYNC_FLUSH);
        assert(ret == (k == nchunk - 1 ? Z_STREAM_END : Z_OK));
        assert(strm.avail_in == 0);
        off += chunk[k];
    }
    have = bound - strm.avail_out;
    ret = deflateEnd(&strm);                     assert(ret == Z_OK);

    out = malloc((unsigned)nchunk * sizeof(unsigned char *));
                                                 assert(out != NULL);
    ret = inflateInit2(&strm, -windowBits);      assert(ret == Z_OK);
    strm.next_in = comp;
    strm.avail_in = have;
    off = 0;
    for (k = 0; k < nchunk; k++) {
        out[k] = malloc(chunk[k] + (k == nchunk - 1 ? 258 : 0));
                                                     assert(out[k] != NULL);
        strm.next_out = out[k];
        strm.avail_out = chunk[k];
        ret = inflate(&strm, k == nchunk - 1 ? Z_FINISH : Z_NO_FLUSH);
        assert(ret == (k == nchunk - 1 ? Z_STREAM_END : Z_OK));
        assert(strm.avail_out == 0);
        assert(memcmp(out[k], src + off, chunk[k]) == 0);
        off += chunk[k];
    }
    assert(strm.total_out == total);
    ret = inflateEnd(&strm);                     assert(ret == Z_OK);

    wsize = 1U << windowBits;
    win = malloc(wsize);                         assert(win != NULL);
    all = malloc(total);                         assert(all != NULL);
    ret = inflateBackInit(&strm, windowBits, win); assert(ret == Z_OK);
    back.out = all;
    back.got = 0;
    back.cap = total;
    strm.next_in = comp;
    strm.avail_in = have;
    ret = inflateBack(&strm, pull_none, Z_NULL, push_copy, &back);
                                                 assert(ret == Z_STREAM_END);
    assert(back.got == total);
    assert(memcmp(all, src, total) == 0);
    ret = inflateBackEnd(&strm);                 assert(ret == Z_OK);

    free(all);
    free(win);
    for (k = 0; k < nchunk; k++)
        free(out[k]);
    free(out);
    free(comp);
    mem_done(&strm, what);
    fputs(what, stderr);
    fputc('\n', stderr);
}

/* Golden output for dist-1 runs, repeating patterns, mixed window+output,
   wrap, and short remainder after a window prefix. */
local void cover_fast_copies(void)
{
    unsigned n, first, second, dist, extra, i, len;
    unsigned char *src;
    char what[64];
    unsigned dists[3];
    unsigned chunks[3];

    src = malloc(512);                           assert(src != NULL);
    for (n = 0; n < 3; n++) {
        len = n == 0 ? 16 : n == 1 ? 17 : 258;
        for (i = 0; i < 64; i++)
            src[i] = (unsigned char)(i + 1);
        fill_repeat(src + 64, 64 + len, 1);
        sprintf(what, "inflate_fast dist 1 len %u", len);
        roundtrip_raw(src, 64 + 64 + len, 15, Z_RLE, what);
    }
    free(src);

    dists[0] = 2;
    dists[1] = 8;
    dists[2] = 16;
    src = malloc(2048);                          assert(src != NULL);
    for (n = 0; n < 3; n++) {
        dist = dists[n];
        fill_repeat(src, 2048, dist);
        sprintf(what, "inflate_fast repeat dist %u", dist);
        roundtrip_raw(src, 2048, 15, Z_DEFAULT_STRATEGY, what);
    }
    free(src);

    /* mixed window+output (from_out): second buffer match longer than dist */
    first = 16384;
    second = 200;
    dist = 100;
    src = malloc(first + second);                assert(src != NULL);
    {
        unsigned long random = 1;
        for (i = 0; i < first; i++) {
            random = random * 1103515245 + 12345;
            src[i] = (unsigned char)(random >> 16);
        }
    }
    for (i = 0; i < second; i++)
        src[first + i] = src[first + i - dist];
    chunks[0] = first;
    chunks[1] = second;
    split_inflate(src, chunks, 2, 15, "inflate_fast mixed window output");
    free(src);

    /* wrap-around window copy with op >= 16 (windowBits 9: zlib maps 8 to 9
       on deflate, so 9 is the smallest window that round-trips.  600 bytes
       fills a 512-byte window so wnext != 0 on the second inflate.) */
    first = 600;
    second = 200;
    dist = 200;
    src = malloc(first + second);                assert(src != NULL);
    {
        unsigned long random = 1;
        for (i = 0; i < first; i++) {
            random = random * 1103515245 + 12345;
            src[i] = (unsigned char)(random >> 16);
        }
    }
    for (i = 0; i < second; i++)
        src[first + i] = src[first + i - dist];
    chunks[0] = first;
    chunks[1] = second;
    split_inflate(src, chunks, 2, 9, "inflate_fast wrap around window");
    free(src);

    /* wrap-around window with bulk chunk copies.  Output phases of 480 and
       100 bytes fill the 512-byte window and wrap its write index to
       wnext = 68.  50 fresh bytes then a 200-byte period follow: the last
       150 bytes of the window content plus those 50.  The first inflate_fast
       match is then 258 bytes at dist 200 from outbeg 50, so op = 150: 82
       bytes come from before the window wrap, 68 from after it, both bulk,
       with 108 bytes remaining from the output.  (deflate can only reference
       distances up to 250 at windowBits 9.) */
    first = 480 + 100;
    second = 50 + 4 * 200;
    src = malloc(first + second);                assert(src != NULL);
    {
        unsigned long random = 1;
        for (i = 0; i < first + 50; i++) {
            random = random * 1103515245 + 12345;
            src[i] = (unsigned char)(random >> 16);
        }
    }
    for (i = 0; i < 4 * 200; i++)
        src[first + 50 + i] = src[first - 150 + (i % 200)];
    chunks[0] = 480;
    chunks[1] = 100;
    chunks[2] = second;
    split_inflate(src, chunks, 3, 9, "inflate_fast wrap chunk copies");
    free(src);

    /* short remainder (1 and 2) after a window prefix of 16 */
    first = 16384;
    src = malloc(first + 20);                    assert(src != NULL);
    {
        unsigned long random = 1;
        for (i = 0; i < first; i++) {
            random = random * 1103515245 + 12345;
            src[i] = (unsigned char)(random >> 16);
        }
    }
    dist = 16;
    for (extra = 1; extra <= 2; extra++) {
        second = dist + extra;
        for (i = 0; i < second; i++)
            src[first + i] = src[first + i - dist];
        sprintf(what, "inflate_fast window remainder %u", extra);
        chunks[0] = first;
        chunks[1] = second;
        split_inflate(src, chunks, 2, 15, what);
    }
    free(src);
}

/* exercise a match sourced from the saved window into a fresh output buffer.
   half is the size of the first inflated buffer: when it is the whole window
   (32768 for windowBits 15), wnext is 0 on the second inflate and the match
   source is at the end of the window; when it is half the window (16384),
   wnext is non-zero and the source is contiguous. */
local void cover_fast_window_half(unsigned half)
{
    int ret;
    unsigned n, have;
    unsigned long random;
    unsigned char *source, *compressed, *first, *second;
    z_stream strm;

    /* The second half is encoded using matches from the first half.  Inflate
       the halves into separate allocations so that the second call starts
       with matches sourced entirely from the saved window. */
    source = malloc(2 * half);                   assert(source != NULL);
    first = malloc(half);                        assert(first != NULL);
    second = malloc(half);                       assert(second != NULL);
    random = 1;
    for (n = 0; n < half; n++) {
        random = random * 1103515245 + 12345;
        source[n] = (unsigned char)(random >> 16);
    }
    /* Fill the start of the second half with new random bytes, then tile
       those bytes alternating with the last 200 bytes of the first half.
       The second half is then decoded using matches that start in the saved
       window and continue into the fresh output buffer, exercising the
       window chunk and output bulk copies in inflate_fast(), including the
       16-byte thresholds.  A fresh random block first keeps the first match
       of the second half from being the pending symbol left by the first
       half, which inflate() would decode in its slow path. */
    for (n = 0; n < 258; n++) {
        random = random * 1103515245 + 12345;
        source[half + n] = (unsigned char)(random >> 16);
    }
    for (n = 0; n < 6 * 458; n++)
        source[half + 258 + n] = source[half - 200 + (n % 458)];
    for (n = 0; n < half - 258 - 6 * 458; n++)
        source[half + 258 + 6 * 458 + n] = source[n];

    memset(&strm, 0, sizeof(strm));
    ret = deflateInit2(&strm, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8,
                       Z_DEFAULT_STRATEGY);       assert(ret == Z_OK);
    have = (unsigned)deflateBound(&strm, 2 * half) + 64;
    compressed = malloc(have);                   assert(compressed != NULL);
    strm.next_out = compressed;
    strm.avail_out = have;
    strm.next_in = source;
    strm.avail_in = half;
    ret = deflate(&strm, Z_SYNC_FLUSH);           assert(ret == Z_OK);
    assert(strm.avail_in == 0);
    strm.next_in = source + half;
    strm.avail_in = half;
    ret = deflate(&strm, Z_FINISH);               assert(ret == Z_STREAM_END);
    have -= strm.avail_out;
    ret = deflateEnd(&strm);                      assert(ret == Z_OK);

    mem_setup(&strm);
    ret = inflateInit2(&strm, -15);               assert(ret == Z_OK);
    strm.next_in = compressed;
    strm.avail_in = have;
    strm.next_out = first;
    strm.avail_out = half;
    ret = inflate(&strm, Z_NO_FLUSH);             assert(ret == Z_OK);
    assert(strm.avail_out == 0);
    assert(memcmp(first, source, half) == 0);
    strm.next_out = second;
    strm.avail_out = half;
    ret = inflate(&strm, Z_FINISH);               assert(ret == Z_STREAM_END);
    assert(strm.avail_out == 0);
    assert(memcmp(second, source + half, half) == 0);
    ret = inflateEnd(&strm);                      assert(ret == Z_OK);
    mem_done(&strm, half == 32768 ?
                    "inflate_fast saved window full" :
                    "inflate_fast saved window contiguous");

    free(second);
    free(first);
    free(compressed);
    free(source);
}

/* exercise matches sourced from the saved window into a fresh output buffer,
   with the first half filling the window completely and with it ending part
   way into the window */
local void cover_fast_window(void)
{
    cover_fast_window_half(32768);
    cover_fast_window_half(16384);
}

/* Hex cases cover inflate_fast() error paths and return codes only (no
   memcmp).  cover_fast_copies() checks dist-1 runs, repeating matches,
   from_out, wrap, and short remainder bytes via inflate and inflateBack.
   cover_fast_window() checks non-overlapping window memcpy into a fresh
   output buffer. */
local void cover_fast(void)
{
    inf("e5 e0 81 ad 6d cb b2 2c c9 01 1e 59 63 ae 7d ee fb 4d fd b5 35 41 68"
        " ff 7f 0f 0 0 0", "fast length extra bits", 0, -8, 258, Z_DATA_ERROR);
    inf("25 fd 81 b5 6d 59 b6 6a 49 ea af 35 6 34 eb 8c b9 f6 b9 1e ef 67 49"
        " 50 fe ff ff 3f 0 0", "fast distance extra bits", 0, -8, 258,
        Z_DATA_ERROR);
    inf("3 7e 0 0 0 0 0", "fast invalid distance code", 0, -8, 258,
        Z_DATA_ERROR);
    inf("1b 7 0 0 0 0 0", "fast invalid literal/length code", 0, -8, 258,
        Z_DATA_ERROR);
    inf("d c7 1 ae eb 38 c 4 41 a0 87 72 de df fb 1f b8 36 b1 38 5d ff ff 0",
        "fast 2nd level codes and too far back", 0, -8, 258, Z_DATA_ERROR);
    inf("63 18 5 8c 10 8 0 0 0 0", "very common case", 0, -8, 259, Z_OK);
    inf("63 60 60 18 c9 0 8 18 18 18 26 c0 28 0 29 0 0 0",
        "contiguous and wrap around window", 6, -8, 259, Z_OK);
    inf("63 0 3 0 0 0 0 0", "copy direct from output", 0, -8, 259,
        Z_STREAM_END);
    cover_fast_window();
    cover_fast_copies();
}

int main(void)
{
    fprintf(stderr, "%s\n", zlibVersion());
    cover_support();
    cover_wrap();
    cover_back();
    cover_inflate();
    cover_trees();
    cover_fast();
    return 0;
}
