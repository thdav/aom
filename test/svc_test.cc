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


#include <string>
#include "third_party/googletest/src/include/gtest/gtest.h"
#include "test/codec_factory.h"
#include "test/decode_test_driver.h"
#include "test/i420_video_source.h"

#include "av1/decoder/decoder.h"

#include "aom/svc_context.h"
#include "aom/aomcx.h"
#include "aom/aom_encoder.h"

namespace {

using libaom_test::CodecFactory;
using libaom_test::Decoder;
using libaom_test::DxDataIterator;
using libaom_test::AV1CodecFactory;

class SvcTest : public ::testing::Test {
 protected:
  static const uint32_t kWidth = 352;
  static const uint32_t kHeight = 288;

  SvcTest()
      : codec_iface_(0), test_file_name_("hantro_collage_w352h288.yuv"),
        codec_initialized_(false), decoder_(0) {
    memset(&svc_, 0, sizeof(svc_));
    memset(&codec_, 0, sizeof(codec_));
    memset(&codec_enc_, 0, sizeof(codec_enc_));
  }

  virtual ~SvcTest() {}

  virtual void SetUp() {
    svc_.log_level = SVC_LOG_DEBUG;
    svc_.log_print = 0;

    codec_iface_ = aom_codec_av1_cx();
    const aom_codec_err_t res =
        aom_codec_enc_config_default(codec_iface_, &codec_enc_, 0);
    EXPECT_EQ(AOM_CODEC_OK, res);

    codec_enc_.g_w = kWidth;
    codec_enc_.g_h = kHeight;
    codec_enc_.g_timebase.num = 1;
    codec_enc_.g_timebase.den = 60;
    codec_enc_.kf_min_dist = 100;
    codec_enc_.kf_max_dist = 100;

    aom_codec_dec_cfg_t dec_cfg = aom_codec_dec_cfg_t();
    AV1CodecFactory codec_factory;
    decoder_ = codec_factory.CreateDecoder(dec_cfg, 0);

    tile_columns_ = 0;
    tile_rows_ = 0;
  }

  virtual void TearDown() {
    ReleaseEncoder();
    delete (decoder_);
  }

  void InitializeEncoder() {
    const aom_codec_err_t res =
        aom_svc_init(&svc_, &codec_, aom_codec_av1_cx(), &codec_enc_);
    EXPECT_EQ(AOM_CODEC_OK, res);
    aom_codec_control(&codec_, AOME_SET_CPUUSED, 4);  // Make the test faster
    aom_codec_control(&codec_, AV1E_SET_TILE_COLUMNS, tile_columns_);
    aom_codec_control(&codec_, AV1E_SET_TILE_ROWS, tile_rows_);
    codec_initialized_ = true;
  }

  void ReleaseEncoder() {
    aom_svc_release(&svc_);
    if (codec_initialized_) aom_codec_destroy(&codec_);
    codec_initialized_ = false;
  }

  void GetStatsData(std::string *const stats_buf) {
    aom_codec_iter_t iter = NULL;
    const aom_codec_cx_pkt_t *cx_pkt;

    while ((cx_pkt = aom_codec_get_cx_data(&codec_, &iter)) != NULL) {
      if (cx_pkt->kind == AOM_CODEC_STATS_PKT) {
        EXPECT_GT(cx_pkt->data.twopass_stats.sz, 0U);
        ASSERT_TRUE(cx_pkt->data.twopass_stats.buf != NULL);
        stats_buf->append(static_cast<char *>(cx_pkt->data.twopass_stats.buf),
                          cx_pkt->data.twopass_stats.sz);
      }
    }
  }

  void Pass1EncodeNFrames(const int n, const int layers,
                          std::string *const stats_buf) {
    aom_codec_err_t res;

    ASSERT_GT(n, 0);
    ASSERT_GT(layers, 0);
    svc_.spatial_layers = layers;
    codec_enc_.g_pass = AOM_RC_FIRST_PASS;
    InitializeEncoder();

    libaom_test::I420VideoSource video(
        test_file_name_, codec_enc_.g_w, codec_enc_.g_h,
        codec_enc_.g_timebase.den, codec_enc_.g_timebase.num, 0, 30);
    video.Begin();

    for (int i = 0; i < n; ++i) {
      res = aom_svc_encode(&svc_, &codec_, video.img(), video.pts(),
                           video.duration(), AOM_DL_GOOD_QUALITY);
      ASSERT_EQ(AOM_CODEC_OK, res);
      GetStatsData(stats_buf);
      video.Next();
    }

    // Flush encoder and test EOS packet.
    res = aom_svc_encode(&svc_, &codec_, NULL, video.pts(), video.duration(),
                         AOM_DL_GOOD_QUALITY);
    ASSERT_EQ(AOM_CODEC_OK, res);
    GetStatsData(stats_buf);

    ReleaseEncoder();
  }

