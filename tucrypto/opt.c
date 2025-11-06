/*
 * Argon2 reference source code package - reference C implementations
 *
 * Copyright 2015
 * Daniel Dinu, Dmitry Khovratovich, Jean-Philippe Aumasson, and Samuel Neves
 *
 * You may use this work under the terms of a Creative Commons CC0 1.0
 * License/Waiver or the Apache Public License 2.0, at your option. The terms of
 * these licenses can be found at:
 *
 * - CC0 1.0 Universal : https://creativecommons.org/publicdomain/zero/1.0
 * - Apache 2.0        : https://www.apache.org/licenses/LICENSE-2.0
 *
 * You should have received a copy of both of these licenses along with this
 * software. If not, they may be obtained at the above URLs.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "argon2.h"
#include "core.h"

#include "blake2.h"

#if defined(__SSE2__)
#include "blamka-round-opt.h"
#else
#include "blamka-round-ref.h"
#endif

/*
 * Function fills a new memory block and optionally XORs the old block over the new one.
 * Memory must be initialized.
 * @param state Pointer to the just produced block. Content will be updated(!)
 * @param ref_block Pointer to the reference block
 * @param next_block Pointer to the block to be XORed over. May coincide with @ref_block
 * @param with_xor Whether to XOR into the new block (1) or just overwrite (0)
 * @pre all block pointers must be valid
 */
