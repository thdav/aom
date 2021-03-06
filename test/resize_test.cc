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

#include <climits>
#include <vector>
#include "third_party/googletest/src/include/gtest/gtest.h"
#include "test/codec_factory.h"
#include "test/encode_test_driver.h"
#include "test/i420_video_source.h"
#include "test/video_source.h"
#include "test/util.h"

// Enable(1) or Disable(0) writing of the compressed bitstream.
#define WRITE_COMPRESSED_STREAM 0

namespace {

#if WRITE_COMPRESSED_STREAM
static void mem_put_le16(char *const mem, const unsigned int val) {
  mem[0] = val;
  mem[1] = val >> 8;
}

static void mem_put_le32(char *const mem, const unsigned int val) {
  mem[0] = val;
  mem[1] = val >> 8;
  mem[2] = val >> 16;
  mem[3] = val >> 24;
}

static void write_ivf_file_header(const aom_codec_enc_cfg_t *const cfg,
                                  int frame_cnt, FILE *const outfile) {
  char header[32];

  header[0] = 'D';
  header[1] = 'K';
  header[2] = 'I';
  header[3] = 'F';
  mem_put_le16(header + 4, 0);                    /* version */
  mem_put_le16(header + 6, 32);                   /* headersize */
  mem_put_le32(header + 8, 0x30395056);           /* fourcc (av1) */
  mem_put_le16(header + 12, cfg->g_w);            /* width */
  mem_put_le16(header + 14, cfg->g_h);            /* height */
  mem_put_le32(header + 16, cfg->g_timebase.den); /* rate */
  mem_put_le32(header + 20, cfg->g_timebase.num); /* scale */
  mem_put_le32(header + 24, frame_cnt);           /* length */
  mem_put_le32(header + 28, 0);                   /* unused */

  (void)fwrite(header, 1, 32, outfile);
}

static void write_ivf_frame_size(FILE *const outfile, const size_t size) {
  char header[4];
  mem_put_le32(header, static_cast<unsigned int>(size));
  (void)fwrite(header, 1, 4, outfile);
}

static void write_ivf_frame_header(const aom_codec_cx_pkt_t *const pkt,
                                   FILE *const outfile) {
  char header[12];
  aom_codec_pts_t pts;

  if (pkt->kind != AOM_CODEC_CX_FRAME_PKT) return;

  pts = pkt->data.frame.pts;
  mem_put_le32(header, static_cast<unsigned int>(pkt->data.frame.sz));
  mem_put_le32(header + 4, pts & 0xFFFFFFFF);
  mem_put_le32(header + 8, pts >> 32);

  (void)fwrite(header, 1, 12, outfile);
}
#endif  // WRITE_COMPRESSED_STREAM

const unsigned int kInitialWidth = 320;
const unsigned int kInitialHeight = 240;

struct FrameInfo {
  FrameInfo(aom_codec_pts_t _pts, unsigned int _w, unsigned int _h)
      : pts(_pts), w(_w), h(_h) {}

  aom_codec_pts_t pts;
  unsigned int w;
  unsigned int h;
};

unsigned int ScaleForFrameNumber(unsigned int frame, unsigned int val) {
  if (frame < 10) return val;
  if (frame < 20) return val / 2;
  if (frame < 30) return val * 2 / 3;
  if (frame < 40) return val / 4;
  if (frame < 50) return val * 7 / 8;
  return val;
}

class ResizingVideoSource : public ::libaom_test::DummyVideoSource {
 public:
  ResizingVideoSource() {
    SetSize(kInitialWidth, kInitialHeight);
    limit_ = 60;
  }

  virtual ~ResizingVideoSource() {}