  void StoreFrames(const size_t max_frame_received,
                   struct aom_fixed_buf *const outputs,
                   size_t *const frame_received) {
    aom_codec_iter_t iter = NULL;
    const aom_codec_cx_pkt_t *cx_pkt;

    while ((cx_pkt = aom_codec_get_cx_data(&codec_, &iter)) != NULL) {
      if (cx_pkt->kind == AOM_CODEC_CX_FRAME_PKT) {
        const size_t frame_size = cx_pkt->data.frame.sz;

        EXPECT_GT(frame_size, 0U);
        ASSERT_TRUE(cx_pkt->data.frame.buf != NULL);
        ASSERT_LT(*frame_received, max_frame_received);

        if (*frame_received == 0)
          EXPECT_EQ(1, !!(cx_pkt->data.frame.flags & AOM_FRAME_IS_KEY));

        outputs[*frame_received].buf = malloc(frame_size + 16);
        ASSERT_TRUE(outputs[*frame_received].buf != NULL);
        memcpy(outputs[*frame_received].buf, cx_pkt->data.frame.buf,
               frame_size);
        outputs[*frame_received].sz = frame_size;
        ++(*frame_received);
      }
    }
  }

  void Pass2EncodeNFrames(std::string *const stats_buf, const int n,
                          const int layers,
                          struct aom_fixed_buf *const outputs) {
    aom_codec_err_t res;
    size_t frame_received = 0;

    ASSERT_TRUE(outputs != NULL);
    ASSERT_GT(n, 0);
    ASSERT_GT(layers, 0);
    svc_.spatial_layers = layers;
    codec_enc_.rc_target_bitrate = 500;
    if (codec_enc_.g_pass == AOM_RC_LAST_PASS) {
      ASSERT_TRUE(stats_buf != NULL);
      ASSERT_GT(stats_buf->size(), 0U);
      codec_enc_.rc_twopass_stats_in.buf = &(*stats_buf)[0];
      codec_enc_.rc_twopass_stats_in.sz = stats_buf->size();
    }
    InitializeEncoder();

    libaom_test::I420VideoSource video(
        test_file_name_, codec_enc_.g_w, codec_enc_.g_h,
        codec_enc_.g_timebase.den, codec_enc_.g_timebase.num, 0, 30);
    video.Begin();

    for (int i = 0; i < n; ++i) {
      res = aom_svc_encode(&svc_, &codec_, video.img(), video.pts(),
                           video.duration(), AOM_DL_GOOD_QUALITY);
      ASSERT_EQ(AOM_CODEC_OK, res);
      StoreFrames(n, outputs, &frame_received);
      video.Next();
    }

    // Flush encoder.
    res = aom_svc_encode(&svc_, &codec_, NULL, 0, video.duration(),
                         AOM_DL_GOOD_QUALITY);
    EXPECT_EQ(AOM_CODEC_OK, res);
    StoreFrames(n, outputs, &frame_received);

    EXPECT_EQ(frame_received, static_cast<size_t>(n));

    ReleaseEncoder();
  }

  void DecodeNFrames(const struct aom_fixed_buf *const inputs, const int n) {
    int decoded_frames = 0;
    int received_frames = 0;

    ASSERT_TRUE(inputs != NULL);
    ASSERT_GT(n, 0);

    for (int i = 0; i < n; ++i) {
      ASSERT_TRUE(inputs[i].buf != NULL);
      ASSERT_GT(inputs[i].sz, 0U);
      const aom_codec_err_t res_dec = decoder_->DecodeFrame(
          static_cast<const uint8_t *>(inputs[i].buf), inputs[i].sz);
      ASSERT_EQ(AOM_CODEC_OK, res_dec) << decoder_->DecodeError();
      ++decoded_frames;

      DxDataIterator dec_iter = decoder_->GetDxData();
      while (dec_iter.Next() != NULL) {
        ++received_frames;
      }
    }
    EXPECT_EQ(decoded_frames, n);
    EXPECT_EQ(received_frames, n);
  }

