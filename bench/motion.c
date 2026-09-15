/** \file motion.c
    \brief Measures the headroom a MOVE primitive would have over what zstd's
           long-distance matching already finds by itself.

    The question. APXL concatenates full frames into one zstd stream, so an LZ
    match already expresses "copy these bytes from further back" -- COPY, in
    effect, and it is why finished frames compress so well across a shot. What
    an LZ match cannot express cheaply is a *displaced* copy: a block that moved
    is not contiguous in raster order, so it costs one match per row instead of
    one vector per block. Before anyone designs blocks and vectors into the
    format, this answers whether there is anything there to win.

    Method. For each consecutive frame pair, cut the later frame into BxB blocks
    and classify each one, always exactly -- the format is lossless, so an
    approximate match is worth nothing:

      COPY      identical to the same position in the previous frame. Already
                free today: zstd finds these as long matches.
      FLAT      a single colour throughout. Excluded from MOVE on purpose: such
                a block matches every other blank block on the page, so counting
                it as motion inflates the result with blocks a vector would
                never be spent on -- a uniform run is already almost free to
                code. On line art this is most of the page, so the distinction
                decides the answer rather than decorating it.
      MOVE      identical to *some* other position in the previous frame, and
                not flat. This is the prize, and the only class a motion vector
                would buy.
      RESIDUAL  not identical anywhere. Needs real residual coding.

    The search is over the whole previous frame, not a +-N window, because the
    point is an upper bound on what MOVE could ever buy, not the yield of one
    particular search strategy. A real encoder would find less.

    Coherence matters as much as the share. Vectors have to be coded too, so
    MOVE only pays if the winning vectors agree with each other -- a camera pan
    gives every block the same vector and costs almost nothing to code, while
    scattered vectors cost as much as they save. The report therefore prints how
    much of the MOVE share the single most common vector accounts for.

    Usage:
      bench/motion <frame.png> <frame.png> ...   [env: MOTION_BLOCK=16]

    Frames are given in order and treated as one shot; the summary is per shot,
    never per frame, because consecutive frames of a shot are near-duplicates
    and frame-level sampling reports one shot as if it were a corpus.
*/

#include "../src/pxl_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Candidate positions checked per block before giving up. Flat content can
   share a first row in millions of places; without a cap one pathological
   block costs more than the whole rest of the frame. Exact (0,0) is tested
   before any search, so the common still-background case never gets here.

   The cap can only ever make MOVE look smaller, so a run whose `capped` count
   is non-zero is a lower bound on the upper bound. Raise it with
   MOTION_CANDIDATES and re-run to check the number has settled. */
#define MAX_CANDIDATES_DEFAULT 100000

typedef struct {
    uint32_t hash;
    int32_t  x, y;
} entry;

typedef struct {
    entry*   e;
    int32_t* head;      /* bucket -> index into e, -1 empty */
    int32_t* next;      /* chain */
    size_t   count, cap;
    uint32_t mask;
} index_t;

/* Signature over four rows spread through the block, not just the first.
   Hashing one row is cheaper but useless on line art: a block whose top row is
   plain background collides with every other such block on the page, and the
   resulting chains are long enough that the candidate cap starts deciding the
   answer. Four rows cost four times as much to build and make collisions rare.
   Equality is still verified in full on every hit, so this only affects speed,
   never the classification. */
static uint32_t block_sig(const unsigned char* p, size_t stride, int b, size_t rowlen)
{
    uint32_t h = 2166136261u;
    int k;
    for (k = 0; k < 4; ++k) {
        int r = (b * k) / 4;
        const unsigned char* q = p + (size_t)r * stride;
        size_t i;
        for (i = 0; i < rowlen; ++i) { h ^= q[i]; h *= 16777619u; }
    }
    return h;
}

/* True when every pixel of the block is the same colour. */
static int block_flat(const unsigned char* p, size_t stride, size_t pb, int b, size_t rowlen)
{
    int r;
    size_t i;
    for (i = pb; i < rowlen; ++i) { if (p[i] != p[i % pb]) { return 0; } }
    for (r = 1; r < b; ++r) {
        if (memcmp(p, p + (size_t)r * stride, rowlen) != 0) { return 0; }
    }
    return 1;
}

