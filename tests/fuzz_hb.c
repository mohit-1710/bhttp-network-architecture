/*
 * fuzz_hb.c - mutation fuzzer for the header-block decoder and the -v
 * field printer. Build with sanitizers (make fuzz) so any out-of-bounds
 * access aborts. Deterministic: the same seed gives the same inputs.
 *
 *   usage: fuzz_hb [iterations]
 */
#include "../src/bproto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long g_state = 0x9e3779b97f4a7c15ULL;

static unsigned rnd(void)
{
    g_state ^= g_state << 13;
    g_state ^= g_state >> 7;
    g_state ^= g_state << 17;
    return (unsigned)(g_state >> 32);
}

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 200000;
    FILE *null = fopen("/dev/null", "w");

    /* Seeds: a real request block and a real response block. */
    bh_buf req = { 0 }, res = { 0 };
    bh_hb_add(&req, ":method", "GET");
    bh_hb_add(&req, ":path", "/hello.txt");
    bh_hb_add(&req, "host", "localhost:9000");
    bh_hb_add(&req, "x-trace-id", "7f3a");
    bh_hb_add(&res, ":status", "200");
    bh_hb_add(&res, "content-type", "text/plain; charset=utf-8");
    bh_hb_add(&res, "content-length", "20");

    uint8_t buf[4096];
    long ok = 0;
    for (long i = 0; i < iters; i++) {
        const bh_buf *seed = (i & 1) ? &res : &req;
        size_t n = seed->len;
        memcpy(buf, seed->buf, n);
        int edits = 1 + rnd() % 8;
        for (int e = 0; e < edits; e++) {
            switch (rnd() % 4) {
            case 0: if (n) buf[rnd() % n] = (uint8_t)rnd(); break;          /* flip a byte */
            case 1: if (n) n = rnd() % n; break;                            /* truncate */
            case 2: if (n + 1 < sizeof buf) buf[n++] = (uint8_t)rnd(); break; /* append */
            case 3: if (n) buf[rnd() % n] ^= 0x80; break;                   /* toggle top bit */
            }
        }
        /* The decoder must see exactly n bytes, so copy to an exact-size heap
         * block: ASan then catches a read one byte past the end. */
        uint8_t *exact = malloc(n ? n : 1);
        memcpy(exact, buf, n);
        bh_headers h;
        if (bh_hb_decode(exact, n, &h) == 0) {
            ok++;
            bh_headers_free(&h);
        }
        bh_trace_fields(null, '<', exact, n);
        free(exact);
    }
    printf("fuzz_hb: %ld inputs, %ld decoded as valid, no crashes\n", iters, ok);
    bh_buf_free(&req);
    bh_buf_free(&res);
    fclose(null);
    return 0;
}