  void DropEnhancementLayers(struct aom_fixed_buf *const inputs,
                             const int num_super_frames,
                             const int remained_spatial_layers) {
    ASSERT_TRUE(inputs != NULL);
    ASSERT_GT(num_super_frames, 0);
    ASSERT_GT(remained_spatial_layers, 0);

    for (int i = 0; i < num_super_frames; ++i) {
      uint32_t frame_sizes[8] = { 0 };
      int frame_count = 0;
      int frames_found = 0;
      int frame;
      ASSERT_TRUE(inputs[i].buf != NULL);
      ASSERT_GT(inputs[i].sz, 0U);

      aom_codec_err_t res = av1_parse_superframe_index(
          static_cast<const uint8_t *>(inputs[i].buf), inputs[i].sz,
          frame_sizes, &frame_count, NULL, NULL);
      ASSERT_EQ(AOM_CODEC_OK, res);

      if (frame_count == 0) {
        // There's no super frame but only a single frame.
        ASSERT_EQ(1, remained_spatial_layers);
      } else {
        // Found a super frame.
        uint8_t *frame_data = static_cast<uint8_t *>(inputs[i].buf);
        uint8_t *frame_start = frame_data;
        for (frame = 0; frame < frame_count; ++frame) {
          // Looking for a visible frame.
          if (frame_data[0] & 0x02) {
            ++frames_found;
            if (frames_found == remained_spatial_layers) break;
          }
          frame_data += frame_sizes[frame];
        }
        ASSERT_LT(frame, frame_count)
            << "Couldn't find a visible frame. "
            << "remained_spatial_layers: " << remained_spatial_layers
            << "    super_frame: " << i;
        if (frame == frame_count - 1) continue;

        frame_data += frame_sizes[frame];

        // We need to add one more frame for multiple frame contexts.
        uint8_t marker =
            static_cast<const uint8_t *>(inputs[i].buf)[inputs[i].sz - 1];
        const uint32_t mag = ((marker >> 3) & 0x3) + 1;
        const size_t index_sz = 2 + mag * frame_count;
        const size_t new_index_sz = 2 + mag * (frame + 1);
        marker &= 0x0f8;
        marker |= frame;

        // Copy existing frame sizes.
        memmove(frame_data + 1, frame_start + inputs[i].sz - index_sz + 1,
                new_index_sz - 2);
        // New marker.
        frame_data[0] = marker;
        frame_data += (mag * (frame + 1) + 1);

        *frame_data++ = marker;
        inputs[i].sz = frame_data - frame_start;
      }
    }
  }

  void FreeBitstreamBuffers(struct aom_fixed_buf *const inputs, const int n) {
    ASSERT_TRUE(inputs != NULL);
    ASSERT_GT(n, 0);

    for (int i = 0; i < n; ++i) {
      free(inputs[i].buf);
      inputs[i].buf = NULL;
      inputs[i].sz = 0;
    }
  }

  SvcContext svc_;
  aom_codec_ctx_t codec_;
  struct aom_codec_enc_cfg codec_enc_;
  aom_codec_iface_t *codec_iface_;
  std::string test_file_name_;
  bool codec_initialized_;
  Decoder *decoder_;
  int tile_columns_;
  int tile_rows_;
};

TEST_F(SvcTest, SvcInit) {
  // test missing parameters
  aom_codec_err_t res = aom_svc_init(NULL, &codec_, codec_iface_, &codec_enc_);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);
  res = aom_svc_init(&svc_, NULL, codec_iface_, &codec_enc_);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);
  res = aom_svc_init(&svc_, &codec_, NULL, &codec_enc_);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);

  res = aom_svc_init(&svc_, &codec_, codec_iface_, NULL);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);

  svc_.spatial_layers = 6;  // too many layers
  res = aom_svc_init(&svc_, &codec_, codec_iface_, &codec_enc_);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);

  svc_.spatial_layers = 0;  // use default layers
  InitializeEncoder();
  EXPECT_EQ(AOM_SS_DEFAULT_LAYERS, svc_.spatial_layers);
}

