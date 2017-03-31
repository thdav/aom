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

#include "./av1_rtcd.h"
#include "./aom_config.h"
#include "./aom_dsp_rtcd.h"

#include "aom_dsp/bitwriter.h"
#include "aom_dsp/quantize.h"
#include "aom_mem/aom_mem.h"
#include "aom_ports/mem.h"

#include "av1/common/idct.h"
#include "av1/common/reconinter.h"
#include "av1/common/reconintra.h"
#include "av1/common/scan.h"

#include "av1/encoder/av1_quantize.h"
#include "av1/encoder/encodemb.h"
#include "av1/encoder/hybrid_fwd_txfm.h"
#include "av1/encoder/rd.h"
#include "av1/encoder/tokenize.h"

#if CONFIG_PVQ
#include "av1/encoder/encint.h"
#include "av1/common/partition.h"
#include "av1/encoder/pvq_encoder.h"
#endif

// Check if one needs to use c version subtraction.
static int check_subtract_block_size(int w, int h) { return w < 4 || h < 4; }

void av1_subtract_plane(MACROBLOCK *x, BLOCK_SIZE bsize, int plane) {
  struct macroblock_plane *const p = &x->plane[plane];
  const struct macroblockd_plane *const pd = &x->e_mbd.plane[plane];
  const BLOCK_SIZE plane_bsize = get_plane_block_size(bsize, pd);
  const int bw = block_size_wide[plane_bsize];
  const int bh = block_size_high[plane_bsize];

  if (check_subtract_block_size(bw, bh)) {
#if CONFIG_AOM_HIGHBITDEPTH
    if (x->e_mbd.cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
      aom_highbd_subtract_block_c(bh, bw, p->src_diff, bw, p->src.buf,
                                  p->src.stride, pd->dst.buf, pd->dst.stride,
                                  x->e_mbd.bd);
      return;
    }
#endif  // CONFIG_AOM_HIGHBITDEPTH
    aom_subtract_block_c(bh, bw, p->src_diff, bw, p->src.buf, p->src.stride,
                         pd->dst.buf, pd->dst.stride);

    return;
  }

#if CONFIG_AOM_HIGHBITDEPTH
  if (x->e_mbd.cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
    aom_highbd_subtract_block(bh, bw, p->src_diff, bw, p->src.buf,
                              p->src.stride, pd->dst.buf, pd->dst.stride,
                              x->e_mbd.bd);
    return;
  }
#endif  // CONFIG_AOM_HIGHBITDEPTH
  aom_subtract_block(bh, bw, p->src_diff, bw, p->src.buf, p->src.stride,
                     pd->dst.buf, pd->dst.stride);
}

typedef struct av1_token_state {
  int64_t error;
  int rate;
  int16_t next;
  int16_t token;
#if CONFIG_NEW_TOKENSET
  int16_t token_head;
  int16_t token_tail;
  int8_t is_eob;
  int8_t is_dc;
#endif
  tran_low_t qc;
  tran_low_t dqc;
  uint8_t best_index;
} av1_token_state;

// These numbers are empirically obtained.
static const int plane_rd_mult[REF_TYPES][PLANE_TYPES] = {
#if CONFIG_EC_ADAPT
  { 8, 7 }, { 8, 5 },
//  { 10, 7 }, { 8, 5 },
#else
  { 10, 6 }, { 8, 5 },
#endif
};

#define UPDATE_RD_COST()                             \
  {                                                  \
    rd_cost0 = RDCOST(rdmult, rddiv, rate0, error0); \
    rd_cost1 = RDCOST(rdmult, rddiv, rate1, error1); \
  }

#if CONFIG_NEW_TOKENSET
static INLINE int av1_entropy_cost(int p)
{
  int pround = ROUND_POWER_OF_TWO(p, (CDF_PROB_BITS - 8));
  if (pround) {
  return av1_cost_zero(pround);
  } else {
    return av1_cost_zero(p) + ((CDF_PROB_BITS - 8) << AV1_PROB_COST_SHIFT);
  }
}
static INLINE int av1_cost_val15(aom_cdf_prob* cdf, int val)
{
  return av1_entropy_cost(val ? cdf[val] - cdf[val-1] : cdf[0]);
}

static INLINE void set_head_and_tail(av1_token_state *token_state)
{
  int16_t token = token_state->token;
  int is_eob = 0;//token_state->is_eob && token;
  int is_dc = token_state->is_dc;
  int token_head = AOMMIN(token, TWO_TOKEN);
  token_state->token_tail = token - token_head;
  token_state->token_head = (is_dc && is_eob) ? 0 : 2 * token_head - is_eob + is_dc;
}

#endif

static inline int get_eob_cost(aom_cdf_prob cdf_head[CDF_SIZE(ENTROPY_TOKENS)], int is_dc)
{
  // Just base on ONE_TOKEN
  int prob = cdf_head[is_dc + ONE_TOKEN_EOB] - cdf_head[is_dc + ONE_TOKEN_EOB - 1];
  return av1_entropy_cost(prob);
}
static inline int64_t get_token_bit_costs(
    unsigned int token_costs[2][COEFF_CONTEXTS][ENTROPY_TOKENS], int skip_eob,
    int ctx, av1_token_state *token_state,  aom_cdf_prob cdf_head[CDF_SIZE(ENTROPY_TOKENS)],
                                            aom_cdf_prob cdf_tail[CDF_SIZE(ENTROPY_TOKENS)])
{
  int16_t token = token_state->token;
  int is_dc = token_state->is_dc;

  if (token == EOB_TOKEN) return get_eob_cost(cdf_head, is_dc);
  set_head_and_tail(token_state);
  int cost0 = av1_cost_val15(cdf_head, token_state->token_head);
  int cost = cost0;

  if (token > ONE_TOKEN) cost += av1_cost_val15(cdf_tail, token_state->token_tail);
  (void)skip_eob;
  return cost;
}


