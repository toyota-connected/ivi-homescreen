/*
 * Copyright 2020-2024 Toyota Connected North America
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

#include "pdf_plugin.h"

#include <sys/wait.h>
#include <unistd.h>
#include <memory>
#include <numeric>

#include <flutter/plugin_registrar.h>
#include <fpdfview.h>

#include "messages.h"
#include "plugins/common/common.h"

namespace plugin_pdf {

std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel;

// static
void PdfPlugin::RegisterWithRegistrar(flutter::PluginRegistrar* registrar) {
  auto plugin = std::make_unique<PdfPlugin>();

  PrintingApi::SetUp(registrar->messenger(), plugin.get());

  registrar->AddPlugin(std::move(plugin));
}

PdfPlugin::PdfPlugin() = default;

PdfPlugin::~PdfPlugin() = default;

std::optional<FlutterError> PdfPlugin::RasterPdf(std::vector<uint8_t> data,
                                                 std::vector<int32_t> pages,
                                                 double scale,
                                                 int job_id) {
  SPDLOG_DEBUG("\tdoc_size: {}", data.size());
  SPDLOG_DEBUG("\tpages_count: {}", pages.size());
  SPDLOG_DEBUG("\tscale: {}", scale);
  SPDLOG_DEBUG("\tjob: {}", job_id);
  FPDF_LIBRARY_CONFIG config;
  config.version = 2;
  config.m_pUserFontPaths = nullptr;
  config.m_pIsolate = nullptr;
  config.m_v8EmbedderSlot = 0;
  // requires a PDFium build with skia enabled
  config.m_RendererType = FPDF_RENDERERTYPE_SKIA;

  FPDF_InitLibraryWithConfig(&config);

  auto doc = FPDF_LoadMemDocument64(data.data(), data.size(), nullptr);
  if (!doc) {
    unsigned long err = FPDF_GetLastError();
    SPDLOG_DEBUG("[pdf] Load unsuccessful: job: {}", job_id);
    switch (err) {
      case FPDF_ERR_SUCCESS:
        on_page_raster_end(job_id, "Success");
        break;
      case FPDF_ERR_UNKNOWN:
        on_page_raster_end(job_id, "Unknown error");
        break;
      case FPDF_ERR_FILE:
        on_page_raster_end(job_id, "File not found or could not be opened");
        break;
      case FPDF_ERR_FORMAT:
        on_page_raster_end(job_id, "File not in PDF format or corrupted");
        break;
      case FPDF_ERR_PASSWORD:
        on_page_raster_end(job_id, "Password required or incorrect password");
        break;
      case FPDF_ERR_SECURITY:
        on_page_raster_end(job_id, "Unsupported security scheme");
        break;
      case FPDF_ERR_PAGE:
        on_page_raster_end(job_id, "Page not found or content error");
        break;
      default:
        on_page_raster_end(job_id, "Unknown error " + std::to_string(err));
    }
    FPDF_DestroyLibrary();
    return std::nullopt;
  }

  auto pageCount = FPDF_GetPageCount(doc);

  if (pages.empty()) {
    // Use all pages
    pages.resize(static_cast<size_t>(pageCount));
    std::iota(std::begin(pages), std::end(pages), 0);
  }

  for (auto n : pages) {
    if (n >= pageCount) {
      continue;
    }

    auto page = FPDF_LoadPage(doc, n);
    if (!page) {
      continue;
    }

    auto width = FPDF_GetPageWidth(page);
    auto height = FPDF_GetPageHeight(page);

    auto bWidth = static_cast<int>(width * scale);
    auto bHeight = static_cast<int>(height * scale);

    auto bitmap = FPDFBitmap_Create(bWidth, bHeight, 1);
    FPDFBitmap_FillRect(bitmap, 0, 0, bWidth, bHeight, 0x00ffffff);

    FPDF_RenderPageBitmap(bitmap, page, 0, 0, bWidth, bHeight, 0,
                          FPDF_ANNOT | FPDF_LCD_TEXT);

    auto* p = static_cast<uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
    auto stride = FPDFBitmap_GetStride(bitmap);
    auto l = bHeight * stride;

    // BGRA to RGBA conversion
    for (auto y = 0; y < bHeight; y++) {
      auto offset = y * stride;
      for (auto x = 0; x < bWidth; x++) {
        auto t = p[offset];
        p[offset] = p[offset + 2];
        p[offset + 2] = t;
        offset += 4;
      }
    }

    on_page_rasterized(std::vector<uint8_t>{p, p + l}, bWidth, bHeight, job_id);

    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);
  }

  FPDF_CloseDocument(doc);
  FPDF_DestroyLibrary();

  on_page_raster_end(job_id, "");
  return std::nullopt;
}

bool PdfPlugin::SharePdf(std::vector<uint8_t> buffer, const std::string& name) {
  SPDLOG_DEBUG("\t{}", name);

  auto filename = "/tmp/" + name;

  auto fd = fopen(filename.c_str(), "wb");
  fwrite(buffer.data(), buffer.size(), 1, fd);
  fclose(fd);

  auto pid = fork();

  if (pid < 0) {
    return false;
  } else if (pid == 0) {
    execlp("xdg-open", "xdg-open", filename.c_str(), nullptr);
  }

  int status = 0;
  waitpid(pid, &status, 0);

  return status == 0;
}

void PdfPlugin::on_page_rasterized(std::vector<uint8_t> data,
                                   int width,
                                   int height,
                                   int job_id) {
  SPDLOG_DEBUG("on_page_rasterized: {}", job_id);
  channel->InvokeMethod(
      "onPageRasterized",
      std::make_unique<flutter::EncodableValue>(
          flutter::EncodableValue(flutter::EncodableMap{
              {flutter::EncodableValue("image"),
               flutter::EncodableValue(std::move(data))},
              {flutter::EncodableValue("width"),
               flutter::EncodableValue(width)},
              {flutter::EncodableValue("height"),
               flutter::EncodableValue(height)},
              {flutter::EncodableValue("job"), flutter::EncodableValue(job_id)},
          })));
}

void PdfPlugin::on_page_raster_end(int job_id, const std::string& error) {
  SPDLOG_DEBUG("on_page_raster_end: {}", job_id);
  auto map = flutter::EncodableMap{
      {flutter::EncodableValue("job"), flutter::EncodableValue(job_id)},
  };

  if (!error.empty()) {
    map[flutter::EncodableValue("error")] = flutter::EncodableValue(error);
  }

  channel->InvokeMethod(
      "onPageRasterEnd",
      std::make_unique<flutter::EncodableValue>(flutter::EncodableValue(map)));
}

}  // namespace plugin_pdf