#if defined(__AVX512F__)
static void fill_block(__m512i *state, const block *ref_block, block *next_block, int with_xor) {
  __m512i      block_XY[ARGON2_512BIT_WORDS_IN_BLOCK];
  unsigned int i;

  if (with_xor) {
    for (i = 0; i < ARGON2_512BIT_WORDS_IN_BLOCK; i++) {
      state[i]    = _mm512_xor_si512(state[i], _mm512_loadu_si512((const __m512i *) ref_block->v + i));
      block_XY[i] = _mm512_xor_si512(state[i], _mm512_loadu_si512((const __m512i *) next_block->v + i));
    }
  } else {
    for (i = 0; i < ARGON2_512BIT_WORDS_IN_BLOCK; i++) {
      block_XY[i] = state[i] = _mm512_xor_si512(state[i], _mm512_loadu_si512((const __m512i *) ref_block->v + i));
    }
  }

  for (i = 0; i < 2; ++i) {
    BLAKE2_ROUND_1(state[8 * i + 0], state[8 * i + 1], state[8 * i + 2], state[8 * i + 3], state[8 * i + 4], state[8 * i + 5],
                   state[8 * i + 6], state[8 * i + 7]);
  }

  for (i = 0; i < 2; ++i) {
    BLAKE2_ROUND_2(state[2 * 0 + i], state[2 * 1 + i], state[2 * 2 + i], state[2 * 3 + i], state[2 * 4 + i], state[2 * 5 + i],
                   state[2 * 6 + i], state[2 * 7 + i]);
  }

  for (i = 0; i < ARGON2_512BIT_WORDS_IN_BLOCK; i++) {
    state[i] = _mm512_xor_si512(state[i], block_XY[i]);
    _mm512_storeu_si512((__m512i *) next_block->v + i, state[i]);
  }
}
#elif defined(__AVX2__)
static void fill_block(__m256i *state, const block *ref_block, block *next_block, int with_xor) {
  __m256i      block_XY[ARGON2_HWORDS_IN_BLOCK];
  unsigned int i;

  if (with_xor) {
    for (i = 0; i < ARGON2_HWORDS_IN_BLOCK; i++) {
      state[i]    = _mm256_xor_si256(state[i], _mm256_loadu_si256((const __m256i *) ref_block->v + i));
      block_XY[i] = _mm256_xor_si256(state[i], _mm256_loadu_si256((const __m256i *) next_block->v + i));
    }
  } else {
    for (i = 0; i < ARGON2_HWORDS_IN_BLOCK; i++) {
      block_XY[i] = state[i] = _mm256_xor_si256(state[i], _mm256_loadu_si256((const __m256i *) ref_block->v + i));
    }
  }

  for (i = 0; i < 4; ++i) {
    BLAKE2_ROUND_1(state[8 * i + 0], state[8 * i + 4], state[8 * i + 1], state[8 * i + 5], state[8 * i + 2], state[8 * i + 6],
                   state[8 * i + 3], state[8 * i + 7]);
  }

  for (i = 0; i < 4; ++i) {
    BLAKE2_ROUND_2(state[0 + i], state[4 + i], state[8 + i], state[12 + i], state[16 + i], state[20 + i], state[24 + i],
                   state[28 + i]);
  }

  for (i = 0; i < ARGON2_HWORDS_IN_BLOCK; i++) {
    state[i] = _mm256_xor_si256(state[i], block_XY[i]);
    _mm256_storeu_si256((__m256i *) next_block->v + i, state[i]);
  }
}
#elif defined(__SSE2__)
static void fill_block(__m128i *state, const block *ref_block, block *next_block, int with_xor) {
  __m128i      block_XY[ARGON2_OWORDS_IN_BLOCK];
  unsigned int i;

  if (with_xor) {
    for (i = 0; i < ARGON2_OWORDS_IN_BLOCK; i++) {
      state[i]    = _mm_xor_si128(state[i], _mm_loadu_si128((const __m128i *) ref_block->v + i));
      block_XY[i] = _mm_xor_si128(state[i], _mm_loadu_si128((const __m128i *) next_block->v + i));
    }
  } else {
    for (i = 0; i < ARGON2_OWORDS_IN_BLOCK; i++) {
      block_XY[i] = state[i] = _mm_xor_si128(state[i], _mm_loadu_si128((const __m128i *) ref_block->v + i));
    }
  }

  for (i = 0; i < 8; ++i) {
    BLAKE2_ROUND(state[8 * i + 0], state[8 * i + 1], state[8 * i + 2], state[8 * i + 3], state[8 * i + 4], state[8 * i + 5],
                 state[8 * i + 6], state[8 * i + 7]);
  }

  for (i = 0; i < 8; ++i) {
    BLAKE2_ROUND(state[8 * 0 + i], state[8 * 1 + i], state[8 * 2 + i], state[8 * 3 + i], state[8 * 4 + i], state[8 * 5 + i],
                 state[8 * 6 + i], state[8 * 7 + i]);
  }

  for (i = 0; i < ARGON2_OWORDS_IN_BLOCK; i++) {
    state[i] = _mm_xor_si128(state[i], block_XY[i]);
    _mm_storeu_si128((__m128i *) next_block->v + i, state[i]);
  }
}
#else
static void fill_block(uint64_t *state, const block *ref_block, block *next_block, int with_xor) {
  uint64_t block_XY[ARGON2_QWORDS_IN_BLOCK];
  unsigned i;

  if (with_xor) {
    for (i = 0; i < ARGON2_QWORDS_IN_BLOCK; ++i)
      block_XY[i] = (state[i] ^= ref_block->v[i]) ^ next_block->v[i];
  } else {
    for (i = 0; i < ARGON2_QWORDS_IN_BLOCK; ++i)
      block_XY[i] = (state[i] ^= ref_block->v[i]);
  }

  for (i = 0; i < 8; ++i) {
    unsigned j = 16 * i;
    BLAKE2_ROUND_NOMSG(state[j + 0], state[j + 1], state[j + 2], state[j + 3], state[j + 4], state[j + 5], state[j + 6],
                       state[j + 7], state[j + 8], state[j + 9], state[j + 10], state[j + 11], state[j + 12], state[j + 13],
                       state[j + 14], state[j + 15]);
  }

  for (i = 0; i < 8; i++) {
    BLAKE2_ROUND_NOMSG(state[2 * i], state[2 * i + 1], state[2 * i + 16], state[2 * i + 17], state[2 * i + 32],
                       state[2 * i + 33], state[2 * i + 48], state[2 * i + 49], state[2 * i + 64], state[2 * i + 65],
                       state[2 * i + 80], state[2 * i + 81], state[2 * i + 96], state[2 * i + 97], state[2 * i + 112],
                       state[2 * i + 113]);
  }

  for (i = 0; i < ARGON2_QWORDS_IN_BLOCK; ++i) {
    state[i] ^= block_XY[i];
    next_block->v[i] = state[i];
  }
}

#endif

static void next_addresses(block *address_block, block *input_block) {
#if defined(__AVX512F__)
  __m512i zero_block[ARGON2_512BIT_WORDS_IN_BLOCK]  = {};
  __m512i zero2_block[ARGON2_512BIT_WORDS_IN_BLOCK] = {};
#elif defined(__AVX2__)
  __m256i zero_block[ARGON2_HWORDS_IN_BLOCK]  = {};
  __m256i zero2_block[ARGON2_HWORDS_IN_BLOCK] = {};
#elif defined(__SSE2__)
  __m128i zero_block[ARGON2_OWORDS_IN_BLOCK]  = {};
  __m128i zero2_block[ARGON2_OWORDS_IN_BLOCK] = {};
#else
  block b_zero_block  = {};
  block b_zero2_block = {};

  uint64_t *zero_block  = b_zero_block.v;
  uint64_t *zero2_block = b_zero2_block.v;
#endif
  input_block->v[6]++;

  fill_block(zero_block, input_block, address_block, 0);
  fill_block(zero2_block, address_block, address_block, 0);
}