 protected:
  virtual void Next() {
    ++frame_;
    SetSize(ScaleForFrameNumber(frame_, kInitialWidth),
            ScaleForFrameNumber(frame_, kInitialHeight));
    FillFrame();
  }
};

class ResizeTest
    : public ::libaom_test::EncoderTest,
      public ::libaom_test::CodecTestWithParam<libaom_test::TestMode> {
 protected:
  ResizeTest() : EncoderTest(GET_PARAM(0)) {}

  virtual ~ResizeTest() {}

  virtual void SetUp() {
    InitializeConfig();
    SetMode(GET_PARAM(1));
  }

  virtual void DecompressedFrameHook(const aom_image_t &img,
                                     aom_codec_pts_t pts) {
    frame_info_list_.push_back(FrameInfo(pts, img.d_w, img.d_h));
  }

  std::vector<FrameInfo> frame_info_list_;
};

TEST_P(ResizeTest, TestExternalResizeWorks) {
  ResizingVideoSource video;
  cfg_.g_lag_in_frames = 0;
  ASSERT_NO_FATAL_FAILURE(RunLoop(&video));

  for (std::vector<FrameInfo>::const_iterator info = frame_info_list_.begin();
       info != frame_info_list_.end(); ++info) {
    const unsigned int frame = static_cast<unsigned>(info->pts);
    const unsigned int expected_w = ScaleForFrameNumber(frame, kInitialWidth);
    const unsigned int expected_h = ScaleForFrameNumber(frame, kInitialHeight);

    EXPECT_EQ(expected_w, info->w) << "Frame " << frame
                                   << " had unexpected width";
    EXPECT_EQ(expected_h, info->h) << "Frame " << frame
                                   << " had unexpected height";
  }
}

const unsigned int kStepDownFrame = 3;
const unsigned int kStepUpFrame = 6;

class ResizeInternalTest : public ResizeTest {
 protected:
#if WRITE_COMPRESSED_STREAM
  ResizeInternalTest()
      : ResizeTest(), frame0_psnr_(0.0), outfile_(NULL), out_frames_(0) {}
#else
  ResizeInternalTest() : ResizeTest(), frame0_psnr_(0.0) {}
#endif

  virtual ~ResizeInternalTest() {}

  virtual void BeginPassHook(unsigned int /*pass*/) {
#if WRITE_COMPRESSED_STREAM
    outfile_ = fopen("av10-2-05-resize.ivf", "wb");
#endif
  }

  virtual void EndPassHook() {
#if WRITE_COMPRESSED_STREAM
    if (outfile_) {
      if (!fseek(outfile_, 0, SEEK_SET))
        write_ivf_file_header(&cfg_, out_frames_, outfile_);
      fclose(outfile_);
      outfile_ = NULL;
    }
#endif
  }

  virtual void PreEncodeFrameHook(libaom_test::VideoSource *video,
                                  libaom_test::Encoder *encoder) {
    if (change_config_) {
      int new_q = 60;
      if (video->frame() == 0) {
        struct aom_scaling_mode mode = { AOME_ONETWO, AOME_ONETWO };
        encoder->Control(AOME_SET_SCALEMODE, &mode);
      }
      if (video->frame() == 1) {
        struct aom_scaling_mode mode = { AOME_NORMAL, AOME_NORMAL };
        encoder->Control(AOME_SET_SCALEMODE, &mode);
        cfg_.rc_min_quantizer = cfg_.rc_max_quantizer = new_q;
        encoder->Config(&cfg_);
      }
    } else {
      if (video->frame() == kStepDownFrame) {
        struct aom_scaling_mode mode = { AOME_FOURFIVE, AOME_THREEFIVE };
        encoder->Control(AOME_SET_SCALEMODE, &mode);
      }
      if (video->frame() == kStepUpFrame) {
        struct aom_scaling_mode mode = { AOME_NORMAL, AOME_NORMAL };
        encoder->Control(AOME_SET_SCALEMODE, &mode);
      }
    }
  }

  virtual void PSNRPktHook(const aom_codec_cx_pkt_t *pkt) {
    if (!frame0_psnr_) frame0_psnr_ = pkt->data.psnr.psnr[0];
    EXPECT_NEAR(pkt->data.psnr.psnr[0], frame0_psnr_, 2.0);
  }

#if WRITE_COMPRESSED_STREAM
  virtual void FramePktHook(const aom_codec_cx_pkt_t *pkt) {
    ++out_frames_;

    // Write initial file header if first frame.
    if (pkt->data.frame.pts == 0) write_ivf_file_header(&cfg_, 0, outfile_);

    // Write frame header and data.
    write_ivf_frame_header(pkt, outfile_);
    (void)fwrite(pkt->data.frame.buf, 1, pkt->data.frame.sz, outfile_);
  }
#endif