TEST_F(SvcTest, InitTwoLayers) {
  svc_.spatial_layers = 2;
  InitializeEncoder();
}

TEST_F(SvcTest, InvalidOptions) {
  aom_codec_err_t res = aom_svc_set_options(&svc_, NULL);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);

  res = aom_svc_set_options(&svc_, "not-an-option=1");
  EXPECT_EQ(AOM_CODEC_OK, res);
  res = aom_svc_init(&svc_, &codec_, aom_codec_av1_cx(), &codec_enc_);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);
}

TEST_F(SvcTest, SetLayersOption) {
  aom_codec_err_t res = aom_svc_set_options(&svc_, "spatial-layers=3");
  EXPECT_EQ(AOM_CODEC_OK, res);
  InitializeEncoder();
  EXPECT_EQ(3, svc_.spatial_layers);
}

TEST_F(SvcTest, SetMultipleOptions) {
  aom_codec_err_t res =
      aom_svc_set_options(&svc_, "spatial-layers=2 scale-factors=1/3,2/3");
  EXPECT_EQ(AOM_CODEC_OK, res);
  InitializeEncoder();
  EXPECT_EQ(2, svc_.spatial_layers);
}

TEST_F(SvcTest, SetScaleFactorsOption) {
  svc_.spatial_layers = 2;
  aom_codec_err_t res =
      aom_svc_set_options(&svc_, "scale-factors=not-scale-factors");
  EXPECT_EQ(AOM_CODEC_OK, res);
  res = aom_svc_init(&svc_, &codec_, aom_codec_av1_cx(), &codec_enc_);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);

  res = aom_svc_set_options(&svc_, "scale-factors=1/3, 3*3");
  EXPECT_EQ(AOM_CODEC_OK, res);
  res = aom_svc_init(&svc_, &codec_, aom_codec_av1_cx(), &codec_enc_);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);

  res = aom_svc_set_options(&svc_, "scale-factors=1/3");
  EXPECT_EQ(AOM_CODEC_OK, res);
  res = aom_svc_init(&svc_, &codec_, aom_codec_av1_cx(), &codec_enc_);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);

  res = aom_svc_set_options(&svc_, "scale-factors=1/3,2/3");
  EXPECT_EQ(AOM_CODEC_OK, res);
  InitializeEncoder();
}

TEST_F(SvcTest, SetQuantizersOption) {
  svc_.spatial_layers = 2;
  aom_codec_err_t res = aom_svc_set_options(&svc_, "max-quantizers=nothing");
  EXPECT_EQ(AOM_CODEC_OK, res);
  res = aom_svc_init(&svc_, &codec_, aom_codec_av1_cx(), &codec_enc_);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);

  res = aom_svc_set_options(&svc_, "min-quantizers=nothing");
  EXPECT_EQ(AOM_CODEC_OK, res);
  res = aom_svc_init(&svc_, &codec_, aom_codec_av1_cx(), &codec_enc_);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);

  res = aom_svc_set_options(&svc_, "max-quantizers=40");
  EXPECT_EQ(AOM_CODEC_OK, res);
  res = aom_svc_init(&svc_, &codec_, aom_codec_av1_cx(), &codec_enc_);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);

  res = aom_svc_set_options(&svc_, "min-quantizers=40");
  EXPECT_EQ(AOM_CODEC_OK, res);
  res = aom_svc_init(&svc_, &codec_, aom_codec_av1_cx(), &codec_enc_);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);

  res = aom_svc_set_options(&svc_, "max-quantizers=30,30 min-quantizers=40,40");
  EXPECT_EQ(AOM_CODEC_OK, res);
  res = aom_svc_init(&svc_, &codec_, aom_codec_av1_cx(), &codec_enc_);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);

  res = aom_svc_set_options(&svc_, "max-quantizers=40,40 min-quantizers=30,30");
  InitializeEncoder();
}