int av1_optimize_b(const AV1_COMMON *cm, MACROBLOCK *mb, int plane, int block,
                   TX_SIZE tx_size, int ctx) {
  MACROBLOCKD *const xd = &mb->e_mbd;
  struct macroblock_plane *const p = &mb->plane[plane];
  struct macroblockd_plane *const pd = &xd->plane[plane];
  const int ref = is_inter_block(&xd->mi[0]->mbmi);
  av1_token_state tokens[MAX_TX_SQUARE + 1][2];
  uint8_t token_cache[MAX_TX_SQUARE];
  const tran_low_t *const coeff = BLOCK_OFFSET(p->coeff, block);
  tran_low_t *const qcoeff = BLOCK_OFFSET(p->qcoeff, block);
  tran_low_t *const dqcoeff = BLOCK_OFFSET(pd->dqcoeff, block);
  const int eob = p->eobs[block];
  const PLANE_TYPE plane_type = pd->plane_type;
  const int default_eob = tx_size_2d[tx_size];
  const int16_t *const dequant_ptr = pd->dequant;
  const uint8_t *const band_translate = get_band_translate(tx_size);
  const int block_raster_idx = av1_block_index_to_raster_order(tx_size, block);
  TX_TYPE tx_type = get_tx_type(plane_type, xd, block_raster_idx, tx_size);
  const SCAN_ORDER *const scan_order =
      get_scan(cm, tx_size, tx_type, is_inter_block(&xd->mi[0]->mbmi));
  const int16_t *const scan = scan_order->scan;
  const int16_t *const nb = scan_order->neighbors;
  int dqv;
  const int shift = get_tx_scale(tx_size);
  const int dq_step[2] = { dequant_ptr[0] >> shift, dequant_ptr[1] >> shift };
  int next = eob, sz = 0;
  const int64_t rdmult = (mb->rdmult * plane_rd_mult[ref][plane_type]) >> 1;
  const int64_t rddiv = mb->rddiv;
  int64_t rd_cost0, rd_cost1;
  int rate0, rate1;
  int64_t error0, error1;
  int16_t t0, t1;
  int best, band = (eob < default_eob) ? band_translate[eob]
                                       : band_translate[eob - 1];
  int pt, i, final_eob;
  const int cat6_bits = av1_get_cat6_extrabits_size(tx_size, 8);
  unsigned int(*token_costs)[2][COEFF_CONTEXTS][ENTROPY_TOKENS] =
      mb->token_costs[txsize_sqr_map[tx_size]][plane_type][ref];
  const uint16_t *band_counts = &band_count_table[tx_size][band];
  uint16_t band_left = eob - band_cum_count_table[tx_size][band] + 1;
  int next_band = band;
  int shortcut = 0;
  int next_shortcut = 0;
#if CONFIG_EC_ADAPT
  FRAME_CONTEXT *ec_ctx = xd->tile_ctx;
#elif CONFIG_EC_MULTISYMBOL
  FRAME_CONTEXT *ec_ctx = cpi->common.fc;
#endif
//  aom_prob *blockz_probs =
//      cm->fc->blockzero_probs[txsize_sqr_map[tx_size]][type][ref];
  aom_cdf_prob(
      *const coef_head_cdfs)[COEFF_CONTEXTS][CDF_SIZE(ENTROPY_TOKENS)] =
      ec_ctx->coef_head_cdfs[txsize_sqr_map[tx_size]][plane_type][ref];
  aom_cdf_prob(
      *const coef_tail_cdfs)[COEFF_CONTEXTS][CDF_SIZE(ENTROPY_TOKENS)] =
      ec_ctx->coef_tail_cdfs[txsize_sqr_map[tx_size]][plane_type][ref];

  aom_cdf_prob(*cdf_head)[CDF_SIZE(ENTROPY_TOKENS)];
  aom_cdf_prob(*cdf_tail)[CDF_SIZE(ENTROPY_TOKENS)];

  assert((mb->qindex == 0) ^ (xd->lossless[xd->mi[0]->mbmi.segment_id] == 0));

  token_costs += band;

  assert((!plane_type && !plane) || (plane_type && plane));
  assert(eob <= default_eob);

  /* Now set up a Viterbi trellis to evaluate alternative roundings. */
  /* Initialize the sentinel node of the trellis. */
  tokens[eob][0].rate = 0;
  tokens[eob][0].error = 0;
  tokens[eob][0].next = default_eob;
  tokens[eob][0].token = EOB_TOKEN;
  tokens[eob][0].qc = 0;
  tokens[eob][1] = tokens[eob][0];

  for (i = 0; i < eob; i++) {
    const int rc = scan[i];
    // Get the extra bits and sign bits for all po
    tokens[i][0].rate = av1_get_token_cost(qcoeff[rc], &t0, cat6_bits);
    tokens[i][0].token = t0;
    token_cache[rc] = av1_pt_energy_class[t0];
  }

  for (i = eob; i-- > 0;) {
    int base_bits, dx;
    int64_t d2;
    const int rc = scan[i];
    int qval = qcoeff[rc];
    dqv = dequant_ptr[rc != 0];
    next_shortcut = shortcut;

    tokens[next][0].is_eob = tokens[tokens[next][0].next][tokens[next][0].best_index].token == EOB_TOKEN;
    tokens[next][1].is_eob = tokens[tokens[next][1].next][tokens[next][1].best_index].token == EOB_TOKEN;
    tokens[next][0].is_dc = 0;
    tokens[next][1].is_dc = 0;

    next_band = next < default_eob ? band_translate[next] : band_translate[eob - 1];

    /* Only add a trellis state for non-zero coefficients. */
    if (UNLIKELY(qval)) {
      error0 = tokens[next][0].error;
      error1 = tokens[next][1].error;
      /* Evaluate the first possibility for this state. */
      rate0 = tokens[next][0].rate;
      rate1 = tokens[next][1].rate;

      if (next_shortcut) {
        /* Consider both possible successor states. */
        if (next < default_eob) {
          pt = get_coef_context(nb, token_cache, next);// should be next??
          cdf_head = &coef_head_cdfs[next_band][pt];
          cdf_tail = &coef_tail_cdfs[next_band][pt];
          rate0 +=
              get_token_bit_costs(*token_costs, 0, pt, &tokens[next][0], *cdf_head, *cdf_tail);
          rate1 +=
              get_token_bit_costs(*token_costs, 0, pt, &tokens[next][1], *cdf_head, *cdf_tail);
        }
        UPDATE_RD_COST();
        /* And pick the best. */
        best = rd_cost1 < rd_cost0;
      } else {
        if (next < default_eob) {
          pt = get_coef_context(nb, token_cache, next);
          cdf_head = &coef_head_cdfs[next_band][pt];
          cdf_tail = &coef_tail_cdfs[next_band][pt];
          rate0 +=
              get_token_bit_costs(*token_costs, 0, pt, &tokens[next][0], *cdf_head, *cdf_tail);
        }
        best = 0;
      }

      dx = (dqcoeff[rc] - coeff[rc]) * (1 << shift);
      d2 = (int64_t)dx * dx;
      tokens[i][0].rate += (best ? rate1 : rate0);
      tokens[i][0].error = d2 + (best ? error1 : error0);// this coeff error plus best next error
      tokens[i][0].next = next;
      tokens[i][0].qc = qval;
      tokens[i][0].dqc = dqcoeff[rc];
      tokens[i][0].best_index = best;

      /* Evaluate the second possibility for this state. */
      rate0 = tokens[next][0].rate;
      rate1 = tokens[next][1].rate;

      // The threshold of 3 is empirically obtained.
      if (UNLIKELY(abs(qval) > 3)) {
        shortcut = 0;
      } else {
        if ((abs(qval) * dequant_ptr[rc != 0] > (abs(coeff[rc]) << shift)) &&
            (abs(qval) * dequant_ptr[rc != 0] <
             (abs(coeff[rc]) << shift) + dequant_ptr[rc != 0]))
          shortcut = 1;
        else
          shortcut = 0;
      }

      if (shortcut) {
        sz = -(qval < 0);
        qval -= 2 * sz + 1;
      } else {
        tokens[i][1] = tokens[i][0];
        next = i;

        if (UNLIKELY(!(--band_left))) {
          --band_counts;
          band_left = *band_counts;
          --token_costs;
        }
        continue;
      }

      /* Consider both possible successor states. */
      if (!qval) {
        /* If we reduced this coefficient to zero, check to see if
         *  we need to move the EOB back here.
         */
        t0 = tokens[next][0].token == EOB_TOKEN ? EOB_TOKEN : ZERO_TOKEN;
        t1 = tokens[next][1].token == EOB_TOKEN ? EOB_TOKEN : ZERO_TOKEN;
        base_bits = 0;
      } else {
        base_bits = av1_get_token_cost(qval, &t0, cat6_bits);
        t1 = t0;
      }

      if (next_shortcut) {
        if (LIKELY(next < default_eob)) {
          if (t0 != EOB_TOKEN) {
            token_cache[rc] = av1_pt_energy_class[t0];
            pt = get_coef_context(nb, token_cache, next);
            cdf_head = &coef_head_cdfs[next_band][pt];
            cdf_tail = &coef_tail_cdfs[next_band][pt];
            rate0 += get_token_bit_costs(*token_costs, !qval, pt,
                                         &tokens[next][0], *cdf_head, *cdf_tail);
          }
          if (t1 != EOB_TOKEN) {
            token_cache[rc] = av1_pt_energy_class[t1];
            pt = get_coef_context(nb, token_cache, next);
            cdf_head = &coef_head_cdfs[next_band][pt];
            cdf_tail = &coef_tail_cdfs[next_band][pt];
            rate1 += get_token_bit_costs(*token_costs, !qval, pt,
                                         &tokens[next][1], *cdf_head, *cdf_tail);
          }
        }

        UPDATE_RD_COST();
        /* And pick the best. */
        best = rd_cost1 < rd_cost0;
      } else {
        // The two states in next stage are identical.
        if (next < default_eob && t0 != EOB_TOKEN) {
          token_cache[rc] = av1_pt_energy_class[t0];
          pt = get_coef_context(nb, token_cache, next);
          cdf_head = &coef_head_cdfs[next_band][pt];
          cdf_tail = &coef_tail_cdfs[next_band][pt];
          rate0 +=
              get_token_bit_costs(*token_costs, !qval, pt, &tokens[next][0], *cdf_head, *cdf_tail);
        }
        best = 0;
      }
      dx -= (dqv + sz) ^ sz;
      d2 = (int64_t)dx * dx;

      tokens[i][1].rate = base_bits + (best ? rate1 : rate0);
      tokens[i][1].error = d2 + (best ? error1 : error0);
      tokens[i][1].next = next;
      tokens[i][1].token = best ? t1 : t0;
      tokens[i][1].qc = qval;

      if (qval) {
// The 32x32 transform coefficient uses half quantization step size.
// Account for the rounding difference in the dequantized coefficeint
// value when the quantization index is dropped from an even number
// to an odd number.
        tran_low_t offset = dq_step[rc != 0];
        if (shift & qval) offset += (dqv & 0x01);

        if (sz == 0)
          tokens[i][1].dqc = dqcoeff[rc] - offset;
        else
          tokens[i][1].dqc = dqcoeff[rc] + offset;
      } else {
        tokens[i][1].dqc = 0;
      }

      tokens[i][1].best_index = best;
      /* Finally, make this the new head of the trellis. */
      next = i;
    } else {
      /* There's no choice to make for a zero coefficient, so we don't
       *  add a new trellis node, but we do need to update the costs.
       */
      t0 = tokens[next][0].token;
      t1 = tokens[next][1].token;
      pt = get_coef_context(nb, token_cache, next);//next?
      /* Update the cost of each path if we're past the EOB token. */
      if (t0 != EOB_TOKEN) {
        cdf_head = &coef_head_cdfs[next_band][pt];
        cdf_tail = &coef_tail_cdfs[next_band][pt];
        tokens[next][0].rate += get_token_bit_costs(*token_costs, 1, pt, &tokens[next][0], *cdf_head, *cdf_tail);
        tokens[next][0].token = ZERO_TOKEN;
      }
      if (t1 != EOB_TOKEN) {
        cdf_head = &coef_head_cdfs[next_band][pt];
        cdf_tail = &coef_tail_cdfs[next_band][pt];
        tokens[next][1].rate += get_token_bit_costs(*token_costs, 1, pt, &tokens[next][1], *cdf_head, *cdf_tail);
        tokens[next][1].token = ZERO_TOKEN;
      }
      tokens[i][0].best_index = tokens[i][1].best_index = 0;
      shortcut = (tokens[next][0].rate != tokens[next][1].rate);
      /* Don't update next, because we didn't add a new node. */
    }

    if (UNLIKELY(!(--band_left))) {
      --band_counts;
      band_left = *band_counts;
      --token_costs;
    }
  }
  pt = get_coef_context(nb, token_cache, next);//next?
  next_band = next < default_eob ? band_translate[next] : band_translate[eob - 1];
  tokens[next][0].is_eob = tokens[tokens[next][0].next][tokens[next][0].best_index].token == EOB_TOKEN;
  tokens[next][1].is_eob = tokens[tokens[next][1].next][tokens[next][1].best_index].token == EOB_TOKEN;
  tokens[next][0].is_dc = next == 0;
  tokens[next][1].is_dc = next == 0;
  cdf_head = &coef_head_cdfs[next_band][pt];
  cdf_tail = &coef_tail_cdfs[next_band][pt];

  /* Now pick the best path through the whole trellis. */
  rate0 = tokens[next][0].rate;
  rate1 = tokens[next][1].rate;
  error0 = tokens[next][0].error;
  error1 = tokens[next][1].error;
  t0 = tokens[next][0].token;
  t1 = tokens[next][1].token;

  rate0 += get_token_bit_costs(*token_costs, 0, ctx, &tokens[next][0], *cdf_head, *cdf_tail);
  rate1 += get_token_bit_costs(*token_costs, 0, ctx, &tokens[next][1], *cdf_head, *cdf_tail);
  UPDATE_RD_COST();
  best = rd_cost1 < rd_cost0;

  final_eob = -1;

  for (i = next; i < eob; i = next) {
    const int qval = tokens[i][best].qc;
    const int rc = scan[i];
    if (qval) final_eob = i;
    qcoeff[rc] = qval;
    dqcoeff[rc] = tokens[i][best].dqc;

    next = tokens[i][best].next;
    best = tokens[i][best].best_index;
  }
  final_eob++;


  mb->plane[plane].eobs[block] = final_eob;
  assert(final_eob <= default_eob);
  return final_eob;
}

