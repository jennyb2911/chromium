/*
 * Copyright (c) 2013, Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "third_party/blink/renderer/platform/graphics/bitmap_image.h"

#include "base/test/simple_test_tick_clock.h"
#include "cc/paint/skia_paint_canvas.h"
#include "cc/tiles/mipmap_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/platform/geometry/float_rect.h"
#include "third_party/blink/renderer/platform/graphics/bitmap_image_metrics.h"
#include "third_party/blink/renderer/platform/graphics/deferred_image_decoder.h"
#include "third_party/blink/renderer/platform/graphics/image_observer.h"
#include "third_party/blink/renderer/platform/graphics/test/mock_image_decoder.h"
#include "third_party/blink/renderer/platform/scheduler/test/fake_task_runner.h"
#include "third_party/blink/renderer/platform/shared_buffer.h"
#include "third_party/blink/renderer/platform/testing/histogram_tester.h"
#include "third_party/blink/renderer/platform/testing/runtime_enabled_features_test_helpers.h"
#include "third_party/blink/renderer/platform/testing/testing_platform_support_with_mock_scheduler.h"
#include "third_party/blink/renderer/platform/testing/unit_test_helpers.h"
#include "third_party/blink/renderer/platform/wtf/std_lib_extras.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkImage.h"

namespace blink {
namespace {

class FrameSettingImageProvider : public cc::ImageProvider {
 public:
  FrameSettingImageProvider(size_t frame_index,
                            cc::PaintImage::GeneratorClientId client_id)
      : frame_index_(frame_index), client_id_(client_id) {}
  ~FrameSettingImageProvider() override = default;

  ScopedDecodedDrawImage GetDecodedDrawImage(
      const cc::DrawImage& draw_image) override {
    auto sk_image =
        draw_image.paint_image().GetSkImageForFrame(frame_index_, client_id_);
    return ScopedDecodedDrawImage(
        cc::DecodedDrawImage(sk_image, SkSize::MakeEmpty(), SkSize::Make(1, 1),
                             draw_image.filter_quality(), true));
  }

 private:
  size_t frame_index_;
  cc::PaintImage::GeneratorClientId client_id_;
};

void GenerateBitmapForPaintImage(cc::PaintImage paint_image,
                                 size_t frame_index,
                                 cc::PaintImage::GeneratorClientId client_id,
                                 SkBitmap* bitmap) {
  CHECK(paint_image);
  CHECK_GE(paint_image.FrameCount(), frame_index);

  SkImageInfo info =
      SkImageInfo::MakeN32Premul(paint_image.width(), paint_image.height());
  bitmap->allocPixels(info, paint_image.width() * 4);
  bitmap->eraseColor(SK_AlphaTRANSPARENT);
  FrameSettingImageProvider image_provider(frame_index, client_id);
  cc::SkiaPaintCanvas canvas(*bitmap, &image_provider);
  canvas.drawImage(paint_image, 0u, 0u, nullptr);
}

}  // namespace

class BitmapImageTest : public testing::Test {
 public:
  class FakeImageObserver : public GarbageCollectedFinalized<FakeImageObserver>,
                            public ImageObserver {
    USING_GARBAGE_COLLECTED_MIXIN(FakeImageObserver);

   public:
    FakeImageObserver()
        : last_decoded_size_(0), last_decoded_size_changed_delta_(0) {}

    void DecodedSizeChangedTo(const Image*, size_t new_size) override {
      last_decoded_size_changed_delta_ =
          SafeCast<int>(new_size) - SafeCast<int>(last_decoded_size_);
      last_decoded_size_ = new_size;
    }
    bool ShouldPauseAnimation(const Image*) override { return false; }
    void AnimationAdvanced(const Image*) override {
      animation_advanced_ = true;
    }
    void AsyncLoadCompleted(const Image*) override { NOTREACHED(); }

    void ChangedInRect(const Image*, const IntRect&) override {}

    size_t last_decoded_size_;
    int last_decoded_size_changed_delta_;
    bool animation_advanced_ = false;
  };

  static scoped_refptr<SharedBuffer> ReadFile(const char* file_name) {
    String file_path = test::BlinkLayoutTestsDir();
    file_path.append(file_name);
    return test::ReadFromFile(file_path);
  }

  // Accessors to BitmapImage's protected methods.
  void DestroyDecodedData() { image_->DestroyDecodedData(); }
  size_t FrameCount() { return image_->FrameCount(); }

  void LoadImage(const char* file_name) {
    scoped_refptr<SharedBuffer> image_data = ReadFile(file_name);
    ASSERT_TRUE(image_data.get());

    image_->SetData(image_data, true);
  }

  SkBitmap GenerateBitmap(size_t frame_index) {
    SkBitmap bitmap;
    GenerateBitmapForPaintImage(image_->PaintImageForTesting(), frame_index,
                                cc::PaintImage::kDefaultGeneratorClientId,
                                &bitmap);
    return bitmap;
  }

  SkBitmap GenerateBitmapForImage(const char* file_name) {
    scoped_refptr<SharedBuffer> image_data = ReadFile(file_name);
    EXPECT_TRUE(image_data.get());
    if (!image_data)
      return SkBitmap();

    auto image = BitmapImage::Create();
    image->SetData(image_data, true);
    auto paint_image = image->PaintImageForCurrentFrame();
    CHECK(paint_image);

    SkBitmap bitmap;
    SkImageInfo info = SkImageInfo::MakeN32Premul(image->Size().Width(),
                                                  image->Size().Height());
    bitmap.allocPixels(info, image->Size().Width() * 4);
    bitmap.eraseColor(SK_AlphaTRANSPARENT);
    cc::SkiaPaintCanvas canvas(bitmap);
    canvas.drawImage(paint_image, 0u, 0u, nullptr);
    return bitmap;
  }

  void VerifyBitmap(const SkBitmap& bitmap, SkColor color) {
    ASSERT_GT(bitmap.width(), 0);
    ASSERT_GT(bitmap.height(), 0);

    for (int i = 0; i < bitmap.width(); ++i) {
      for (int j = 0; j < bitmap.height(); ++j) {
        auto bitmap_color = bitmap.getColor(i, j);
        EXPECT_EQ(bitmap_color, color)
            << "Bitmap: " << SkColorGetA(bitmap_color) << ","
            << SkColorGetR(bitmap_color) << "," << SkColorGetG(bitmap_color)
            << "," << SkColorGetB(bitmap_color)
            << "Expected: " << SkColorGetA(color) << "," << SkColorGetR(color)
            << "," << SkColorGetG(color) << "," << SkColorGetB(color);
      }
    }
  }

  void VerifyBitmap(const SkBitmap& bitmap, const SkBitmap& expected) {
    ASSERT_GT(bitmap.width(), 0);
    ASSERT_GT(bitmap.height(), 0);
    ASSERT_EQ(bitmap.info(), expected.info());

    for (int i = 0; i < bitmap.width(); ++i) {
      for (int j = 0; j < bitmap.height(); ++j) {
        auto bitmap_color = bitmap.getColor(i, j);
        auto expected_color = expected.getColor(i, j);
        EXPECT_EQ(bitmap_color, expected_color)
            << "Bitmap: " << SkColorGetA(bitmap_color) << ","
            << SkColorGetR(bitmap_color) << "," << SkColorGetG(bitmap_color)
            << "," << SkColorGetB(bitmap_color)
            << "Expected: " << SkColorGetA(expected_color) << ","
            << SkColorGetR(expected_color) << "," << SkColorGetG(expected_color)
            << "," << SkColorGetB(expected_color);
      }
    }
  }

  size_t DecodedSize() { return image_->TotalFrameBytes(); }

  int RepetitionCount() { return image_->RepetitionCount(); }

  scoped_refptr<Image> ImageForDefaultFrame() {
    return image_->ImageForDefaultFrame();
  }

  int LastDecodedSizeChange() {
    return image_observer_->last_decoded_size_changed_delta_;
  }

  scoped_refptr<SharedBuffer> Data() { return image_->Data(); }

 protected:
  void SetUp() override {
    image_observer_ = new FakeImageObserver;
    image_ = BitmapImage::Create(image_observer_.Get());
  }

  Persistent<FakeImageObserver> image_observer_;
  scoped_refptr<BitmapImage> image_;
  ScopedTestingPlatformSupport<TestingPlatformSupportWithMockScheduler>
      platform_;
};

TEST_F(BitmapImageTest, destroyDecodedData) {
  LoadImage("/images/resources/animated-10color.gif");
  image_->PaintImageForCurrentFrame();
  size_t total_size = DecodedSize();
  EXPECT_GT(total_size, 0u);
  DestroyDecodedData();
  EXPECT_EQ(-static_cast<int>(total_size), LastDecodedSizeChange());
  EXPECT_EQ(0u, DecodedSize());
}

TEST_F(BitmapImageTest, maybeAnimated) {
  LoadImage("/images/resources/gif-loop-count.gif");
  EXPECT_TRUE(image_->MaybeAnimated());
}

TEST_F(BitmapImageTest, isAllDataReceived) {
  scoped_refptr<SharedBuffer> image_data =
      ReadFile("/images/resources/green.jpg");
  ASSERT_TRUE(image_data.get());

  scoped_refptr<BitmapImage> image = BitmapImage::Create();
  EXPECT_FALSE(image->IsAllDataReceived());

  image->SetData(image_data, false);
  EXPECT_FALSE(image->IsAllDataReceived());

  image->SetData(image_data, true);
  EXPECT_TRUE(image->IsAllDataReceived());

  image->SetData(SharedBuffer::Create("data", sizeof("data")), false);
  EXPECT_FALSE(image->IsAllDataReceived());

  image->SetData(image_data, true);
  EXPECT_TRUE(image->IsAllDataReceived());
}

TEST_F(BitmapImageTest, noColorProfile) {
  LoadImage("/images/resources/green.jpg");
  image_->PaintImageForCurrentFrame();
  EXPECT_EQ(1024u, DecodedSize());
  EXPECT_FALSE(image_->HasColorProfile());
}

TEST_F(BitmapImageTest, jpegHasColorProfile) {
  LoadImage("/images/resources/icc-v2-gbr.jpg");
  image_->PaintImageForCurrentFrame();
  EXPECT_EQ(227700u, DecodedSize());
  EXPECT_TRUE(image_->HasColorProfile());
}

TEST_F(BitmapImageTest, pngHasColorProfile) {
  LoadImage(
      "/images/resources/"
      "palatted-color-png-gamma-one-color-profile.png");
  image_->PaintImageForCurrentFrame();
  EXPECT_EQ(65536u, DecodedSize());
  EXPECT_TRUE(image_->HasColorProfile());
}

TEST_F(BitmapImageTest, webpHasColorProfile) {
  LoadImage("/images/resources/webp-color-profile-lossy.webp");
  image_->PaintImageForCurrentFrame();
  EXPECT_EQ(2560000u, DecodedSize());
  EXPECT_TRUE(image_->HasColorProfile());
}

TEST_F(BitmapImageTest, icoHasWrongFrameDimensions) {
  LoadImage("/images/resources/wrong-frame-dimensions.ico");
  // This call would cause crash without fix for 408026
  ImageForDefaultFrame();
}

TEST_F(BitmapImageTest, correctDecodedDataSize) {
  // Requesting any one frame shouldn't result in decoding any other frames.
  LoadImage("/images/resources/anim_none.gif");
  image_->PaintImageForCurrentFrame();
  int frame_size =
      static_cast<int>(image_->Size().Area() * sizeof(ImageFrame::PixelData));
  EXPECT_EQ(frame_size, LastDecodedSizeChange());
}

TEST_F(BitmapImageTest, recachingFrameAfterDataChanged) {
  LoadImage("/images/resources/green.jpg");
  image_->PaintImageForCurrentFrame();
  EXPECT_GT(LastDecodedSizeChange(), 0);
  image_observer_->last_decoded_size_changed_delta_ = 0;

  // Calling dataChanged causes the cache to flush, but doesn't affect the
  // source's decoded frames. It shouldn't affect decoded size.
  image_->DataChanged(true);
  EXPECT_EQ(0, LastDecodedSizeChange());
  // Recaching the first frame also shouldn't affect decoded size.
  image_->PaintImageForCurrentFrame();
  EXPECT_EQ(0, LastDecodedSizeChange());
}

TEST_F(BitmapImageTest, ConstantImageIdForPartiallyLoadedImages) {
  scoped_refptr<SharedBuffer> image_data =
      ReadFile("/images/resources/green.jpg");
  ASSERT_TRUE(image_data.get());

  // Create a new buffer to partially supply the data.
  scoped_refptr<SharedBuffer> partial_buffer = SharedBuffer::Create();
  partial_buffer->Append(image_data->Data(), image_data->size() - 4);

  // First partial load. Repeated calls for a PaintImage should have the same
  // image until the data changes or the decoded data is destroyed.
  ASSERT_EQ(image_->SetData(partial_buffer, false), Image::kSizeAvailable);
  auto image1 = image_->PaintImageForCurrentFrame();
  auto image2 = image_->PaintImageForCurrentFrame();
  EXPECT_EQ(image1, image2);
  auto sk_image1 = image1.GetSkImage();
  auto sk_image2 = image2.GetSkImage();
  EXPECT_EQ(sk_image1->uniqueID(), sk_image2->uniqueID());

  // Frame keys should be the same for these PaintImages.
  EXPECT_EQ(image1.GetKeyForFrame(PaintImage::kDefaultFrameIndex),
            image2.GetKeyForFrame(PaintImage::kDefaultFrameIndex));

  // Destroy the decoded data. This generates a new id since we don't cache
  // image ids for partial decodes.
  DestroyDecodedData();
  auto image3 = image_->PaintImageForCurrentFrame();
  auto sk_image3 = image3.GetSkImage();
  EXPECT_NE(sk_image1, sk_image3);
  EXPECT_NE(sk_image1->uniqueID(), sk_image3->uniqueID());

  // Since the cached generator is discarded on destroying the cached decode,
  // the new content id is generated resulting in an updated frame key.
  EXPECT_NE(image1.GetKeyForFrame(PaintImage::kDefaultFrameIndex),
            image3.GetKeyForFrame(PaintImage::kDefaultFrameIndex));

  // Load complete. This should generate a new image id.
  image_->SetData(image_data, true);
  auto complete_image = image_->PaintImageForCurrentFrame();
  auto complete_sk_image = complete_image.GetSkImage();
  EXPECT_NE(sk_image3, complete_sk_image);
  EXPECT_NE(sk_image3->uniqueID(), complete_sk_image->uniqueID());
  EXPECT_NE(complete_image.GetKeyForFrame(PaintImage::kDefaultFrameIndex),
            image3.GetKeyForFrame(PaintImage::kDefaultFrameIndex));

  // Destroy the decoded data and re-create the PaintImage. The frame key
  // remains constant but the SkImage id will change since we don't cache skia
  // uniqueIDs.
  DestroyDecodedData();
  auto new_complete_image = image_->PaintImageForCurrentFrame();
  auto new_complete_sk_image = new_complete_image.GetSkImage();
  EXPECT_NE(new_complete_sk_image, complete_sk_image);
  EXPECT_EQ(new_complete_image.GetKeyForFrame(PaintImage::kDefaultFrameIndex),
            complete_image.GetKeyForFrame(PaintImage::kDefaultFrameIndex));
}

TEST_F(BitmapImageTest, ImageForDefaultFrame_MultiFrame) {
  LoadImage("/images/resources/anim_none.gif");

  // Multi-frame images create new StaticBitmapImages for each call.
  auto default_image1 = image_->ImageForDefaultFrame();
  auto default_image2 = image_->ImageForDefaultFrame();
  EXPECT_NE(default_image1, default_image2);

  // But the PaintImage should be the same.
  auto paint_image1 = default_image1->PaintImageForCurrentFrame();
  auto paint_image2 = default_image2->PaintImageForCurrentFrame();
  EXPECT_EQ(paint_image1, paint_image2);
  EXPECT_EQ(paint_image1.GetSkImage()->uniqueID(),
            paint_image2.GetSkImage()->uniqueID());
}

TEST_F(BitmapImageTest, ImageForDefaultFrame_SingleFrame) {
  LoadImage("/images/resources/green.jpg");

  // Default frame images for single-frame cases is the image itself.
  EXPECT_EQ(image_->ImageForDefaultFrame(), image_);
}

TEST_F(BitmapImageTest, GifDecoderFrame0) {
  LoadImage("/images/resources/green-red-blue-yellow-animated.gif");
  auto bitmap = GenerateBitmap(0u);
  SkColor color = SkColorSetARGB(255, 0, 128, 0);
  VerifyBitmap(bitmap, color);
}

TEST_F(BitmapImageTest, GifDecoderFrame1) {
  LoadImage("/images/resources/green-red-blue-yellow-animated.gif");
  auto bitmap = GenerateBitmap(1u);
  VerifyBitmap(bitmap, SK_ColorRED);
}

TEST_F(BitmapImageTest, GifDecoderFrame2) {
  LoadImage("/images/resources/green-red-blue-yellow-animated.gif");
  auto bitmap = GenerateBitmap(2u);
  VerifyBitmap(bitmap, SK_ColorBLUE);
}

TEST_F(BitmapImageTest, GifDecoderFrame3) {
  LoadImage("/images/resources/green-red-blue-yellow-animated.gif");
  auto bitmap = GenerateBitmap(3u);
  VerifyBitmap(bitmap, SK_ColorYELLOW);
}

TEST_F(BitmapImageTest, GifDecoderMultiThreaded) {
  LoadImage("/images/resources/green-red-blue-yellow-animated.gif");
  auto paint_image = image_->PaintImageForTesting();
  ASSERT_EQ(paint_image.FrameCount(), 4u);

  struct Decode {
    SkBitmap bitmap;
    std::unique_ptr<base::Thread> thread;
    cc::PaintImage::GeneratorClientId client_id;
  };

  Decode decodes[4];
  SkColor expected_color[4] = {SkColorSetARGB(255, 0, 128, 0), SK_ColorRED,
                               SK_ColorBLUE, SK_ColorYELLOW};
  for (int i = 0; i < 4; ++i) {
    decodes[i].thread =
        std::make_unique<base::Thread>("Decode" + std::to_string(i));
    decodes[i].client_id = cc::PaintImage::GetNextGeneratorClientId();

    decodes[i].thread->StartAndWaitForTesting();
    decodes[i].thread->task_runner()->PostTask(
        FROM_HERE, base::BindOnce(&GenerateBitmapForPaintImage, paint_image, i,
                                  decodes[i].client_id, &decodes[i].bitmap));
  }

  for (int i = 0; i < 4; ++i) {
    decodes[i].thread->FlushForTesting();
    VerifyBitmap(decodes[i].bitmap, expected_color[i]);
  }
}

TEST_F(BitmapImageTest, APNGDecoder00) {
  LoadImage("/images/resources/apng00.png");
  auto actual_bitmap = GenerateBitmap(0u);
  auto expected_bitmap =
      GenerateBitmapForImage("/images/resources/apng00-ref.png");
  VerifyBitmap(actual_bitmap, expected_bitmap);
}

// Jump to the final frame of each image.
TEST_F(BitmapImageTest, APNGDecoder01) {
  LoadImage("/images/resources/apng01.png");
  auto actual_bitmap = GenerateBitmap(9u);
  auto expected_bitmap =
      GenerateBitmapForImage("/images/resources/apng01-ref.png");
  VerifyBitmap(actual_bitmap, expected_bitmap);
}

TEST_F(BitmapImageTest, APNGDecoder02) {
  LoadImage("/images/resources/apng02.png");
  auto actual_bitmap = GenerateBitmap(9u);
  auto expected_bitmap =
      GenerateBitmapForImage("/images/resources/apng02-ref.png");
  VerifyBitmap(actual_bitmap, expected_bitmap);
}

TEST_F(BitmapImageTest, APNGDecoder04) {
  LoadImage("/images/resources/apng04.png");
  auto actual_bitmap = GenerateBitmap(12u);
  auto expected_bitmap =
      GenerateBitmapForImage("/images/resources/apng04-ref.png");
  VerifyBitmap(actual_bitmap, expected_bitmap);
}

TEST_F(BitmapImageTest, APNGDecoder08) {
  LoadImage("/images/resources/apng08.png");
  auto actual_bitmap = GenerateBitmap(12u);
  auto expected_bitmap =
      GenerateBitmapForImage("/images/resources/apng08-ref.png");
  VerifyBitmap(actual_bitmap, expected_bitmap);
}

TEST_F(BitmapImageTest, APNGDecoder10) {
  LoadImage("/images/resources/apng10.png");
  auto actual_bitmap = GenerateBitmap(3u);
  auto expected_bitmap =
      GenerateBitmapForImage("/images/resources/apng10-ref.png");
  VerifyBitmap(actual_bitmap, expected_bitmap);
}

TEST_F(BitmapImageTest, APNGDecoder11) {
  LoadImage("/images/resources/apng11.png");
  auto actual_bitmap = GenerateBitmap(9u);
  auto expected_bitmap =
      GenerateBitmapForImage("/images/resources/apng11-ref.png");
  VerifyBitmap(actual_bitmap, expected_bitmap);
}

TEST_F(BitmapImageTest, APNGDecoder12) {
  LoadImage("/images/resources/apng12.png");
  auto actual_bitmap = GenerateBitmap(9u);
  auto expected_bitmap =
      GenerateBitmapForImage("/images/resources/apng12-ref.png");
  VerifyBitmap(actual_bitmap, expected_bitmap);
}

TEST_F(BitmapImageTest, APNGDecoder14) {
  LoadImage("/images/resources/apng14.png");
  auto actual_bitmap = GenerateBitmap(12u);
  auto expected_bitmap =
      GenerateBitmapForImage("/images/resources/apng14-ref.png");
  VerifyBitmap(actual_bitmap, expected_bitmap);
}

TEST_F(BitmapImageTest, APNGDecoder18) {
  LoadImage("/images/resources/apng18.png");
  auto actual_bitmap = GenerateBitmap(12u);
  auto expected_bitmap =
      GenerateBitmapForImage("/images/resources/apng18-ref.png");
  VerifyBitmap(actual_bitmap, expected_bitmap);
}

TEST_F(BitmapImageTest, APNGDecoder19) {
  LoadImage("/images/resources/apng19.png");
  auto actual_bitmap = GenerateBitmap(12u);
  auto expected_bitmap =
      GenerateBitmapForImage("/images/resources/apng19-ref.png");
  VerifyBitmap(actual_bitmap, expected_bitmap);
}

TEST_F(BitmapImageTest, APNGDecoderDisposePrevious) {
  LoadImage("/images/resources/crbug722072.png");
  auto actual_bitmap = GenerateBitmap(3u);
  auto expected_bitmap = GenerateBitmapForImage("/images/resources/green.png");
  VerifyBitmap(actual_bitmap, expected_bitmap);
}

TEST_F(BitmapImageTest, GIFRepetitionCount) {
  LoadImage("/images/resources/three-frames_loop-three-times.gif");
  auto paint_image = image_->PaintImageForCurrentFrame();
  EXPECT_EQ(paint_image.repetition_count(), 3);
  EXPECT_EQ(paint_image.FrameCount(), 3u);
}

TEST_F(BitmapImageTest, DecoderAndCacheMipLevels) {
  // Tests that the supported sizes from the decoder match the mip level sizes
  // in cc.
  LoadImage("/images/resources/cat.jpg");
  auto paint_image = image_->PaintImageForCurrentFrame();
  // Jpeg decoder supports upto 1/8 downscales, or mip level 3.
  for (int mip_level = 0; mip_level < 4; ++mip_level) {
    SCOPED_TRACE(mip_level);
    SkISize scaled_size = gfx::SizeToSkISize(cc::MipMapUtil::GetSizeForLevel(
        gfx::Size(paint_image.width(), paint_image.height()), mip_level));
    SkISize supported_size = paint_image.GetSupportedDecodeSize(scaled_size);
    EXPECT_EQ(gfx::SkISizeToSize(supported_size),
              gfx::SkISizeToSize(scaled_size));
  }
}

class BitmapImageTestWithMockDecoder : public BitmapImageTest,
                                       public MockImageDecoderClient {
 public:
  void SetUp() override {
    BitmapImageTest::SetUp();

    auto decoder = MockImageDecoder::Create(this);
    decoder->SetSize(10, 10);
    image_->SetDecoderForTesting(
        DeferredImageDecoder::CreateForTesting(std::move(decoder)));
  }

  void DecoderBeingDestroyed() override {}
  void DecodeRequested() override {}
  ImageFrame::Status GetStatus(size_t index) override {
    if (index < frame_count_ - 1 || last_frame_complete_)
      return ImageFrame::Status::kFrameComplete;
    return ImageFrame::Status::kFramePartial;
  }
  size_t FrameCount() override { return frame_count_; }
  int RepetitionCount() const override { return repetition_count_; }
  TimeDelta FrameDuration() const override { return duration_; }

 protected:
  TimeDelta duration_;
  int repetition_count_;
  size_t frame_count_;
  bool last_frame_complete_;
};

TEST_F(BitmapImageTestWithMockDecoder, ImageMetadataTracking) {
  // For a zero duration, we should make it non-zero when creating a PaintImage.
  repetition_count_ = kAnimationLoopOnce;
  frame_count_ = 4u;
  last_frame_complete_ = false;
  image_->SetData(SharedBuffer::Create("data", sizeof("data")), false);

  PaintImage image = image_->PaintImageForCurrentFrame();
  ASSERT_TRUE(image);
  EXPECT_EQ(image.FrameCount(), frame_count_);
  EXPECT_EQ(image.completion_state(),
            PaintImage::CompletionState::PARTIALLY_DONE);
  EXPECT_EQ(image.repetition_count(), repetition_count_);
  for (size_t i = 0; i < image.GetFrameMetadata().size(); ++i) {
    const auto& data = image.GetFrameMetadata()[i];
    EXPECT_EQ(data.duration, base::TimeDelta::FromMilliseconds(100));
    if (i == frame_count_ - 1 && !last_frame_complete_)
      EXPECT_FALSE(data.complete);
    else
      EXPECT_TRUE(data.complete);
  }

  // Now the load is finished.
  duration_ = TimeDelta::FromSeconds(1);
  repetition_count_ = kAnimationLoopInfinite;
  frame_count_ = 6u;
  last_frame_complete_ = true;
  image_->SetData(SharedBuffer::Create("data", sizeof("data")), true);

  image = image_->PaintImageForCurrentFrame();
  ASSERT_TRUE(image);
  EXPECT_EQ(image.FrameCount(), frame_count_);
  EXPECT_EQ(image.completion_state(), PaintImage::CompletionState::DONE);
  EXPECT_EQ(image.repetition_count(), repetition_count_);
  for (size_t i = 0; i < image.GetFrameMetadata().size(); ++i) {
    const auto& data = image.GetFrameMetadata()[i];
    if (i < 4u)
      EXPECT_EQ(data.duration, base::TimeDelta::FromMilliseconds(100));
    else
      EXPECT_EQ(data.duration, base::TimeDelta::FromSeconds(1));
    EXPECT_TRUE(data.complete);
  }
};

TEST_F(BitmapImageTestWithMockDecoder, AnimationPolicyOverride) {
  repetition_count_ = kAnimationLoopInfinite;
  frame_count_ = 4u;
  last_frame_complete_ = true;
  image_->SetData(SharedBuffer::Create("data", sizeof("data")), false);

  PaintImage image = image_->PaintImageForCurrentFrame();
  EXPECT_EQ(image.repetition_count(), repetition_count_);

  // Only one loop allowed.
  image_->SetAnimationPolicy(kImageAnimationPolicyAnimateOnce);
  image = image_->PaintImageForCurrentFrame();
  EXPECT_EQ(image.repetition_count(), kAnimationLoopOnce);

  // No animation allowed.
  image_->SetAnimationPolicy(kImageAnimationPolicyNoAnimation);
  image = image_->PaintImageForCurrentFrame();
  EXPECT_EQ(image.repetition_count(), kAnimationNone);

  // Default policy.
  image_->SetAnimationPolicy(kImageAnimationPolicyAllowed);
  image = image_->PaintImageForCurrentFrame();
  EXPECT_EQ(image.repetition_count(), repetition_count_);
}

TEST_F(BitmapImageTestWithMockDecoder, ResetAnimation) {
  repetition_count_ = kAnimationLoopInfinite;
  frame_count_ = 4u;
  last_frame_complete_ = true;
  image_->SetData(SharedBuffer::Create("data", sizeof("data")), false);

  PaintImage image = image_->PaintImageForCurrentFrame();
  image_->ResetAnimation();
  PaintImage image2 = image_->PaintImageForCurrentFrame();
  EXPECT_GT(image2.reset_animation_sequence_id(),
            image.reset_animation_sequence_id());
}

TEST_F(BitmapImageTestWithMockDecoder, PaintImageForStaticBitmapImage) {
  repetition_count_ = kAnimationLoopInfinite;
  frame_count_ = 5;
  last_frame_complete_ = true;
  image_->SetData(SharedBuffer::Create("data", sizeof("data")), false);

  // PaintImage for the original image is animated.
  EXPECT_TRUE(image_->PaintImageForCurrentFrame().ShouldAnimate());

  // But the StaticBitmapImage is not.
  EXPECT_FALSE(image_->ImageForDefaultFrame()
                   ->PaintImageForCurrentFrame()
                   .ShouldAnimate());
}

template <typename HistogramEnumType>
struct HistogramTestParams {
  const char* filename;
  HistogramEnumType type;
};

template <typename HistogramEnumType>
class BitmapHistogramTest : public BitmapImageTest,
                            public testing::WithParamInterface<
                                HistogramTestParams<HistogramEnumType>> {
 public:
  // Flag to tell the test that no samples should have been reported in this
  // case. Only useful when the parametric type is int.
  static const int kNoSamplesReported = -1;

 protected:
  void RunTest(const char* histogram_name) {
    HistogramTester histogram_tester;
    LoadImage(this->GetParam().filename);
    if (std::is_same<HistogramEnumType, int>::value &&
        this->GetParam().type == kNoSamplesReported) {
      histogram_tester.ExpectTotalCount(histogram_name, 0);
    } else {
      histogram_tester.ExpectUniqueSample(histogram_name, this->GetParam().type,
                                          1);
    }
  }
};

using DecodedImageTypeHistogramTest =
    BitmapHistogramTest<BitmapImageMetrics::DecodedImageType>;

TEST_P(DecodedImageTypeHistogramTest, ImageType) {
  RunTest("Blink.DecodedImageType");
}

const DecodedImageTypeHistogramTest::ParamType
    kDecodedImageTypeHistogramTestparams[] = {
        {"/images/resources/green.jpg", BitmapImageMetrics::kImageJPEG},
        {"/images/resources/"
         "palatted-color-png-gamma-one-color-profile.png",
         BitmapImageMetrics::kImagePNG},
        {"/images/resources/animated-10color.gif",
         BitmapImageMetrics::kImageGIF},
        {"/images/resources/webp-color-profile-lossy.webp",
         BitmapImageMetrics::kImageWebP},
        {"/images/resources/wrong-frame-dimensions.ico",
         BitmapImageMetrics::kImageICO},
        {"/images/resources/lenna.bmp", BitmapImageMetrics::kImageBMP}};

INSTANTIATE_TEST_CASE_P(
    DecodedImageTypeHistogramTest,
    DecodedImageTypeHistogramTest,
    testing::ValuesIn(kDecodedImageTypeHistogramTestparams));

using DecodedImageOrientationHistogramTest =
    BitmapHistogramTest<ImageOrientationEnum>;

TEST_P(DecodedImageOrientationHistogramTest, ImageOrientation) {
  RunTest("Blink.DecodedImage.Orientation");
}

const DecodedImageOrientationHistogramTest::ParamType
    kDecodedImageOrientationHistogramTestParams[] = {
        {"/images/resources/exif-orientation-1-ul.jpg", kOriginTopLeft},
        {"/images/resources/exif-orientation-2-ur.jpg", kOriginTopRight},
        {"/images/resources/exif-orientation-3-lr.jpg", kOriginBottomRight},
        {"/images/resources/exif-orientation-4-lol.jpg", kOriginBottomLeft},
        {"/images/resources/exif-orientation-5-lu.jpg", kOriginLeftTop},
        {"/images/resources/exif-orientation-6-ru.jpg", kOriginRightTop},
        {"/images/resources/exif-orientation-7-rl.jpg", kOriginRightBottom},
        {"/images/resources/exif-orientation-8-llo.jpg", kOriginLeftBottom}};

INSTANTIATE_TEST_CASE_P(
    DecodedImageOrientationHistogramTest,
    DecodedImageOrientationHistogramTest,
    testing::ValuesIn(kDecodedImageOrientationHistogramTestParams));

using DecodedImageDensityHistogramTest100px = BitmapHistogramTest<int>;

TEST_P(DecodedImageDensityHistogramTest100px, JpegDensity) {
  RunTest("Blink.DecodedImage.JpegDensity.100px");
}

const DecodedImageDensityHistogramTest100px::ParamType
    kDecodedImageDensityHistogramTest100pxParams[] = {
        // 64x64 too small to report any metric
        {"/images/resources/rgb-jpeg-red.jpg",
         DecodedImageDensityHistogramTest100px::kNoSamplesReported},
        // 439x154, 23220 bytes --> 2.74 bpp
        {"/images/resources/cropped_mandrill.jpg", 274},
        // 320x320, 74017 bytes --> 5.78
        {"/images/resources/blue-wheel-srgb-color-profile.jpg", 578},
        // 632x475 too big for the 100-399px range.
        {"/images/resources/cat.jpg",
         DecodedImageDensityHistogramTest100px::kNoSamplesReported}};

INSTANTIATE_TEST_CASE_P(
    DecodedImageDensityHistogramTest100px,
    DecodedImageDensityHistogramTest100px,
    testing::ValuesIn(kDecodedImageDensityHistogramTest100pxParams));

using DecodedImageDensityHistogramTest400px = BitmapHistogramTest<int>;

TEST_P(DecodedImageDensityHistogramTest400px, JpegDensity) {
  RunTest("Blink.DecodedImage.JpegDensity.400px");
}

const DecodedImageDensityHistogramTest400px::ParamType
    kDecodedImageDensityHistogramTest400pxParams[] = {
        // 439x154, only one dimension is big enough.
        {"/images/resources/cropped_mandrill.jpg",
         DecodedImageDensityHistogramTest400px::kNoSamplesReported},
        // 320x320, not big enough.
        {"/images/resources/blue-wheel-srgb-color-profile.jpg",
         DecodedImageDensityHistogramTest400px::kNoSamplesReported},
        // 632x475, 68826 bytes --> 1.83
        {"/images/resources/cat.jpg", 183}};

INSTANTIATE_TEST_CASE_P(
    DecodedImageDensityHistogramTest400px,
    DecodedImageDensityHistogramTest400px,
    testing::ValuesIn(kDecodedImageDensityHistogramTest400pxParams));

}  // namespace blink