TEST_F(SvcTest, SetAutoAltRefOption) {
  svc_.spatial_layers = 5;
  aom_codec_err_t res = aom_svc_set_options(&svc_, "auto-alt-refs=none");
  EXPECT_EQ(AOM_CODEC_OK, res);
  res = aom_svc_init(&svc_, &codec_, aom_codec_av1_cx(), &codec_enc_);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);

  res = aom_svc_set_options(&svc_, "auto-alt-refs=1,1,1,1,0");
  EXPECT_EQ(AOM_CODEC_OK, res);
  res = aom_svc_init(&svc_, &codec_, aom_codec_av1_cx(), &codec_enc_);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);

  aom_svc_set_options(&svc_, "auto-alt-refs=0,1,1,1,0");
  InitializeEncoder();
}

// Test that decoder can handle an SVC frame as the first frame in a sequence.
TEST_F(SvcTest, OnePassEncodeOneFrame) {
  codec_enc_.g_pass = AOM_RC_ONE_PASS;
  aom_fixed_buf output = { 0 };
  Pass2EncodeNFrames(NULL, 1, 2, &output);
  DecodeNFrames(&output, 1);
  FreeBitstreamBuffers(&output, 1);
}

TEST_F(SvcTest, OnePassEncodeThreeFrames) {
  codec_enc_.g_pass = AOM_RC_ONE_PASS;
  codec_enc_.g_lag_in_frames = 0;
  aom_fixed_buf outputs[3];
  memset(&outputs[0], 0, sizeof(outputs));
  Pass2EncodeNFrames(NULL, 3, 2, &outputs[0]);
  DecodeNFrames(&outputs[0], 3);
  FreeBitstreamBuffers(&outputs[0], 3);
}

TEST_F(SvcTest, TwoPassEncode10Frames) {
  // First pass encode
  std::string stats_buf;
  Pass1EncodeNFrames(10, 2, &stats_buf);

  // Second pass encode
  codec_enc_.g_pass = AOM_RC_LAST_PASS;
  aom_fixed_buf outputs[10];
  memset(&outputs[0], 0, sizeof(outputs));
  Pass2EncodeNFrames(&stats_buf, 10, 2, &outputs[0]);
  DecodeNFrames(&outputs[0], 10);
  FreeBitstreamBuffers(&outputs[0], 10);
}

TEST_F(SvcTest, TwoPassEncode20FramesWithAltRef) {
  // First pass encode
  std::string stats_buf;
  Pass1EncodeNFrames(20, 2, &stats_buf);

  // Second pass encode
  codec_enc_.g_pass = AOM_RC_LAST_PASS;
  aom_svc_set_options(&svc_, "auto-alt-refs=1,1");
  aom_fixed_buf outputs[20];
  memset(&outputs[0], 0, sizeof(outputs));
  Pass2EncodeNFrames(&stats_buf, 20, 2, &outputs[0]);
  DecodeNFrames(&outputs[0], 20);
  FreeBitstreamBuffers(&outputs[0], 20);
}

TEST_F(SvcTest, TwoPassEncode2SpatialLayersDecodeBaseLayerOnly) {
  // First pass encode
  std::string stats_buf;
  Pass1EncodeNFrames(10, 2, &stats_buf);

  // Second pass encode
  codec_enc_.g_pass = AOM_RC_LAST_PASS;
  aom_svc_set_options(&svc_, "auto-alt-refs=1,1");
  aom_fixed_buf outputs[10];
  memset(&outputs[0], 0, sizeof(outputs));
  Pass2EncodeNFrames(&stats_buf, 10, 2, &outputs[0]);
  DropEnhancementLayers(&outputs[0], 10, 1);
  DecodeNFrames(&outputs[0], 10);
  FreeBitstreamBuffers(&outputs[0], 10);
}

TEST_F(SvcTest, TwoPassEncode5SpatialLayersDecode54321Layers) {
  // First pass encode
  std::string stats_buf;
  Pass1EncodeNFrames(10, 5, &stats_buf);

  // Second pass encode
  codec_enc_.g_pass = AOM_RC_LAST_PASS;
  aom_svc_set_options(&svc_, "auto-alt-refs=0,1,1,1,0");
  aom_fixed_buf outputs[10];
  memset(&outputs[0], 0, sizeof(outputs));
  Pass2EncodeNFrames(&stats_buf, 10, 5, &outputs[0]);

  DecodeNFrames(&outputs[0], 10);
  DropEnhancementLayers(&outputs[0], 10, 4);
  DecodeNFrames(&outputs[0], 10);
  DropEnhancementLayers(&outputs[0], 10, 3);
  DecodeNFrames(&outputs[0], 10);
  DropEnhancementLayers(&outputs[0], 10, 2);
  DecodeNFrames(&outputs[0], 10);
  DropEnhancementLayers(&outputs[0], 10, 1);
  DecodeNFrames(&outputs[0], 10);

  FreeBitstreamBuffers(&outputs[0], 10);
}