#if !CONFIG_PVQ
#if CONFIG_AOM_HIGHBITDEPTH
typedef enum QUANT_FUNC {
  QUANT_FUNC_LOWBD = 0,
  QUANT_FUNC_HIGHBD = 1,
  QUANT_FUNC_TYPES = 2
} QUANT_FUNC;

static AV1_QUANT_FACADE
    quant_func_list[AV1_XFORM_QUANT_TYPES][QUANT_FUNC_TYPES] = {
      { av1_quantize_fp_facade, av1_highbd_quantize_fp_facade },
      { av1_quantize_b_facade, av1_highbd_quantize_b_facade },
      { av1_quantize_dc_facade, av1_highbd_quantize_dc_facade },
#if CONFIG_NEW_QUANT
      { av1_quantize_fp_nuq_facade, av1_highbd_quantize_fp_nuq_facade },
      { av1_quantize_b_nuq_facade, av1_highbd_quantize_b_nuq_facade },
      { av1_quantize_dc_nuq_facade, av1_highbd_quantize_dc_nuq_facade },
#endif  // CONFIG_NEW_QUANT
      { NULL, NULL }
    };

#else

typedef enum QUANT_FUNC {
  QUANT_FUNC_LOWBD = 0,
  QUANT_FUNC_TYPES = 1
} QUANT_FUNC;

static AV1_QUANT_FACADE quant_func_list[AV1_XFORM_QUANT_TYPES]
                                       [QUANT_FUNC_TYPES] = {
                                         { av1_quantize_fp_facade },
                                         { av1_quantize_b_facade },
                                         { av1_quantize_dc_facade },
#if CONFIG_NEW_QUANT
                                         { av1_quantize_fp_nuq_facade },
                                         { av1_quantize_b_nuq_facade },
                                         { av1_quantize_dc_nuq_facade },
#endif  // CONFIG_NEW_QUANT
                                         { NULL }
                                       };
#endif  // CONFIG_AOM_HIGHBITDEPTH
#endif  // CONFIG_PVQ

