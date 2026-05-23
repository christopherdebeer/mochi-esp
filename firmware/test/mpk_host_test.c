/*
 * Host unit test for firmware/main/mochi_pack.h — exercises the MPK1
 * reader (both format=0 cell math and format=1 zones/actions) on real
 * packMpkV1 output, without an ESP. Build + run:
 *
 *   gcc -std=c11   -Wall -Wextra -o /tmp/mpk_c   firmware/test/mpk_host_test.c
 *   g++ -std=c++17 -Wall -Wextra -x c++ -o /tmp/mpk_cpp firmware/test/mpk_host_test.c
 *   /tmp/mpk_c   firmware/test/fmt1.bin
 *
 * fmt1.bin is emitted by c15r/mochi-device/emit-test-packs.ts.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../main/mochi_pack.h"

static unsigned char buf[1 << 16];

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s pack.bin\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    assert(n > 16);

    mpk_t p;
    assert(mpk_open(buf, &p) == 0);
    assert(p.format == 1);
    assert(p.count == 2);
    assert(p.cell_w == 8 && p.cell_h == 8);
    assert(p.has_mask == 1);
    assert(p.plane_bytes == 8);
    assert(p.cell_bytes == 24);

    /* labels via directory-indexed entries */
    assert(mpk_find(&p, "sceneA") == 0);
    assert(mpk_find(&p, "sceneB") == 1);
    assert(mpk_find(&p, "nope") == -1);

    /* planes recovered byte-for-byte */
    const uint8_t *ink = mpk_ink(&p, 0);
    const uint8_t *mask = mpk_mask(&p, 0);
    for (int i = 0; i < 8; i++) {
        assert(ink[i] == ((i * 17) & 0xff));
        assert(mask[i] == ((i * 31 + 1) & 0xff));
    }

    /* zones: counts */
    assert(mpk_zone_count(&p, 0) == 3);
    assert(mpk_zone_count(&p, 1) == 2);

    mpk_zone_v1_t z;
    /* entry 0: event */
    assert(mpk_zone_get(&p, 0, 0, &z));
    assert(z.x == 1 && z.y == 2 && z.w == 3 && z.h == 4);
    assert(z.kind == MPK_ACTION_EVENT && z.data == 5 && z.seed_text == NULL);
    /* entry 0: nav_relative -2 (i8 sign-extended) */
    assert(mpk_zone_get(&p, 0, 1, &z));
    assert(z.kind == MPK_ACTION_NAV_RELATIVE && z.data == -2);
    /* entry 0: talk_seed */
    assert(mpk_zone_get(&p, 0, 2, &z));
    assert(z.kind == MPK_ACTION_TALK_SEED && z.seed_len == 11);
    assert(memcmp(z.seed_text, "hello there", 11) == 0);
    /* entry 1: nav_scene absolute + deduped seed resolves to same text */
    assert(mpk_zone_get(&p, 1, 0, &z));
    assert(z.kind == MPK_ACTION_NAV_SCENE && z.data == 9);
    assert(mpk_zone_get(&p, 1, 1, &z));
    assert(z.kind == MPK_ACTION_TALK_SEED && z.seed_len == 11);
    assert(memcmp(z.seed_text, "hello there", 11) == 0);
    /* out-of-range zone */
    assert(!mpk_zone_get(&p, 0, 3, &z));

    /* hit-test: (2,3) lands in zone 0's rect (1,2,3,4) */
    mpk_zone_v1_t hz;
    assert(mpk_zone_hit(&p, 0, 2, 3, &hz) == 0 && hz.kind == MPK_ACTION_EVENT);
    assert(mpk_zone_hit(&p, 0, 100, 100, NULL) == -1);

    /* format guard: a corrupted format byte is rejected, not misparsed */
    unsigned char bad[16];
    memcpy(bad, buf, 16);
    bad[5] = 2;
    mpk_t q;
    assert(mpk_open(bad, &q) == -3);

    printf("mpk_host_test OK (%zu bytes, format=%u, %u entries)\n",
           n, p.format, p.count);
    return 0;
}