TEST_F(SvcTest, TwoPassEncode2SNRLayers) {
  // First pass encode
  std::string stats_buf;
  aom_svc_set_options(&svc_, "scale-factors=1/1,1/1");
  Pass1EncodeNFrames(20, 2, &stats_buf);

  // Second pass encode
  codec_enc_.g_pass = AOM_RC_LAST_PASS;
  aom_svc_set_options(&svc_, "auto-alt-refs=1,1 scale-factors=1/1,1/1");
  aom_fixed_buf outputs[20];
  memset(&outputs[0], 0, sizeof(outputs));
  Pass2EncodeNFrames(&stats_buf, 20, 2, &outputs[0]);
  DecodeNFrames(&outputs[0], 20);
  FreeBitstreamBuffers(&outputs[0], 20);
}

TEST_F(SvcTest, TwoPassEncode3SNRLayersDecode321Layers) {
  // First pass encode
  std::string stats_buf;
  aom_svc_set_options(&svc_, "scale-factors=1/1,1/1,1/1");
  Pass1EncodeNFrames(20, 3, &stats_buf);

  // Second pass encode
  codec_enc_.g_pass = AOM_RC_LAST_PASS;
  aom_svc_set_options(&svc_, "auto-alt-refs=1,1,1 scale-factors=1/1,1/1,1/1");
  aom_fixed_buf outputs[20];
  memset(&outputs[0], 0, sizeof(outputs));
  Pass2EncodeNFrames(&stats_buf, 20, 3, &outputs[0]);
  DecodeNFrames(&outputs[0], 20);
  DropEnhancementLayers(&outputs[0], 20, 2);
  DecodeNFrames(&outputs[0], 20);
  DropEnhancementLayers(&outputs[0], 20, 1);
  DecodeNFrames(&outputs[0], 20);

  FreeBitstreamBuffers(&outputs[0], 20);
}

TEST_F(SvcTest, SetMultipleFrameContextsOption) {
  svc_.spatial_layers = 5;
  aom_codec_err_t res = aom_svc_set_options(&svc_, "multi-frame-contexts=1");
  EXPECT_EQ(AOM_CODEC_OK, res);
  res = aom_svc_init(&svc_, &codec_, aom_codec_av1_cx(), &codec_enc_);
  EXPECT_EQ(AOM_CODEC_INVALID_PARAM, res);

  svc_.spatial_layers = 2;
  res = aom_svc_set_options(&svc_, "multi-frame-contexts=1");
  InitializeEncoder();
}

TEST_F(SvcTest, TwoPassEncode2SpatialLayersWithMultipleFrameContexts) {
  // First pass encode
  std::string stats_buf;
  Pass1EncodeNFrames(10, 2, &stats_buf);

  // Second pass encode
  codec_enc_.g_pass = AOM_RC_LAST_PASS;
  codec_enc_.g_error_resilient = 0;
  aom_svc_set_options(&svc_, "auto-alt-refs=1,1 multi-frame-contexts=1");
  aom_fixed_buf outputs[10];
  memset(&outputs[0], 0, sizeof(outputs));
  Pass2EncodeNFrames(&stats_buf, 10, 2, &outputs[0]);
  DecodeNFrames(&outputs[0], 10);
  FreeBitstreamBuffers(&outputs[0], 10);
}

TEST_F(SvcTest,
       TwoPassEncode2SpatialLayersWithMultipleFrameContextsDecodeBaselayer) {
  // First pass encode
  std::string stats_buf;
  Pass1EncodeNFrames(10, 2, &stats_buf);

  // Second pass encode
  codec_enc_.g_pass = AOM_RC_LAST_PASS;
  codec_enc_.g_error_resilient = 0;
  aom_svc_set_options(&svc_, "auto-alt-refs=1,1 multi-frame-contexts=1");
  aom_fixed_buf outputs[10];
  memset(&outputs[0], 0, sizeof(outputs));
  Pass2EncodeNFrames(&stats_buf, 10, 2, &outputs[0]);
  DropEnhancementLayers(&outputs[0], 10, 1);
  DecodeNFrames(&outputs[0], 10);
  FreeBitstreamBuffers(&outputs[0], 10);
}