void av1_xform_quant(const AV1_COMMON *cm, MACROBLOCK *x, int plane, int block,
                     int blk_row, int blk_col, BLOCK_SIZE plane_bsize,
                     TX_SIZE tx_size, int ctx,
                     AV1_XFORM_QUANT xform_quant_idx) {
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mbmi = &xd->mi[0]->mbmi;
#if !(CONFIG_PVQ || CONFIG_DAALA_DIST)
  const struct macroblock_plane *const p = &x->plane[plane];
  const struct macroblockd_plane *const pd = &xd->plane[plane];
#else
  struct macroblock_plane *const p = &x->plane[plane];
  struct macroblockd_plane *const pd = &xd->plane[plane];
#endif
  PLANE_TYPE plane_type = get_plane_type(plane);
  const int block_raster_idx = av1_block_index_to_raster_order(tx_size, block);
  TX_TYPE tx_type = get_tx_type(plane_type, xd, block_raster_idx, tx_size);
  const int is_inter = is_inter_block(mbmi);
  const SCAN_ORDER *const scan_order = get_scan(cm, tx_size, tx_type, is_inter);
  tran_low_t *const coeff = BLOCK_OFFSET(p->coeff, block);
  tran_low_t *const qcoeff = BLOCK_OFFSET(p->qcoeff, block);
  tran_low_t *const dqcoeff = BLOCK_OFFSET(pd->dqcoeff, block);
  uint16_t *const eob = &p->eobs[block];
  const int diff_stride = block_size_wide[plane_bsize];
#if CONFIG_AOM_QM
  int seg_id = mbmi->segment_id;
  const qm_val_t *qmatrix = pd->seg_qmatrix[seg_id][!is_inter][tx_size];
  const qm_val_t *iqmatrix = pd->seg_iqmatrix[seg_id][!is_inter][tx_size];
#endif

  FWD_TXFM_PARAM fwd_txfm_param;

#if CONFIG_PVQ || CONFIG_DAALA_DIST
  uint8_t *dst;
  int16_t *pred;
  const int dst_stride = pd->dst.stride;
  int tx_blk_size;
  int i, j;
#endif

#if !CONFIG_PVQ
  const int tx2d_size = tx_size_2d[tx_size];
  QUANT_PARAM qparam;
  const int16_t *src_diff;

  src_diff =
      &p->src_diff[(blk_row * diff_stride + blk_col) << tx_size_wide_log2[0]];
  qparam.log_scale = get_tx_scale(tx_size);
#if CONFIG_NEW_QUANT
  qparam.tx_size = tx_size;
  qparam.dq = get_dq_profile_from_ctx(x->qindex, ctx, is_inter, plane_type);
#endif  // CONFIG_NEW_QUANT
#if CONFIG_AOM_QM
  qparam.qmatrix = qmatrix;
  qparam.iqmatrix = iqmatrix;
#endif  // CONFIG_AOM_QM
#else
  tran_low_t *ref_coeff = BLOCK_OFFSET(pd->pvq_ref_coeff, block);
  int skip = 1;
  PVQ_INFO *pvq_info = NULL;
  uint8_t *src;
  int16_t *src_int16;
  const int src_stride = p->src.stride;

  (void)scan_order;
  (void)qcoeff;

  if (x->pvq_coded) {
    assert(block < MAX_PVQ_BLOCKS_IN_SB);
    pvq_info = &x->pvq[block][plane];
  }
  src = &p->src.buf[(blk_row * src_stride + blk_col) << tx_size_wide_log2[0]];
  src_int16 =
      &p->src_int16[(blk_row * diff_stride + blk_col) << tx_size_wide_log2[0]];

  // transform block size in pixels
  tx_blk_size = tx_size_wide[tx_size];
#if CONFIG_AOM_HIGHBITDEPTH
  if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
    for (j = 0; j < tx_blk_size; j++)
      for (i = 0; i < tx_blk_size; i++)
        src_int16[diff_stride * j + i] =
            CONVERT_TO_SHORTPTR(src)[src_stride * j + i];
  } else {
#endif  // CONFIG_AOM_HIGHBITDEPTH
    for (j = 0; j < tx_blk_size; j++)
      for (i = 0; i < tx_blk_size; i++)
        src_int16[diff_stride * j + i] = src[src_stride * j + i];
#if CONFIG_AOM_HIGHBITDEPTH
  }
#endif  // CONFIG_AOM_HIGHBITDEPTH
#endif

#if CONFIG_PVQ || CONFIG_DAALA_DIST
  dst = &pd->dst.buf[(blk_row * dst_stride + blk_col) << tx_size_wide_log2[0]];
  pred = &pd->pred[(blk_row * diff_stride + blk_col) << tx_size_wide_log2[0]];

  // transform block size in pixels
  tx_blk_size = tx_size_wide[tx_size];

// copy uint8 orig and predicted block to int16 buffer
// in order to use existing VP10 transform functions
#if CONFIG_AOM_HIGHBITDEPTH
  if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
    for (j = 0; j < tx_blk_size; j++)
      for (i = 0; i < tx_blk_size; i++)
        pred[diff_stride * j + i] =
            CONVERT_TO_SHORTPTR(dst)[dst_stride * j + i];
  } else {
#endif  // CONFIG_AOM_HIGHBITDEPTH
    for (j = 0; j < tx_blk_size; j++)
      for (i = 0; i < tx_blk_size; i++)
        pred[diff_stride * j + i] = dst[dst_stride * j + i];
#if CONFIG_AOM_HIGHBITDEPTH
  }
#endif  // CONFIG_AOM_HIGHBITDEPTH
#endif

  (void)ctx;

  fwd_txfm_param.tx_type = tx_type;
  fwd_txfm_param.tx_size = tx_size;
  fwd_txfm_param.lossless = xd->lossless[mbmi->segment_id];

#if !CONFIG_PVQ
#if CONFIG_AOM_HIGHBITDEPTH
  fwd_txfm_param.bd = xd->bd;
  if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
    highbd_fwd_txfm(src_diff, coeff, diff_stride, &fwd_txfm_param);
    if (xform_quant_idx != AV1_XFORM_QUANT_SKIP_QUANT) {
      if (LIKELY(!x->skip_block)) {
        quant_func_list[xform_quant_idx][QUANT_FUNC_HIGHBD](
            coeff, tx2d_size, p, qcoeff, pd, dqcoeff, eob, scan_order, &qparam);
      } else {
        av1_quantize_skip(tx2d_size, qcoeff, dqcoeff, eob);
      }
    }
    return;
  }
#endif  // CONFIG_AOM_HIGHBITDEPTH
  fwd_txfm(src_diff, coeff, diff_stride, &fwd_txfm_param);
  if (xform_quant_idx != AV1_XFORM_QUANT_SKIP_QUANT) {
    if (LIKELY(!x->skip_block)) {
      quant_func_list[xform_quant_idx][QUANT_FUNC_LOWBD](
          coeff, tx2d_size, p, qcoeff, pd, dqcoeff, eob, scan_order, &qparam);
    } else {
      av1_quantize_skip(tx2d_size, qcoeff, dqcoeff, eob);
    }
  }
#else  // #if !CONFIG_PVQ
  (void)xform_quant_idx;
#if CONFIG_AOM_HIGHBITDEPTH
  fwd_txfm_param.bd = xd->bd;
  if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
    highbd_fwd_txfm(src_int16, coeff, diff_stride, &fwd_txfm_param);
    highbd_fwd_txfm(pred, ref_coeff, diff_stride, &fwd_txfm_param);
  } else {
#endif
    fwd_txfm(src_int16, coeff, diff_stride, &fwd_txfm_param);
    fwd_txfm(pred, ref_coeff, diff_stride, &fwd_txfm_param);
#if CONFIG_AOM_HIGHBITDEPTH
  }
#endif

  // PVQ for inter mode block
  if (!x->skip_block) {
    PVQ_SKIP_TYPE ac_dc_coded =
        av1_pvq_encode_helper(x,
                              coeff,        // target original vector
                              ref_coeff,    // reference vector
                              dqcoeff,      // de-quantized vector
                              eob,          // End of Block marker
                              pd->dequant,  // aom's quantizers
                              plane,        // image plane
                              tx_size,      // block size in log_2 - 2
                              tx_type,
                              &x->rate,  // rate measured
                              x->pvq_speed,
                              pvq_info);  // PVQ info for a block
    skip = ac_dc_coded == PVQ_SKIP;
  }
  x->pvq_skip[plane] = skip;

  if (!skip) mbmi->skip = 0;
#endif  // #if !CONFIG_PVQ
}