void fill_segment(const argon2_instance_t *instance, argon2_position_t position) {
  block   *ref_block = NULL, *curr_block = NULL;
  block    address_block, input_block;
  uint64_t pseudo_rand, ref_index, ref_lane;
  uint32_t prev_offset, curr_offset;
  uint32_t starting_index, i;
#if defined(__AVX512F__)
  __m512i state[ARGON2_512BIT_WORDS_IN_BLOCK];
#elif defined(__AVX2__)
  __m256i state[ARGON2_HWORDS_IN_BLOCK];
#elif defined(__SSE2__)
  __m128i state[ARGON2_OWORDS_IN_BLOCK];
#else
  block     b_state;
  uint64_t *state = b_state.v;
#endif

  int data_independent_addressing;

  if (instance == NULL) {
    return;
  }

  data_independent_addressing = (instance->type == Argon2_i) || (instance->type == Argon2_id && (position.pass == 0) &&
                                                                 (position.slice < ARGON2_SYNC_POINTS / 2));

  if (data_independent_addressing) {
    init_block_value(&input_block, 0);

    input_block.v[0] = position.pass;
    input_block.v[1] = position.lane;
    input_block.v[2] = position.slice;
    input_block.v[3] = instance->memory_blocks;
    input_block.v[4] = instance->passes;
    input_block.v[5] = instance->type;
  }

  starting_index = 0;

  if ((0 == position.pass) && (0 == position.slice)) {
    starting_index = 2; /* we have already generated the first two blocks */

    /* Don't forget to generate the first block of addresses: */
    if (data_independent_addressing) {
      next_addresses(&address_block, &input_block);
    }
  }

  /* Offset of the current block */
  curr_offset = position.lane * instance->lane_length + position.slice * instance->segment_length + starting_index;

  if (0 == curr_offset % instance->lane_length) {
    /* Last block in this lane */
    prev_offset = curr_offset + instance->lane_length - 1;
  } else {
    /* Previous block */
    prev_offset = curr_offset - 1;
  }

  memcpy(state, ((instance->memory + prev_offset)->v), ARGON2_BLOCK_SIZE);

  for (i = starting_index; i < instance->segment_length; ++i, ++curr_offset, ++prev_offset) {
    /*1.1 Rotating prev_offset if needed */
    if (curr_offset % instance->lane_length == 1) {
      prev_offset = curr_offset - 1;
    }

    /* 1.2 Computing the index of the reference block */
    /* 1.2.1 Taking pseudo-random value from the previous block */
    if (data_independent_addressing) {
      if (i % ARGON2_ADDRESSES_IN_BLOCK == 0) {
        next_addresses(&address_block, &input_block);
      }
      pseudo_rand = address_block.v[i % ARGON2_ADDRESSES_IN_BLOCK];
    } else {
      pseudo_rand = instance->memory[prev_offset].v[0];
    }

    /* 1.2.2 Computing the lane of the reference block */
    ref_lane = ((pseudo_rand >> 32)) % instance->lanes;

    if ((position.pass == 0) && (position.slice == 0)) {
      /* Can not reference other lanes yet */
      ref_lane = position.lane;
    }

    /* 1.2.3 Computing the number of possible reference block within the
     * lane.
     */
    position.index = i;
    ref_index      = index_alpha(instance, &position, pseudo_rand & 0xFFFFFFFF, ref_lane == position.lane);

    /* 2 Creating a new block */
    ref_block  = instance->memory + instance->lane_length * ref_lane + ref_index;
    curr_block = instance->memory + curr_offset;
    if (ARGON2_VERSION_10 == instance->version) {
      /* version 1.2.1 and earlier: overwrite, not XOR */
      fill_block(state, ref_block, curr_block, 0);
    } else {
      if (0 == position.pass) {
        fill_block(state, ref_block, curr_block, 0);
      } else {
        fill_block(state, ref_block, curr_block, 1);
      }
    }
  }
}