TEST_F(SvcTest, TwoPassEncode2SNRLayersWithMultipleFrameContexts) {
  // First pass encode
  std::string stats_buf;
  aom_svc_set_options(&svc_, "scale-factors=1/1,1/1");
  Pass1EncodeNFrames(10, 2, &stats_buf);

  // Second pass encode
  codec_enc_.g_pass = AOM_RC_LAST_PASS;
  codec_enc_.g_error_resilient = 0;
  aom_svc_set_options(&svc_,
                      "auto-alt-refs=1,1 scale-factors=1/1,1/1 "
                      "multi-frame-contexts=1");
  aom_fixed_buf outputs[10];
  memset(&outputs[0], 0, sizeof(outputs));
  Pass2EncodeNFrames(&stats_buf, 10, 2, &outputs[0]);
  DecodeNFrames(&outputs[0], 10);
  FreeBitstreamBuffers(&outputs[0], 10);
}

TEST_F(SvcTest,
       TwoPassEncode3SNRLayersWithMultipleFrameContextsDecode321Layer) {
  // First pass encode
  std::string stats_buf;
  aom_svc_set_options(&svc_, "scale-factors=1/1,1/1,1/1");
  Pass1EncodeNFrames(10, 3, &stats_buf);

  // Second pass encode
  codec_enc_.g_pass = AOM_RC_LAST_PASS;
  codec_enc_.g_error_resilient = 0;
  aom_svc_set_options(&svc_,
                      "auto-alt-refs=1,1,1 scale-factors=1/1,1/1,1/1 "
                      "multi-frame-contexts=1");
  aom_fixed_buf outputs[10];
  memset(&outputs[0], 0, sizeof(outputs));
  Pass2EncodeNFrames(&stats_buf, 10, 3, &outputs[0]);

  DecodeNFrames(&outputs[0], 10);
  DropEnhancementLayers(&outputs[0], 10, 2);
  DecodeNFrames(&outputs[0], 10);
  DropEnhancementLayers(&outputs[0], 10, 1);
  DecodeNFrames(&outputs[0], 10);

  FreeBitstreamBuffers(&outputs[0], 10);
}

TEST_F(SvcTest, TwoPassEncode2TemporalLayers) {
  // First pass encode
  std::string stats_buf;
  aom_svc_set_options(&svc_, "scale-factors=1/1");
  svc_.temporal_layers = 2;
  Pass1EncodeNFrames(10, 1, &stats_buf);

  // Second pass encode
  codec_enc_.g_pass = AOM_RC_LAST_PASS;
  svc_.temporal_layers = 2;
  aom_svc_set_options(&svc_, "auto-alt-refs=1 scale-factors=1/1");
  aom_fixed_buf outputs[10];
  memset(&outputs[0], 0, sizeof(outputs));
  Pass2EncodeNFrames(&stats_buf, 10, 1, &outputs[0]);
  DecodeNFrames(&outputs[0], 10);
  FreeBitstreamBuffers(&outputs[0], 10);
}

TEST_F(SvcTest, TwoPassEncode2TemporalLayersWithMultipleFrameContexts) {
  // First pass encode
  std::string stats_buf;
  aom_svc_set_options(&svc_, "scale-factors=1/1");
  svc_.temporal_layers = 2;
  Pass1EncodeNFrames(10, 1, &stats_buf);

  // Second pass encode
  codec_enc_.g_pass = AOM_RC_LAST_PASS;
  svc_.temporal_layers = 2;
  codec_enc_.g_error_resilient = 0;
  aom_svc_set_options(&svc_,
                      "auto-alt-refs=1 scale-factors=1/1 "
                      "multi-frame-contexts=1");
  aom_fixed_buf outputs[10];
  memset(&outputs[0], 0, sizeof(outputs));
  Pass2EncodeNFrames(&stats_buf, 10, 1, &outputs[0]);
  DecodeNFrames(&outputs[0], 10);
  FreeBitstreamBuffers(&outputs[0], 10);
}

