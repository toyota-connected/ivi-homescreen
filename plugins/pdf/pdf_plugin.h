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

#ifndef FLUTTER_PLUGIN_PDF_PLUGIN_H_
#define FLUTTER_PLUGIN_PDF_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>

#include "messages.h"

namespace plugin_pdf {

class PdfPlugin final : public flutter::Plugin, public PrintingApi {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar);

  PdfPlugin();

  ~PdfPlugin() override;

  static void on_page_rasterized(std::vector<uint8_t> data,
                                 int width,
                                 int height,
                                 int job_id);

  static void on_page_raster_end(int job_id, const std::string& error);

  // Disallow copy and assign.
  PdfPlugin(const PdfPlugin&) = delete;
  PdfPlugin& operator=(const PdfPlugin&) = delete;

  // PdfApi methods.
  std::optional<FlutterError> RasterPdf(std::vector<uint8_t> doc,
                                        std::vector<int32_t> pages,
                                        double scale,
                                        int job_id) override;

  bool SharePdf(std::vector<uint8_t> buffer, const std::string& name) override;
};

}  // namespace plugin_pdf

#endif  // FLUTTER_PLUGIN_PDF_PLUGIN_H_