  double frame0_psnr_;
  bool change_config_;
#if WRITE_COMPRESSED_STREAM
  FILE *outfile_;
  unsigned int out_frames_;
#endif
};

TEST_P(ResizeInternalTest, TestInternalResizeWorks) {
  ::libaom_test::I420VideoSource video("hantro_collage_w352h288.yuv", 352, 288,
                                       30, 1, 0, 10);
  init_flags_ = AOM_CODEC_USE_PSNR;
  change_config_ = false;

  // q picked such that initial keyframe on this clip is ~30dB PSNR
  cfg_.rc_min_quantizer = cfg_.rc_max_quantizer = 48;

  // If the number of frames being encoded is smaller than g_lag_in_frames
  // the encoded frame is unavailable using the current API. Comparing
  // frames to detect mismatch would then not be possible. Set
  // g_lag_in_frames = 0 to get around this.
  cfg_.g_lag_in_frames = 0;
  ASSERT_NO_FATAL_FAILURE(RunLoop(&video));

  for (std::vector<FrameInfo>::const_iterator info = frame_info_list_.begin();
       info != frame_info_list_.end(); ++info) {
    const aom_codec_pts_t pts = info->pts;
    if (pts >= kStepDownFrame && pts < kStepUpFrame) {
      ASSERT_EQ(282U, info->w) << "Frame " << pts << " had unexpected width";
      ASSERT_EQ(173U, info->h) << "Frame " << pts << " had unexpected height";
    } else {
      EXPECT_EQ(352U, info->w) << "Frame " << pts << " had unexpected width";
      EXPECT_EQ(288U, info->h) << "Frame " << pts << " had unexpected height";
    }
  }
}

TEST_P(ResizeInternalTest, TestInternalResizeChangeConfig) {
  ::libaom_test::I420VideoSource video("hantro_collage_w352h288.yuv", 352, 288,
                                       30, 1, 0, 10);
  cfg_.g_w = 352;
  cfg_.g_h = 288;
  change_config_ = true;
  ASSERT_NO_FATAL_FAILURE(RunLoop(&video));
}

class ResizeRealtimeTest
    : public ::libaom_test::EncoderTest,
      public ::libaom_test::CodecTestWith2Params<libaom_test::TestMode, int> {
 protected:
  ResizeRealtimeTest() : EncoderTest(GET_PARAM(0)) {}
  virtual ~ResizeRealtimeTest() {}

  virtual void PreEncodeFrameHook(libaom_test::VideoSource *video,
                                  libaom_test::Encoder *encoder) {
    if (video->frame() == 0) {
      encoder->Control(AV1E_SET_AQ_MODE, 3);
      encoder->Control(AOME_SET_CPUUSED, set_cpu_used_);
    }

    if (change_bitrate_ && video->frame() == 120) {
      change_bitrate_ = false;
      cfg_.rc_target_bitrate = 500;
      encoder->Config(&cfg_);
    }
  }

  virtual void SetUp() {
    InitializeConfig();
    SetMode(GET_PARAM(1));
    set_cpu_used_ = GET_PARAM(2);
  }

  virtual void DecompressedFrameHook(const aom_image_t &img,
                                     aom_codec_pts_t pts) {
    frame_info_list_.push_back(FrameInfo(pts, img.d_w, img.d_h));
  }

  void DefaultConfig() {
    cfg_.rc_buf_initial_sz = 500;
    cfg_.rc_buf_optimal_sz = 600;
    cfg_.rc_buf_sz = 1000;
    cfg_.rc_min_quantizer = 2;
    cfg_.rc_max_quantizer = 56;
    cfg_.rc_undershoot_pct = 50;
    cfg_.rc_overshoot_pct = 50;
    cfg_.rc_end_usage = AOM_CBR;
    cfg_.kf_mode = AOM_KF_AUTO;
    cfg_.g_lag_in_frames = 0;
    cfg_.kf_min_dist = cfg_.kf_max_dist = 3000;
    // Enable dropped frames.
    cfg_.rc_dropframe_thresh = 1;
    // Enable error_resilience mode.
    cfg_.g_error_resilient = 1;
    // Enable dynamic resizing.
    cfg_.rc_resize_allowed = 1;
    // Run at low bitrate.
    cfg_.rc_target_bitrate = 200;
  }

  std::vector<FrameInfo> frame_info_list_;
  int set_cpu_used_;
  bool change_bitrate_;
};

TEST_P(ResizeRealtimeTest, TestExternalResizeWorks) {
  ResizingVideoSource video;
  DefaultConfig();
  change_bitrate_ = false;
  ASSERT_NO_FATAL_FAILURE(RunLoop(&video));

  for (std::vector<FrameInfo>::const_iterator info = frame_info_list_.begin();
       info != frame_info_list_.end(); ++info) {
    const unsigned int frame = static_cast<unsigned>(info->pts);
    const unsigned int expected_w = ScaleForFrameNumber(frame, kInitialWidth);
    const unsigned int expected_h = ScaleForFrameNumber(frame, kInitialHeight);

    EXPECT_EQ(expected_w, info->w) << "Frame " << frame
                                   << " had unexpected width";
    EXPECT_EQ(expected_h, info->h) << "Frame " << frame
                                   << " had unexpected height";
  }
}

// Verify the dynamic resizer behavior for real time, 1 pass CBR mode.
// Run at low bitrate, with resize_allowed = 1, and verify that we get
// one resize down event.
TEST_P(ResizeRealtimeTest, TestInternalResizeDown) {
  ::libaom_test::I420VideoSource video("hantro_collage_w352h288.yuv", 352, 288,
                                       30, 1, 0, 299);
  DefaultConfig();
  cfg_.g_w = 352;
  cfg_.g_h = 288;
  change_bitrate_ = false;
  ASSERT_NO_FATAL_FAILURE(RunLoop(&video));

  unsigned int last_w = cfg_.g_w;
  unsigned int last_h = cfg_.g_h;
  int resize_count = 0;
  for (std::vector<FrameInfo>::const_iterator info = frame_info_list_.begin();
       info != frame_info_list_.end(); ++info) {
    if (info->w != last_w || info->h != last_h) {
      // Verify that resize down occurs.
      ASSERT_LT(info->w, last_w);
      ASSERT_LT(info->h, last_h);
      last_w = info->w;
      last_h = info->h;
      resize_count++;
    }
  }

  // Verify that we get 1 resize down event in this test.
  ASSERT_EQ(1, resize_count) << "Resizing should occur.";
}

// Verify the dynamic resizer behavior for real time, 1 pass CBR mode.
// Start at low target bitrate, raise the bitrate in the middle of the clip,
// scaling-up should occur after bitrate changed.
TEST_P(ResizeRealtimeTest, TestInternalResizeDownUpChangeBitRate) {
  ::libaom_test::I420VideoSource video("hantro_collage_w352h288.yuv", 352, 288,
                                       30, 1, 0, 359);
  DefaultConfig();
  cfg_.g_w = 352;
  cfg_.g_h = 288;
  change_bitrate_ = true;
  // Disable dropped frames.
  cfg_.rc_dropframe_thresh = 0;
  // Starting bitrate low.
  cfg_.rc_target_bitrate = 80;
  ASSERT_NO_FATAL_FAILURE(RunLoop(&video));

  unsigned int last_w = cfg_.g_w;
  unsigned int last_h = cfg_.g_h;
  int resize_count = 0;
  for (std::vector<FrameInfo>::const_iterator info = frame_info_list_.begin();
       info != frame_info_list_.end(); ++info) {
    if (info->w != last_w || info->h != last_h) {
      resize_count++;
      if (resize_count == 1) {
        // Verify that resize down occurs.
        ASSERT_LT(info->w, last_w);
        ASSERT_LT(info->h, last_h);
      } else if (resize_count == 2) {
        // Verify that resize up occurs.
        ASSERT_GT(info->w, last_w);
        ASSERT_GT(info->h, last_h);
      }
      last_w = info->w;
      last_h = info->h;
    }
  }

  // Verify that we get 2 resize events in this test.
  ASSERT_EQ(resize_count, 2) << "Resizing should occur twice.";
}

aom_img_fmt_t CspForFrameNumber(int frame) {
  if (frame < 10) return AOM_IMG_FMT_I420;
  if (frame < 20) return AOM_IMG_FMT_I444;
  return AOM_IMG_FMT_I420;
}

class ResizeCspTest : public ResizeTest {
 protected:
#if WRITE_COMPRESSED_STREAM
  ResizeCspTest()
      : ResizeTest(), frame0_psnr_(0.0), outfile_(NULL), out_frames_(0) {}
#else
  ResizeCspTest() : ResizeTest(), frame0_psnr_(0.0) {}
#endif

