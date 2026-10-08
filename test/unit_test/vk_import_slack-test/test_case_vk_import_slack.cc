/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vulkan/vulkan.h>

#include "gtest/gtest.h"

#include "dmabuf_vulkan_import.h"

namespace {

using Importer = DmabufVulkanImporter;

// V3D_TFU_READAHEAD_SIZE, and the page v3dv rounds an allocation to.
constexpr VkDeviceSize kReadahead = 64;
constexpr VkDeviceSize kPage = 4096;

// What a producer's dma-buf actually holds: the kernel rounds a BO up to a
// page, and that is all the slack an exactly-sized allocation gets.
constexpr VkDeviceSize BoBytes(VkDeviceSize requested) {
  return ((requested + kPage - 1) / kPage) * kPage;
}

// A driver that pads nothing asks for the image and no more, and offers no
// short import to fall back on.
TEST(VkImportSlack, NoPaddingAsksForTheImage) {
  EXPECT_EQ(Importer::ImportFootprint(7372800, 0), 7372800u);
  EXPECT_EQ(Importer::ImportFootprint(7372801, 0), 7372801u);
  EXPECT_EQ(Importer::ShortImportSize(7372800, 7372800, 0), 0u);
}

// v3dv's rule, straight from v3dv_AllocateMemory: align(size + 64, 4096).
TEST(VkImportSlack, FootprintIsThePaddedPageRoundUp) {
  EXPECT_EQ(Importer::ImportFootprint(7372800, kReadahead), 7376896u);
  EXPECT_EQ(Importer::ImportFootprint(311296, kReadahead), 315392u);
  EXPECT_EQ(Importer::ImportFootprint(1, kReadahead), kPage);
}

// The two sizes measured on real hardware: 1280x1440 XR24 (#598) and the
// gtk4 popup whose import #691 reported. Both are whole multiples of a page,
// so the BO the producer gets has no slack at all and the driver refuses it --
// while asking 64 short brings the demand down to exactly what the BO holds.
TEST(VkImportSlack, PageAlignedImagesCannotBeImportedAtTheirOwnSize) {
  for (const VkDeviceSize image :
       {VkDeviceSize{7372800}, VkDeviceSize{311296}}) {
    const VkDeviceSize held = BoBytes(image);
    ASSERT_EQ(held, image) << "fixture must be page-aligned to be the case";
    EXPECT_GT(Importer::ImportFootprint(image, kReadahead), held);
    EXPECT_EQ(Importer::ShortImportSize(image, held, kReadahead),
              image - kReadahead);
    // And the retry is satisfiable by that same BO, which is why it works.
    EXPECT_LE(Importer::ImportFootprint(image - kReadahead, kReadahead), held);
  }
}

// Why the same producer imports some surfaces and not others: it turns on how
// much of the last page the image leaves spare. 64 spare bytes import; 63 do
// not. Nothing about the surface's role or modifier decides it.
TEST(VkImportSlack, SpareBytesInTheLastPageDecideIt) {
  constexpr VkDeviceSize kBase = 4096 * 1800;

  const VkDeviceSize fits = kBase + (kPage - kReadahead);  // 64 spare
  EXPECT_LE(Importer::ImportFootprint(fits, kReadahead), BoBytes(fits));
  EXPECT_EQ(Importer::ShortImportSize(fits, BoBytes(fits), kReadahead),
            fits - kReadahead)
      << "offered even where unneeded; Import only asks after a refusal";

  const VkDeviceSize refused = fits + 1;  // 63 spare
  EXPECT_GT(Importer::ImportFootprint(refused, kReadahead), BoBytes(refused));
  // The short ask does rescue it, and that generalizes: the retry only needs
  // the dma-buf to cover align(image, page), which the kernel's page-rounded
  // BO always does. So the workaround carries every frame whose producer
  // allocated at least the image, and nothing else.
  EXPECT_EQ(Importer::ShortImportSize(refused, BoBytes(refused), kReadahead),
            refused - kReadahead);

  const VkDeviceSize aligned = kBase;  // 0 spare
  EXPECT_GT(Importer::ImportFootprint(aligned, kReadahead), BoBytes(aligned));
}

// A dma-buf that does not even cover the image is the producer under-allocating
// (#598), not this bug: no short ask can make it fit, so none is offered and
// the failure is reported as it was.
TEST(VkImportSlack, ShortDmabufGetsNoRetry) {
  constexpr VkDeviceSize kImage = 7372800;
  EXPECT_EQ(Importer::ShortImportSize(kImage, kImage - kPage, kReadahead), 0u);
  EXPECT_EQ(Importer::ShortImportSize(kImage, 0, kReadahead), 0u);
  // One page below the image, rounded: still short of align(image, page).
  EXPECT_EQ(Importer::ShortImportSize(kImage + 1, BoBytes(kImage), kReadahead),
            0u);
}

// An image smaller than the padding cannot be asked for short at all.
TEST(VkImportSlack, TinyImageGetsNoRetry) {
  EXPECT_EQ(Importer::ShortImportSize(kReadahead, kPage, kReadahead), 0u);
  EXPECT_EQ(Importer::ShortImportSize(1, kPage, kReadahead), 0u);
  EXPECT_EQ(Importer::ShortImportSize(kReadahead + 1, kPage, kReadahead), 1u);
}

// The page is the driver's constant, not the host's, so a driver that rounds
// to something else is expressible here.
TEST(VkImportSlack, PageIsAParameter) {
  EXPECT_EQ(Importer::ImportFootprint(8192, kReadahead, 16384), 16384u);
  EXPECT_EQ(Importer::ShortImportSize(16384, 16384, kReadahead, 16384),
            16384u - kReadahead);
}

}  // namespace