static void encode_block(int plane, int block, int blk_row, int blk_col,
                         BLOCK_SIZE plane_bsize, TX_SIZE tx_size, void *arg) {
  struct encode_b_args *const args = arg;
  AV1_COMMON *cm = args->cm;
  MACROBLOCK *const x = args->x;
  MACROBLOCKD *const xd = &x->e_mbd;
  int ctx;
  struct macroblock_plane *const p = &x->plane[plane];
  struct macroblockd_plane *const pd = &xd->plane[plane];
  tran_low_t *const dqcoeff = BLOCK_OFFSET(pd->dqcoeff, block);
  uint8_t *dst;
  ENTROPY_CONTEXT *a, *l;
  INV_TXFM_PARAM inv_txfm_param;
  const int block_raster_idx = av1_block_index_to_raster_order(tx_size, block);
#if CONFIG_PVQ
  int tx_width_pixels, tx_height_pixels;
  int j;
#endif
#if CONFIG_VAR_TX
  int bw = block_size_wide[plane_bsize] >> tx_size_wide_log2[0];
#endif
  dst = &pd->dst
             .buf[(blk_row * pd->dst.stride + blk_col) << tx_size_wide_log2[0]];
  a = &args->ta[blk_col];
  l = &args->tl[blk_row];
#if CONFIG_VAR_TX
  ctx = get_entropy_context(tx_size, a, l);
#else
  ctx = combine_entropy_contexts(*a, *l);
#endif

#if CONFIG_VAR_TX
  // Assert not magic number (uninitialized).
  assert(x->blk_skip[plane][blk_row * bw + blk_col] != 234);

  if (x->blk_skip[plane][blk_row * bw + blk_col] == 0) {
#else
  {
#endif
#if CONFIG_NEW_QUANT
    av1_xform_quant(cm, x, plane, block, blk_row, blk_col, plane_bsize, tx_size,
                    ctx, AV1_XFORM_QUANT_FP_NUQ);
#else
    av1_xform_quant(cm, x, plane, block, blk_row, blk_col, plane_bsize, tx_size,
                    ctx, AV1_XFORM_QUANT_FP);
#endif  // CONFIG_NEW_QUANT
  }
#if CONFIG_VAR_TX
  else {
    p->eobs[block] = 0;
  }
#endif
#if !CONFIG_PVQ
  if (p->eobs[block] && !xd->lossless[xd->mi[0]->mbmi.segment_id])
    av1_optimize_b(cm, x, plane, block, tx_size, ctx);
#endif

  av1_set_txb_context(x, plane, block, tx_size, a, l);

#if !CONFIG_PVQ
  if (p->eobs[block]) *(args->skip) = 0;

  if (p->eobs[block] == 0) return;
#else
  (void)ctx;
  if (!x->pvq_skip[plane]) *(args->skip) = 0;

  if (x->pvq_skip[plane]) return;

  // transform block size in pixels
  tx_width_pixels = tx_size_wide[tx_size];
  tx_height_pixels = tx_size_high[tx_size];

  // Since av1 does not have separate function which does inverse transform
  // but av1_inv_txfm_add_*x*() also does addition of predicted image to
  // inverse transformed image,
  // pass blank dummy image to av1_inv_txfm_add_*x*(), i.e. set dst as zeros
  for (j = 0; j < tx_height_pixels; j++) {
    int i;
    for (i = 0; i < tx_width_pixels; i++) dst[j * pd->dst.stride + i] = 0;
  }
#endif

  // inverse transform parameters
  inv_txfm_param.tx_type =
      get_tx_type(pd->plane_type, xd, block_raster_idx, tx_size);
  inv_txfm_param.tx_size = tx_size;
  inv_txfm_param.eob = p->eobs[block];
  inv_txfm_param.lossless = xd->lossless[xd->mi[0]->mbmi.segment_id];

#if CONFIG_AOM_HIGHBITDEPTH
  if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
    inv_txfm_param.bd = xd->bd;
    av1_highbd_inv_txfm_add(dqcoeff, dst, pd->dst.stride, &inv_txfm_param);
    return;
  }
#endif  // CONFIG_AOM_HIGHBITDEPTH
  av1_inv_txfm_add(dqcoeff, dst, pd->dst.stride, &inv_txfm_param);
}

#if CONFIG_VAR_TX
static void encode_block_inter(int plane, int block, int blk_row, int blk_col,
                               BLOCK_SIZE plane_bsize, TX_SIZE tx_size,
                               void *arg) {
  struct encode_b_args *const args = arg;
  MACROBLOCK *const x = args->x;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mbmi = &xd->mi[0]->mbmi;
  const BLOCK_SIZE bsize = txsize_to_bsize[tx_size];
  const struct macroblockd_plane *const pd = &xd->plane[plane];
  const int tx_row = blk_row >> (1 - pd->subsampling_y);
  const int tx_col = blk_col >> (1 - pd->subsampling_x);
  TX_SIZE plane_tx_size;
  const int max_blocks_high = max_block_high(xd, plane_bsize, plane);
  const int max_blocks_wide = max_block_wide(xd, plane_bsize, plane);

  if (blk_row >= max_blocks_high || blk_col >= max_blocks_wide) return;

  plane_tx_size =
      plane ? uv_txsize_lookup[bsize][mbmi->inter_tx_size[tx_row][tx_col]][0][0]
            : mbmi->inter_tx_size[tx_row][tx_col];

  if (tx_size == plane_tx_size) {
    encode_block(plane, block, blk_row, blk_col, plane_bsize, tx_size, arg);
  } else {
    const TX_SIZE sub_txs = sub_tx_size_map[tx_size];
    // This is the square transform block partition entry point.
    int bsl = tx_size_wide_unit[sub_txs];
    int i;
    assert(bsl > 0);
    assert(tx_size < TX_SIZES_ALL);

    for (i = 0; i < 4; ++i) {
      const int offsetr = blk_row + ((i >> 1) * bsl);
      const int offsetc = blk_col + ((i & 0x01) * bsl);
      int step = tx_size_wide_unit[sub_txs] * tx_size_high_unit[sub_txs];

      if (offsetr >= max_blocks_high || offsetc >= max_blocks_wide) continue;

      encode_block_inter(plane, block, offsetr, offsetc, plane_bsize, sub_txs,
                         arg);
      block += step;
    }
  }
}
#endif

typedef struct encode_block_pass1_args {
  AV1_COMMON *cm;
  MACROBLOCK *x;
} encode_block_pass1_args;

static void encode_block_pass1(int plane, int block, int blk_row, int blk_col,
                               BLOCK_SIZE plane_bsize, TX_SIZE tx_size,
                               void *arg) {
  encode_block_pass1_args *args = (encode_block_pass1_args *)arg;
  AV1_COMMON *cm = args->cm;
  MACROBLOCK *const x = args->x;
  MACROBLOCKD *const xd = &x->e_mbd;
  struct macroblock_plane *const p = &x->plane[plane];
  struct macroblockd_plane *const pd = &xd->plane[plane];
  tran_low_t *const dqcoeff = BLOCK_OFFSET(pd->dqcoeff, block);
  uint8_t *dst;
  int ctx = 0;
  dst = &pd->dst
             .buf[(blk_row * pd->dst.stride + blk_col) << tx_size_wide_log2[0]];

#if CONFIG_NEW_QUANT
  av1_xform_quant(cm, x, plane, block, blk_row, blk_col, plane_bsize, tx_size,
                  ctx, AV1_XFORM_QUANT_B_NUQ);
#else
  av1_xform_quant(cm, x, plane, block, blk_row, blk_col, plane_bsize, tx_size,
                  ctx, AV1_XFORM_QUANT_B);
#endif  // CONFIG_NEW_QUANT
#if !CONFIG_PVQ
  if (p->eobs[block] > 0) {
#else
  if (!x->pvq_skip[plane]) {
    {
      int tx_blk_size;
      int i, j;
      // transform block size in pixels
      tx_blk_size = tx_size_wide[tx_size];

// Since av1 does not have separate function which does inverse transform
// but av1_inv_txfm_add_*x*() also does addition of predicted image to
// inverse transformed image,
// pass blank dummy image to av1_inv_txfm_add_*x*(), i.e. set dst as zeros
#if CONFIG_AOM_HIGHBITDEPTH
      if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
        for (j = 0; j < tx_blk_size; j++)
          for (i = 0; i < tx_blk_size; i++)
            CONVERT_TO_SHORTPTR(dst)[j * pd->dst.stride + i] = 0;
      } else {
#endif  // CONFIG_AOM_HIGHBITDEPTH
        for (j = 0; j < tx_blk_size; j++)
          for (i = 0; i < tx_blk_size; i++) dst[j * pd->dst.stride + i] = 0;
#if CONFIG_AOM_HIGHBITDEPTH
      }
#endif  // CONFIG_AOM_HIGHBITDEPTH
    }
#endif  // !CONFIG_PVQ
#if CONFIG_AOM_HIGHBITDEPTH
    if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
      if (xd->lossless[xd->mi[0]->mbmi.segment_id]) {
        av1_highbd_iwht4x4_add(dqcoeff, dst, pd->dst.stride, p->eobs[block],
                               xd->bd);
      } else {
        av1_highbd_idct4x4_add(dqcoeff, dst, pd->dst.stride, p->eobs[block],
                               xd->bd);
      }
      return;
    }
#endif  //  CONFIG_AOM_HIGHBITDEPTH
    if (xd->lossless[xd->mi[0]->mbmi.segment_id]) {
      av1_iwht4x4_add(dqcoeff, dst, pd->dst.stride, p->eobs[block]);
    } else {
      av1_idct4x4_add(dqcoeff, dst, pd->dst.stride, p->eobs[block]);
    }
  }
}