  virtual ~ResizeCspTest() {}

  virtual void BeginPassHook(unsigned int /*pass*/) {
#if WRITE_COMPRESSED_STREAM
    outfile_ = fopen("av11-2-05-cspchape.ivf", "wb");
#endif
  }

  virtual void EndPassHook() {
#if WRITE_COMPRESSED_STREAM
    if (outfile_) {
      if (!fseek(outfile_, 0, SEEK_SET))
        write_ivf_file_header(&cfg_, out_frames_, outfile_);
      fclose(outfile_);
      outfile_ = NULL;
    }
#endif
  }

  virtual void PreEncodeFrameHook(libaom_test::VideoSource *video,
                                  libaom_test::Encoder *encoder) {
    if (CspForFrameNumber(video->frame()) != AOM_IMG_FMT_I420 &&
        cfg_.g_profile != 1) {
      cfg_.g_profile = 1;
      encoder->Config(&cfg_);
    }
    if (CspForFrameNumber(video->frame()) == AOM_IMG_FMT_I420 &&
        cfg_.g_profile != 0) {
      cfg_.g_profile = 0;
      encoder->Config(&cfg_);
    }
  }

  virtual void PSNRPktHook(const aom_codec_cx_pkt_t *pkt) {
    if (!frame0_psnr_) frame0_psnr_ = pkt->data.psnr.psnr[0];
    EXPECT_NEAR(pkt->data.psnr.psnr[0], frame0_psnr_, 2.0);
  }

#if WRITE_COMPRESSED_STREAM
  virtual void FramePktHook(const aom_codec_cx_pkt_t *pkt) {
    ++out_frames_;

    // Write initial file header if first frame.
    if (pkt->data.frame.pts == 0) write_ivf_file_header(&cfg_, 0, outfile_);

    // Write frame header and data.
    write_ivf_frame_header(pkt, outfile_);
    (void)fwrite(pkt->data.frame.buf, 1, pkt->data.frame.sz, outfile_);
  }
#endif

