/**********************************************************************
  Copyright(c) 2026 Intel Corporation All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.
    * Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**********************************************************************/

/*
 * Level 3 extends the matches of 8 bytes that gen_icf_map_lh1() finds to up to 16 bytes and
 * more in set_long_icf_fg(). gen_icf_map_lh1_06 extends them to 16 bytes itself, and
 * set_long_icf_fg_06 relies on that, while the other versions of gen_icf_map_lh1() leave them
 * to set_long_icf_fg(). This checks that exactly one of the dispatched functions extends a
 * match of more than 8 bytes, so that they are a pair.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "igzip_lib.h"
#include "igzip_level_buf_structs.h"

extern uint32_t
gen_icf_map_lh1(struct isal_zstream *, struct deflate_icf *, uint64_t);
extern void
set_long_icf_fg(uint8_t *, uint64_t, uint64_t, struct deflate_icf *);

#define TEST_LEN   2048
#define MATCH_POS  1000
#define MATCH_DIST 100
#define MATCH_LEN  12

/* Returns the length of the match of an entry, or less than 3 for a literal */
static int
entry_len(struct deflate_icf *icf)
{
        uint32_t code;

        memcpy(&code, icf, sizeof(code));
        return (int) (code & LIT_LEN_MASK) - LEN_OFFSET;
}

int
main(void)
{
        struct isal_zstream stream;
        struct level_buf *level_buf;
        struct deflate_icf *matches;
        uint8_t *in;
        uint32_t i, pos, processed, code, rand_state = 1;
        int full_len, gen_extends, set_long_extends;
        int ret = 1;

        printf("igzip_icf_map_test: ");
        level_buf = malloc(sizeof(*level_buf));
        in = malloc(TEST_LEN);
        if (level_buf == NULL || in == NULL) {
                printf("alloc error: Fail\n");
                goto exit;
        }

        /* Random bytes with a match of MATCH_LEN bytes at MATCH_POS */
        for (i = 0; i < TEST_LEN; i++) {
                rand_state = rand_state * 1103515245 + 12345;
                in[i] = rand_state >> 16;
        }
        memcpy(in + MATCH_POS, in + MATCH_POS - MATCH_DIST, MATCH_LEN);
        in[MATCH_POS + MATCH_LEN] = ~in[MATCH_POS - MATCH_DIST + MATCH_LEN];

        memset(&stream, 0, sizeof(stream));
        memset(level_buf->hash_map.hash_table, -1, sizeof(level_buf->hash_map.hash_table));
        stream.next_in = in;
        stream.avail_in = TEST_LEN;
        stream.level_buf = (uint8_t *) level_buf;
        stream.internal_state.dist_mask = IGZIP_HIST_SIZE - 1;
        stream.internal_state.hash_mask = IGZIP_LVL3_HASH_SIZE - 1;
        stream.internal_state.has_hist = IGZIP_NO_HIST;
        matches = level_buf->hash_map.matches;

        /* Depending on the hashes, the match may start a few positions into the copy */
        processed = gen_icf_map_lh1(&stream, matches, TEST_LEN);
        for (pos = MATCH_POS; pos < MATCH_POS + 4; pos++)
                if (pos + 16 < processed && entry_len(&matches[pos]) >= 8)
                        break;
        if (pos == MATCH_POS + 4) {
                printf("match not found: Fail\n");
                goto exit;
        }
        full_len = MATCH_LEN - (pos - MATCH_POS);
        gen_extends = entry_len(&matches[pos]) == full_len;

        /* Give set_long_icf_fg() the match as 8 bytes */
        memcpy(&code, &matches[pos], sizeof(code));
        code = (code & ~LIT_LEN_MASK) | (LEN_OFFSET + 8);
        memcpy(&matches[pos], &code, sizeof(code));

        set_long_icf_fg(in, processed, TEST_LEN, matches);
        set_long_extends = entry_len(&matches[pos]) == full_len;

        if (gen_extends == set_long_extends) {
                printf("gen_icf_map_lh1() %s and set_long_icf_fg() %s 8 byte matches: Fail\n",
                       gen_extends ? "extends" : "does not extend",
                       set_long_extends ? "extends" : "does not extend");
                goto exit;
        }

        printf("Pass\n");
        ret = 0;
exit:
        free(level_buf);
        free(in);
        return ret;
}