void av1_encode_sby_pass1(AV1_COMMON *cm, MACROBLOCK *x, BLOCK_SIZE bsize) {
  encode_block_pass1_args args = { cm, x };
  av1_subtract_plane(x, bsize, 0);
  av1_foreach_transformed_block_in_plane(&x->e_mbd, bsize, 0,
                                         encode_block_pass1, &args);
}

void av1_encode_sb(AV1_COMMON *cm, MACROBLOCK *x, BLOCK_SIZE bsize,
                   const int mi_row, const int mi_col) {
  MACROBLOCKD *const xd = &x->e_mbd;
  struct optimize_ctx ctx;
  MB_MODE_INFO *mbmi = &xd->mi[0]->mbmi;
  struct encode_b_args arg = { cm, x, &ctx, &mbmi->skip, NULL, NULL, 1 };
  int plane;

  mbmi->skip = 1;

  if (x->skip) return;

  for (plane = 0; plane < MAX_MB_PLANE; ++plane) {
#if CONFIG_CB4X4 && !CONFIG_CHROMA_2X2
    if (bsize < BLOCK_8X8 && plane && !is_chroma_reference(mi_row, mi_col))
      continue;
    if (plane) bsize = AOMMAX(bsize, BLOCK_8X8);
#else
    (void)mi_row;
    (void)mi_col;
#endif

#if CONFIG_VAR_TX
    // TODO(jingning): Clean this up.
    const struct macroblockd_plane *const pd = &xd->plane[plane];
    const BLOCK_SIZE plane_bsize = get_plane_block_size(bsize, pd);
    const int mi_width = block_size_wide[plane_bsize] >> tx_size_wide_log2[0];
    const int mi_height = block_size_high[plane_bsize] >> tx_size_wide_log2[0];
    const TX_SIZE max_tx_size = max_txsize_rect_lookup[plane_bsize];
    const BLOCK_SIZE txb_size = txsize_to_bsize[max_tx_size];
    const int bw = block_size_wide[txb_size] >> tx_size_wide_log2[0];
    const int bh = block_size_high[txb_size] >> tx_size_wide_log2[0];
    int idx, idy;
    int block = 0;
    int step = tx_size_wide_unit[max_tx_size] * tx_size_high_unit[max_tx_size];
    av1_get_entropy_contexts(bsize, 0, pd, ctx.ta[plane], ctx.tl[plane]);
#else
    const struct macroblockd_plane *const pd = &xd->plane[plane];
    const TX_SIZE tx_size = get_tx_size(plane, xd);
    av1_get_entropy_contexts(bsize, tx_size, pd, ctx.ta[plane], ctx.tl[plane]);
#endif

#if !CONFIG_PVQ
    av1_subtract_plane(x, bsize, plane);
#endif
    arg.ta = ctx.ta[plane];
    arg.tl = ctx.tl[plane];

#if CONFIG_VAR_TX
    for (idy = 0; idy < mi_height; idy += bh) {
      for (idx = 0; idx < mi_width; idx += bw) {
        encode_block_inter(plane, block, idy, idx, plane_bsize, max_tx_size,
                           &arg);
        block += step;
      }
    }
#else
    av1_foreach_transformed_block_in_plane(xd, bsize, plane, encode_block,
                                           &arg);
#endif
  }
}

#if CONFIG_SUPERTX
void av1_encode_sb_supertx(AV1_COMMON *cm, MACROBLOCK *x, BLOCK_SIZE bsize) {
  MACROBLOCKD *const xd = &x->e_mbd;
  struct optimize_ctx ctx;
  MB_MODE_INFO *mbmi = &xd->mi[0]->mbmi;
  struct encode_b_args arg = { cm, x, &ctx, &mbmi->skip, NULL, NULL, 1 };
  int plane;

  mbmi->skip = 1;
  if (x->skip) return;

  for (plane = 0; plane < MAX_MB_PLANE; ++plane) {
    const struct macroblockd_plane *const pd = &xd->plane[plane];
#if CONFIG_VAR_TX
    const TX_SIZE tx_size = TX_4X4;
#else
    const TX_SIZE tx_size = get_tx_size(plane, xd);
#endif
    av1_subtract_plane(x, bsize, plane);
    av1_get_entropy_contexts(bsize, tx_size, pd, ctx.ta[plane], ctx.tl[plane]);
    arg.ta = ctx.ta[plane];
    arg.tl = ctx.tl[plane];
    av1_foreach_transformed_block_in_plane(xd, bsize, plane, encode_block,
                                           &arg);
  }
}
#endif  // CONFIG_SUPERTX

void av1_set_txb_context(MACROBLOCK *x, int plane, int block, TX_SIZE tx_size,
                         ENTROPY_CONTEXT *a, ENTROPY_CONTEXT *l) {
  (void)tx_size;
#if !CONFIG_PVQ
  struct macroblock_plane *p = &x->plane[plane];
  *a = *l = p->eobs[block] > 0;
#else   // !CONFIG_PVQ
  (void)block;
  *a = *l = !x->pvq_skip[plane];
#endif  // !CONFIG_PVQ

#if CONFIG_VAR_TX || CONFIG_LV_MAP
  int i;
  for (i = 0; i < tx_size_wide_unit[tx_size]; ++i) a[i] = a[0];

  for (i = 0; i < tx_size_high_unit[tx_size]; ++i) l[i] = l[0];
#endif
}

static void encode_block_intra_and_set_context(int plane, int block,
                                               int blk_row, int blk_col,
                                               BLOCK_SIZE plane_bsize,
                                               TX_SIZE tx_size, void *arg) {
  av1_encode_block_intra(plane, block, blk_row, blk_col, plane_bsize, tx_size,
                         arg);

  struct encode_b_args *const args = arg;
  MACROBLOCK *x = args->x;
  ENTROPY_CONTEXT *a = &args->ta[blk_col];
  ENTROPY_CONTEXT *l = &args->tl[blk_row];
  av1_set_txb_context(x, plane, block, tx_size, a, l);
}