static int block_equal(const unsigned char* a, const unsigned char* b,
                       size_t stride_a, size_t stride_b, int bh, size_t rowlen)
{
    int r;
    for (r = 0; r < bh; ++r) {
        if (memcmp(a + (size_t)r * stride_a, b + (size_t)r * stride_b, rowlen) != 0) {
            return 0;
        }
    }
    return 1;
}

/* Index every block-origin of `img` by the hash of the block's first row. The
   first row is a cheap filter; full equality is verified on lookup. */
static int index_build(index_t* ix, const unsigned char* img, uint32_t w, uint32_t h,
                       size_t stride, size_t pb, int b, size_t rowlen)
{
    size_t positions, bucket_count = 1;
    int32_t x, y;

    memset(ix, 0, sizeof(*ix));   /* so index_free is safe on every early exit */
    if ((uint32_t)b > w || (uint32_t)b > h) { return 0; }
    positions = (size_t)(w - b + 1) * (size_t)(h - b + 1);
    while (bucket_count < positions * 2 && bucket_count < (1u << 24)) { bucket_count <<= 1; }

    ix->cap = positions;
    ix->count = 0;
    ix->mask = (uint32_t)(bucket_count - 1);
    ix->e    = (entry*)malloc(positions * sizeof(entry));
    ix->next = (int32_t*)malloc(positions * sizeof(int32_t));
    ix->head = (int32_t*)malloc(bucket_count * sizeof(int32_t));
    if (!ix->e || !ix->next || !ix->head) { return 0; }
    memset(ix->head, 0xFF, bucket_count * sizeof(int32_t));

    for (y = 0; y + b <= (int32_t)h; ++y) {
        for (x = 0; x + b <= (int32_t)w; ++x) {
            const unsigned char* p = img + (size_t)y * stride + (size_t)x * pb;
            uint32_t hv = block_sig(p, stride, b, rowlen);
            uint32_t bkt = hv & ix->mask;
            size_t i = ix->count++;
            ix->e[i].hash = hv;
            ix->e[i].x = x;
            ix->e[i].y = y;
            ix->next[i] = ix->head[bkt];
            ix->head[bkt] = (int32_t)i;
        }
    }
    return 1;
}

static void index_free(index_t* ix)
{
    free(ix->e); free(ix->next); free(ix->head);
    memset(ix, 0, sizeof(*ix));
}

/* Vector tally, kept small: we only need the dominant vector's share. */
typedef struct { int32_t dx, dy; uint64_t n; } vec;

static void vec_add(vec* v, size_t* nv, size_t cap, int32_t dx, int32_t dy)
{
    size_t i;
    for (i = 0; i < *nv; ++i) {
        if (v[i].dx == dx && v[i].dy == dy) { v[i].n++; return; }
    }
    if (*nv < cap) { v[*nv].dx = dx; v[*nv].dy = dy; v[*nv].n = 1; (*nv)++; }
}

