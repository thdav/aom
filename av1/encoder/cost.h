/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#ifndef AV1_ENCODER_COST_H_
#define AV1_ENCODER_COST_H_

#include "aom_dsp/prob.h"
#include "aom/aom_integer.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const uint16_t av1_prob_cost[257];

// The factor to scale from cost in bits to cost in av1_prob_cost units.
#define AV1_PROB_COST_SHIFT 9

#define av1_cost_zero(prob) (av1_prob_cost[prob])

#define av1_cost_one(prob) av1_cost_zero(256 - (prob))

#define av1_cost_bit(prob, bit) av1_cost_zero((bit) ? 256 - (prob) : (prob))

// Cost of coding an n bit literal, using 128 (i.e. 50%) probability
// for each bit.
#define av1_cost_literal(n) ((n) * (1 << AV1_PROB_COST_SHIFT))

static INLINE unsigned int cost_branch256(const unsigned int ct[2],
                                          aom_prob p) {
  return ct[0] * av1_cost_zero(p) + ct[1] * av1_cost_one(p);
}

static INLINE int treed_cost(aom_tree tree, const aom_prob *probs, int bits,
                             int len) {
  int cost = 0;
  aom_tree_index i = 0;

  do {
    const int bit = (bits >> --len) & 1;
    cost += av1_cost_bit(probs[i >> 1], bit);
    i = tree[i + bit];
  } while (len);

  return cost;
}

void av1_cost_tokens(int *costs, const aom_prob *probs, aom_tree tree);
void av1_cost_tokens_skip(int *costs, const aom_prob *probs, aom_tree tree);

#if CONFIG_EC_MULTISYMBOL
static INLINE int av1_get_cdf_prob(const aom_cdf_prob *const cdf, const int i){
  return AOM_ICDF(cdf[i]) - (i==0 ? 0 : AOM_ICDF(cdf[i-1]));
}

static INLINE int av1_cost_prob15(const aom_cdf_prob prob) {
  const int prob8 = prob >> (CDF_PROB_BITS - 8);
  int cost;
  if (prob8 > 1) {
    const int scale = 1<<(CDF_PROB_BITS - 8);
    const int rem = prob - (prob8 << (CDF_PROB_BITS - 8));
    cost = (av1_cost_zero(prob8) * (scale - rem) + av1_cost_zero(prob8) * rem ) >> (CDF_PROB_BITS - 8);
  } else {
    cost = av1_cost_zero(prob) + ((CDF_PROB_BITS - 8) << AV1_PROB_COST_SHIFT);
  }
  return cost;
}
static INLINE int av1_cost_cdf_val(const aom_cdf_prob *const cdf, const int i) {
  return av1_cost_prob15(av1_get_cdf_prob(cdf, i));
}
#endif

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AV1_ENCODER_COST_H_