void av1_encode_block_intra(int plane, int block, int blk_row, int blk_col,
                            BLOCK_SIZE plane_bsize, TX_SIZE tx_size,
                            void *arg) {
  struct encode_b_args *const args = arg;
  AV1_COMMON *cm = args->cm;
  MACROBLOCK *const x = args->x;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *mbmi = &xd->mi[0]->mbmi;
  struct macroblock_plane *const p = &x->plane[plane];
  struct macroblockd_plane *const pd = &xd->plane[plane];
  tran_low_t *dqcoeff = BLOCK_OFFSET(pd->dqcoeff, block);
  PLANE_TYPE plane_type = get_plane_type(plane);
  const int block_raster_idx = av1_block_index_to_raster_order(tx_size, block);
  const TX_TYPE tx_type =
      get_tx_type(plane_type, xd, block_raster_idx, tx_size);
  PREDICTION_MODE mode;
  const int diff_stride = block_size_wide[plane_bsize];
  uint8_t *src, *dst;
  int16_t *src_diff;
  uint16_t *eob = &p->eobs[block];
  const int src_stride = p->src.stride;
  const int dst_stride = pd->dst.stride;
  const int tx1d_width = tx_size_wide[tx_size];
  const int tx1d_height = tx_size_high[tx_size];
  int ctx = 0;
  INV_TXFM_PARAM inv_txfm_param;
#if CONFIG_PVQ
  int tx_blk_size;
  int i, j;
#endif

  dst = &pd->dst.buf[(blk_row * dst_stride + blk_col) << tx_size_wide_log2[0]];
  src = &p->src.buf[(blk_row * src_stride + blk_col) << tx_size_wide_log2[0]];
  src_diff =
      &p->src_diff[(blk_row * diff_stride + blk_col) << tx_size_wide_log2[0]];
  mode = (plane == 0) ? get_y_mode(xd->mi[0], block_raster_idx) : mbmi->uv_mode;
  av1_predict_intra_block(xd, pd->width, pd->height, txsize_to_bsize[tx_size],
                          mode, dst, dst_stride, dst, dst_stride, blk_col,
                          blk_row, plane);

  if (check_subtract_block_size(tx1d_width, tx1d_height)) {
#if CONFIG_AOM_HIGHBITDEPTH
    if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
      aom_highbd_subtract_block_c(tx1d_height, tx1d_width, src_diff,
                                  diff_stride, src, src_stride, dst, dst_stride,
                                  xd->bd);
    } else {
      aom_subtract_block_c(tx1d_height, tx1d_width, src_diff, diff_stride, src,
                           src_stride, dst, dst_stride);
    }
#else
    aom_subtract_block_c(tx1d_height, tx1d_width, src_diff, diff_stride, src,
                         src_stride, dst, dst_stride);
#endif  // CONFIG_AOM_HIGHBITDEPTH
  } else {
#if CONFIG_AOM_HIGHBITDEPTH
    if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
      aom_highbd_subtract_block(tx1d_height, tx1d_width, src_diff, diff_stride,
                                src, src_stride, dst, dst_stride, xd->bd);
    } else {
      aom_subtract_block(tx1d_height, tx1d_width, src_diff, diff_stride, src,
                         src_stride, dst, dst_stride);
    }
#else
    aom_subtract_block(tx1d_height, tx1d_width, src_diff, diff_stride, src,
                       src_stride, dst, dst_stride);
#endif  // CONFIG_AOM_HIGHBITDEPTH
  }

#if !CONFIG_PVQ
  const ENTROPY_CONTEXT *a = &args->ta[blk_col];
  const ENTROPY_CONTEXT *l = &args->tl[blk_row];
  ctx = combine_entropy_contexts(*a, *l);

  if (args->enable_optimize_b) {
#if CONFIG_NEW_QUANT
    av1_xform_quant(cm, x, plane, block, blk_row, blk_col, plane_bsize, tx_size,
                    ctx, AV1_XFORM_QUANT_FP_NUQ);
#else   // CONFIG_NEW_QUANT
    av1_xform_quant(cm, x, plane, block, blk_row, blk_col, plane_bsize, tx_size,
                    ctx, AV1_XFORM_QUANT_FP);
#endif  // CONFIG_NEW_QUANT
    if (p->eobs[block]) {
      av1_optimize_b(cm, x, plane, block, tx_size, ctx);
    }
  } else {
#if CONFIG_NEW_QUANT
    av1_xform_quant(cm, x, plane, block, blk_row, blk_col, plane_bsize, tx_size,
                    ctx, AV1_XFORM_QUANT_B_NUQ);
#else   // CONFIG_NEW_QUANT
    av1_xform_quant(cm, x, plane, block, blk_row, blk_col, plane_bsize, tx_size,
                    ctx, AV1_XFORM_QUANT_B);
#endif  // CONFIG_NEW_QUANT
  }

  if (*eob) {
    // inverse transform
    inv_txfm_param.tx_type = tx_type;
    inv_txfm_param.tx_size = tx_size;
    inv_txfm_param.eob = *eob;
    inv_txfm_param.lossless = xd->lossless[mbmi->segment_id];
#if CONFIG_AOM_HIGHBITDEPTH
    inv_txfm_param.bd = xd->bd;
    if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
      av1_highbd_inv_txfm_add(dqcoeff, dst, dst_stride, &inv_txfm_param);
    } else {
      av1_inv_txfm_add(dqcoeff, dst, dst_stride, &inv_txfm_param);
    }
#else
    av1_inv_txfm_add(dqcoeff, dst, dst_stride, &inv_txfm_param);
#endif  // CONFIG_AOM_HIGHBITDEPTH

    *(args->skip) = 0;
  }
#else  // #if !CONFIG_PVQ

#if CONFIG_NEW_QUANT
  av1_xform_quant(cm, x, plane, block, blk_row, blk_col, plane_bsize, tx_size,
                  ctx, AV1_XFORM_QUANT_FP_NUQ);
#else
  av1_xform_quant(cm, x, plane, block, blk_row, blk_col, plane_bsize, tx_size,
                  ctx, AV1_XFORM_QUANT_FP);
#endif  // CONFIG_NEW_QUANT

  // *(args->skip) == mbmi->skip
  if (!x->pvq_skip[plane]) *(args->skip) = 0;

  if (x->pvq_skip[plane]) return;

  // transform block size in pixels
  tx_blk_size = tx_size_wide[tx_size];

// Since av1 does not have separate function which does inverse transform
// but av1_inv_txfm_add_*x*() also does addition of predicted image to
// inverse transformed image,
// pass blank dummy image to av1_inv_txfm_add_*x*(), i.e. set dst as zeros
#if CONFIG_AOM_HIGHBITDEPTH
  if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
    for (j = 0; j < tx_blk_size; j++)
      for (i = 0; i < tx_blk_size; i++)
        CONVERT_TO_SHORTPTR(dst)[j * dst_stride + i] = 0;
  } else {
#endif  // CONFIG_AOM_HIGHBITDEPTH
    for (j = 0; j < tx_blk_size; j++)
      for (i = 0; i < tx_blk_size; i++) dst[j * dst_stride + i] = 0;
#if CONFIG_AOM_HIGHBITDEPTH
  }
#endif  // CONFIG_AOM_HIGHBITDEPTH

  inv_txfm_param.tx_type = tx_type;
  inv_txfm_param.tx_size = tx_size;
  inv_txfm_param.eob = *eob;
  inv_txfm_param.lossless = xd->lossless[mbmi->segment_id];
#if CONFIG_AOM_HIGHBITDEPTH
  if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
    inv_txfm_param.bd = xd->bd;
    av1_highbd_inv_txfm_add(dqcoeff, dst, dst_stride, &inv_txfm_param);
  } else {
#endif
    av1_inv_txfm_add(dqcoeff, dst, dst_stride, &inv_txfm_param);
#if CONFIG_AOM_HIGHBITDEPTH
  }
#endif
#endif  // #if !CONFIG_PVQ

#if !CONFIG_PVQ
  if (*eob) *(args->skip) = 0;
#else
// Note : *(args->skip) == mbmi->skip
#endif
}

void av1_encode_intra_block_plane(AV1_COMMON *cm, MACROBLOCK *x,
                                  BLOCK_SIZE bsize, int plane,
                                  int enable_optimize_b, const int mi_row,
                                  const int mi_col) {
  const MACROBLOCKD *const xd = &x->e_mbd;
  ENTROPY_CONTEXT ta[2 * MAX_MIB_SIZE] = { 0 };
  ENTROPY_CONTEXT tl[2 * MAX_MIB_SIZE] = { 0 };

  struct encode_b_args arg = {
    cm, x, NULL, &xd->mi[0]->mbmi.skip, ta, tl, enable_optimize_b
  };

#if CONFIG_CB4X4
  if (bsize < BLOCK_8X8 && plane && !is_chroma_reference(mi_row, mi_col))
    return;
#else
  (void)mi_row;
  (void)mi_col;
#endif

  if (enable_optimize_b) {
    const struct macroblockd_plane *const pd = &xd->plane[plane];
    const TX_SIZE tx_size = get_tx_size(plane, xd);
    av1_get_entropy_contexts(bsize, tx_size, pd, ta, tl);
  }
  av1_foreach_transformed_block_in_plane(
      xd, bsize, plane, encode_block_intra_and_set_context, &arg);
}