TEST_F(SvcTest, TwoPassEncode2TemporalLayersDecodeBaseLayer) {
  // First pass encode
  std::string stats_buf;
  aom_svc_set_options(&svc_, "scale-factors=1/1");
  svc_.temporal_layers = 2;
  Pass1EncodeNFrames(10, 1, &stats_buf);

  // Second pass encode
  codec_enc_.g_pass = AOM_RC_LAST_PASS;
  svc_.temporal_layers = 2;
  aom_svc_set_options(&svc_, "auto-alt-refs=1 scale-factors=1/1");
  aom_fixed_buf outputs[10];
  memset(&outputs[0], 0, sizeof(outputs));
  Pass2EncodeNFrames(&stats_buf, 10, 1, &outputs[0]);

  aom_fixed_buf base_layer[5];
  for (int i = 0; i < 5; ++i) base_layer[i] = outputs[i * 2];

  DecodeNFrames(&base_layer[0], 5);
  FreeBitstreamBuffers(&outputs[0], 10);
}

TEST_F(SvcTest,
       TwoPassEncode2TemporalLayersWithMultipleFrameContextsDecodeBaseLayer) {
  // First pass encode
  std::string stats_buf;
  aom_svc_set_options(&svc_, "scale-factors=1/1");
  svc_.temporal_layers = 2;
  Pass1EncodeNFrames(10, 1, &stats_buf);

  // Second pass encode
  codec_enc_.g_pass = AOM_RC_LAST_PASS;
  svc_.temporal_layers = 2;
  codec_enc_.g_error_resilient = 0;
  aom_svc_set_options(&svc_,
                      "auto-alt-refs=1 scale-factors=1/1 "
                      "multi-frame-contexts=1");
  aom_fixed_buf outputs[10];
  memset(&outputs[0], 0, sizeof(outputs));
  Pass2EncodeNFrames(&stats_buf, 10, 1, &outputs[0]);

  aom_fixed_buf base_layer[5];
  for (int i = 0; i < 5; ++i) base_layer[i] = outputs[i * 2];

  DecodeNFrames(&base_layer[0], 5);
  FreeBitstreamBuffers(&outputs[0], 10);
}

TEST_F(SvcTest, TwoPassEncode2TemporalLayersWithTiles) {
  // First pass encode
  std::string stats_buf;
  aom_svc_set_options(&svc_, "scale-factors=1/1");
  svc_.temporal_layers = 2;
  Pass1EncodeNFrames(10, 1, &stats_buf);

  // Second pass encode
  codec_enc_.g_pass = AOM_RC_LAST_PASS;
  svc_.temporal_layers = 2;
  aom_svc_set_options(&svc_, "auto-alt-refs=1 scale-factors=1/1");
  codec_enc_.g_w = 704;
  codec_enc_.g_h = 144;
  tile_columns_ = 1;
  tile_rows_ = 1;
  aom_fixed_buf outputs[10];
  memset(&outputs[0], 0, sizeof(outputs));
  Pass2EncodeNFrames(&stats_buf, 10, 1, &outputs[0]);
  DecodeNFrames(&outputs[0], 10);
  FreeBitstreamBuffers(&outputs[0], 10);
}

TEST_F(SvcTest, TwoPassEncode2TemporalLayersWithMultipleFrameContextsAndTiles) {
  // First pass encode
  std::string stats_buf;
  aom_svc_set_options(&svc_, "scale-factors=1/1");
  svc_.temporal_layers = 2;
  Pass1EncodeNFrames(10, 1, &stats_buf);

  // Second pass encode
  codec_enc_.g_pass = AOM_RC_LAST_PASS;
  svc_.temporal_layers = 2;
  codec_enc_.g_error_resilient = 0;
  codec_enc_.g_w = 704;
  codec_enc_.g_h = 144;
  tile_columns_ = 1;
  tile_rows_ = 1;
  aom_svc_set_options(&svc_,
                      "auto-alt-refs=1 scale-factors=1/1 "
                      "multi-frame-contexts=1");
  aom_fixed_buf outputs[10];
  memset(&outputs[0], 0, sizeof(outputs));
  Pass2EncodeNFrames(&stats_buf, 10, 1, &outputs[0]);
  DecodeNFrames(&outputs[0], 10);
  FreeBitstreamBuffers(&outputs[0], 10);
}

}  // namespace