int main(int argc, char** argv)
{
    int b = 16;
    long max_cand = MAX_CANDIDATES_DEFAULT;
    const char* env = getenv("MOTION_BLOCK");
    const char* envc = getenv("MOTION_CANDIDATES");
    pxl_image prev;
    uint64_t t_copy = 0, t_move = 0, t_resid = 0, t_flat = 0, t_blocks = 0;
    uint64_t pairs = 0, capped = 0;
    vec vtab[4096];
    size_t nv = 0;
    int i;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <frame.png> <frame.png> ...  (one shot, in order)\n", argv[0]);
        return 2;
    }
    if (env && atoi(env) > 0) { b = atoi(env); }
    if (envc && atol(envc) > 0) { max_cand = atol(envc); }

    memset(&prev, 0, sizeof(prev));
    prev = pxl_load_png(argv[1]);
    if (!prev.buffer.data) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }

    for (i = 2; i < argc; ++i) {
        pxl_image cur = pxl_load_png(argv[i]);
        size_t pb, stride, rowlen;
        index_t ix;
        int32_t bx, by;

        if (!cur.buffer.data) { fprintf(stderr, "cannot load %s\n", argv[i]); break; }
        if (cur.width != prev.width || cur.height != prev.height ||
            cur.channels != prev.channels ||
            cur.bytes_per_channel != prev.bytes_per_channel) {
            fprintf(stderr, "geometry changes at %s, stopping\n", argv[i]);
            pxl_image_free(&cur);
            break;
        }

        pb     = (size_t)cur.channels * cur.bytes_per_channel;
        stride = (size_t)cur.width * pb;
        rowlen = (size_t)b * pb;

        if (!index_build(&ix, prev.buffer.data, prev.width, prev.height, stride, pb, b, rowlen)) {
            fprintf(stderr, "index failed\n");
            index_free(&ix);
            pxl_image_free(&cur);
            break;
        }

        for (by = 0; by + b <= (int32_t)cur.height; by += b) {
            for (bx = 0; bx + b <= (int32_t)cur.width; bx += b) {
                const unsigned char* c = cur.buffer.data + (size_t)by * stride + (size_t)bx * pb;
                const unsigned char* p0 = prev.buffer.data + (size_t)by * stride + (size_t)bx * pb;
                uint32_t hv, bkt;
                int32_t j, tried = 0, found = 0, best_dx = 0, best_dy = 0;
                uint64_t best_cost = (uint64_t)-1;

                ++t_blocks;

                /* COPY first: same position, exact. This is what zstd already
                   gets for free, and it keeps flat backgrounds out of the
                   search below. */
                if (block_equal(c, p0, stride, stride, b, rowlen)) { ++t_copy; continue; }

                /* Blank blocks match anywhere and would be counted as motion
                   for nothing; see the file header. */
                if (block_flat(c, stride, pb, b, rowlen)) { ++t_flat; continue; }

                hv  = block_sig(c, stride, b, rowlen);
                bkt = hv & ix.mask;
                for (j = ix.head[bkt]; j >= 0; j = ix.next[j]) {
                    const unsigned char* q;
                    int32_t dx, dy;
                    uint64_t cost;
                    if (ix.e[j].hash != hv) { continue; }
                    if (++tried > max_cand) { ++capped; break; }
                    q = prev.buffer.data + (size_t)ix.e[j].y * stride + (size_t)ix.e[j].x * pb;
                    if (!block_equal(c, q, stride, stride, b, rowlen)) { continue; }
                    dx = ix.e[j].x - bx;
                    dy = ix.e[j].y - by;
                    /* Prefer the shortest vector: shorter ones are cheaper to
                       code and likelier to agree with their neighbours. */
                    cost = (uint64_t)((int64_t)dx * dx + (int64_t)dy * dy);
                    if (cost < best_cost) { best_cost = cost; best_dx = dx; best_dy = dy; found = 1; }
                }

                if (found) {
                    ++t_move;
                    vec_add(vtab, &nv, sizeof(vtab) / sizeof(vtab[0]), best_dx, best_dy);
                } else {
                    ++t_resid;
                }
            }
        }

        index_free(&ix);
        pxl_image_free(&prev);
        prev = cur;
        ++pairs;
    }
    pxl_image_free(&prev);

    {
        uint64_t dom = 0;
        size_t k;
        for (k = 0; k < nv; ++k) { if (vtab[k].n > dom) { dom = vtab[k].n; } }
        printf("pairs=%llu block=%d blocks=%llu copy=%.2f%% flat=%.2f%% move=%.2f%% residual=%.2f%%"
               " move_dominant_vec=%.1f%% distinct_vecs=%llu capped=%llu cand_cap=%ld\n",
               (unsigned long long)pairs, b, (unsigned long long)t_blocks,
               t_blocks ? 100.0 * (double)t_copy  / (double)t_blocks : 0.0,
               t_blocks ? 100.0 * (double)t_flat  / (double)t_blocks : 0.0,
               t_blocks ? 100.0 * (double)t_move  / (double)t_blocks : 0.0,
               t_blocks ? 100.0 * (double)t_resid / (double)t_blocks : 0.0,
               t_move   ? 100.0 * (double)dom     / (double)t_move   : 0.0,
               (unsigned long long)nv, (unsigned long long)capped, max_cand);
    }
    return 0;
}