  double frame0_psnr_;
#if WRITE_COMPRESSED_STREAM
  FILE *outfile_;
  unsigned int out_frames_;
#endif
};

class ResizingCspVideoSource : public ::libaom_test::DummyVideoSource {
 public:
  ResizingCspVideoSource() {
    SetSize(kInitialWidth, kInitialHeight);
    limit_ = 30;
  }

  virtual ~ResizingCspVideoSource() {}

 protected:
  virtual void Next() {
    ++frame_;
    SetImageFormat(CspForFrameNumber(frame_));
    FillFrame();
  }
};

TEST_P(ResizeCspTest, TestResizeCspWorks) {
  ResizingCspVideoSource video;
  init_flags_ = AOM_CODEC_USE_PSNR;
  cfg_.rc_min_quantizer = cfg_.rc_max_quantizer = 48;
  cfg_.g_lag_in_frames = 0;
  ASSERT_NO_FATAL_FAILURE(RunLoop(&video));
}

AV1_INSTANTIATE_TEST_CASE(ResizeTest,
                           ::testing::Values(::libaom_test::kRealTime));
AV1_INSTANTIATE_TEST_CASE(ResizeInternalTest,
                           ::testing::Values(::libaom_test::kOnePassBest));
AV1_INSTANTIATE_TEST_CASE(ResizeRealtimeTest,
                           ::testing::Values(::libaom_test::kRealTime),
                           ::testing::Range(5, 9));
AV1_INSTANTIATE_TEST_CASE(ResizeCspTest,
                           ::testing::Values(::libaom_test::kRealTime));
}  // namespace