#if CONFIG_PVQ
PVQ_SKIP_TYPE av1_pvq_encode_helper(MACROBLOCK *x, tran_low_t *const coeff,
                                    tran_low_t *ref_coeff,
                                    tran_low_t *const dqcoeff, uint16_t *eob,
                                    const int16_t *quant, int plane,
                                    int tx_size, TX_TYPE tx_type, int *rate,
                                    int speed, PVQ_INFO *pvq_info) {
  const int tx_blk_size = tx_size_wide[tx_size];
  daala_enc_ctx *daala_enc = &x->daala_enc;
  PVQ_SKIP_TYPE ac_dc_coded;
  int coeff_shift = 3 - get_tx_scale(tx_size);
  int hbd_downshift = 0;
  int rounding_mask;
  int pvq_dc_quant;
  int use_activity_masking = daala_enc->use_activity_masking;
  int tell;
  int has_dc_skip = 1;
  int i;
  int off = od_qm_offset(tx_size, plane ? 1 : 0);

  DECLARE_ALIGNED(16, tran_low_t, coeff_pvq[OD_TXSIZE_MAX * OD_TXSIZE_MAX]);
  DECLARE_ALIGNED(16, tran_low_t, ref_coeff_pvq[OD_TXSIZE_MAX * OD_TXSIZE_MAX]);
  DECLARE_ALIGNED(16, tran_low_t, dqcoeff_pvq[OD_TXSIZE_MAX * OD_TXSIZE_MAX]);

  DECLARE_ALIGNED(16, int32_t, in_int32[OD_TXSIZE_MAX * OD_TXSIZE_MAX]);
  DECLARE_ALIGNED(16, int32_t, ref_int32[OD_TXSIZE_MAX * OD_TXSIZE_MAX]);
  DECLARE_ALIGNED(16, int32_t, out_int32[OD_TXSIZE_MAX * OD_TXSIZE_MAX]);

#if CONFIG_AOM_HIGHBITDEPTH
  hbd_downshift = x->e_mbd.bd - 8;
#endif

  assert(OD_COEFF_SHIFT >= 4);
  // DC quantizer for PVQ
  if (use_activity_masking)
    pvq_dc_quant =
        OD_MAXI(1, (quant[0] << (OD_COEFF_SHIFT - 3) >> hbd_downshift) *
                           daala_enc->state
                               .pvq_qm_q4[plane][od_qm_get_index(tx_size, 0)] >>
                       4);
  else
    pvq_dc_quant =
        OD_MAXI(1, quant[0] << (OD_COEFF_SHIFT - 3) >> hbd_downshift);

  *eob = 0;

#if CONFIG_DAALA_EC
  tell = od_ec_enc_tell_frac(&daala_enc->w.ec);
#else
#error "CONFIG_PVQ currently requires CONFIG_DAALA_EC."
#endif

  // Change coefficient ordering for pvq encoding.
  od_raster_to_coding_order(coeff_pvq, tx_blk_size, tx_type, coeff,
                            tx_blk_size);
  od_raster_to_coding_order(ref_coeff_pvq, tx_blk_size, tx_type, ref_coeff,
                            tx_blk_size);

  // copy int16 inputs to int32
  for (i = 0; i < tx_blk_size * tx_blk_size; i++) {
    ref_int32[i] =
        AOM_SIGNED_SHL(ref_coeff_pvq[i], OD_COEFF_SHIFT - coeff_shift) >>
        hbd_downshift;
    in_int32[i] = AOM_SIGNED_SHL(coeff_pvq[i], OD_COEFF_SHIFT - coeff_shift) >>
                  hbd_downshift;
  }

  if (abs(in_int32[0] - ref_int32[0]) < pvq_dc_quant * 141 / 256) { /* 0.55 */
    out_int32[0] = 0;
  } else {
    out_int32[0] = OD_DIV_R0(in_int32[0] - ref_int32[0], pvq_dc_quant);
  }

  ac_dc_coded =
      od_pvq_encode(daala_enc, ref_int32, in_int32, out_int32,
                    OD_MAXI(1, quant[0] << (OD_COEFF_SHIFT - 3) >>
                                   hbd_downshift),  // scale/quantizer
                    OD_MAXI(1, quant[1] << (OD_COEFF_SHIFT - 3) >>
                                   hbd_downshift),  // scale/quantizer
                    plane,
                    tx_size, OD_PVQ_BETA[use_activity_masking][plane][tx_size],
                    OD_ROBUST_STREAM,
                    0,  // is_keyframe,
                    daala_enc->state.qm + off, daala_enc->state.qm_inv + off,
                    speed,  // speed
                    pvq_info);

  // Encode residue of DC coeff, if required.
  if (!has_dc_skip || out_int32[0]) {
    generic_encode(&daala_enc->w, &daala_enc->state.adapt.model_dc[plane],
                   abs(out_int32[0]) - has_dc_skip, -1,
                   &daala_enc->state.adapt.ex_dc[plane][tx_size][0], 2);
  }
  if (out_int32[0]) {
    aom_write_bit(&daala_enc->w, out_int32[0] < 0);
  }

  // need to save quantized residue of DC coeff
  // so that final pvq bitstream writing can know whether DC is coded.
  if (pvq_info) pvq_info->dq_dc_residue = out_int32[0];

  out_int32[0] = out_int32[0] * pvq_dc_quant;
  out_int32[0] += ref_int32[0];

  // copy int32 result back to int16
  assert(OD_COEFF_SHIFT > coeff_shift);
  rounding_mask = (1 << (OD_COEFF_SHIFT - coeff_shift - 1)) - 1;
  for (i = 0; i < tx_blk_size * tx_blk_size; i++) {
    out_int32[i] = AOM_SIGNED_SHL(out_int32[i], hbd_downshift);
    dqcoeff_pvq[i] = (out_int32[i] + (out_int32[i] < 0) + rounding_mask) >>
                     (OD_COEFF_SHIFT - coeff_shift);
  }

  // Back to original coefficient order
  od_coding_order_to_raster(dqcoeff, tx_blk_size, tx_type, dqcoeff_pvq,
                            tx_blk_size);

  *eob = tx_blk_size * tx_blk_size;

#if CONFIG_DAALA_EC
  *rate = (od_ec_enc_tell_frac(&daala_enc->w.ec) - tell)
          << (AV1_PROB_COST_SHIFT - OD_BITRES);
#else
#error "CONFIG_PVQ currently requires CONFIG_DAALA_EC."
#endif
  assert(*rate >= 0);

  return ac_dc_coded;
}

void av1_store_pvq_enc_info(PVQ_INFO *pvq_info, int *qg, int *theta,
                            int *max_theta, int *k, od_coeff *y, int nb_bands,
                            const int *off, int *size, int skip_rest,
                            int skip_dir,
                            int bs) {  // block size in log_2 -2
  int i;
  const int tx_blk_size = tx_size_wide[bs];

  for (i = 0; i < nb_bands; i++) {
    pvq_info->qg[i] = qg[i];
    pvq_info->theta[i] = theta[i];
    pvq_info->max_theta[i] = max_theta[i];
    pvq_info->k[i] = k[i];
    pvq_info->off[i] = off[i];
    pvq_info->size[i] = size[i];
  }

  memcpy(pvq_info->y, y, tx_blk_size * tx_blk_size * sizeof(od_coeff));

  pvq_info->nb_bands = nb_bands;
  pvq_info->skip_rest = skip_rest;
  pvq_info->skip_dir = skip_dir;
  pvq_info->bs = bs;
}
#endif
