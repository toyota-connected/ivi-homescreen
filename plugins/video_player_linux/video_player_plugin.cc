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

#include "video_player_plugin.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>

extern "C" {
#include <libavutil/avutil.h>
}

#include "messages.g.h"
#include "plugins/common/glib/main_loop.h"
#include "video_player.h"

namespace video_player_linux {

// static
void VideoPlayerPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarDesktop* registrar) {
  auto plugin = std::make_unique<VideoPlayerPlugin>(registrar);
  VideoPlayerApi::SetUp(registrar->messenger(), plugin.get());
  registrar->AddPlugin(std::move(plugin));
}

VideoPlayerPlugin::~VideoPlayerPlugin() = default;

VideoPlayerPlugin::VideoPlayerPlugin(flutter::PluginRegistrarDesktop* registrar)
    : registrar_(registrar) {
  // GStreamer lib only needs to be initialized once.  Calling it multiple times
  // is fine.
  gst_init(nullptr, nullptr);

  // start the main loop if not already running
  plugin_common_glib::MainLoop::GetInstance();

  // supress libavformat logging
  av_log_set_callback([](void* /* avcl */, int /* level */,
                         const char* /* fmt */, va_list /* vl */) {});
}

std::optional<FlutterError> VideoPlayerPlugin::Initialize() {
  for (auto& player : videoPlayers) {
    player.second->Dispose();
  }
  videoPlayers.clear();
  return std::nullopt;
}

ErrorOr<int64_t> VideoPlayerPlugin::Create(
    const std::string* asset,
    const std::string* uri,
    const flutter::EncodableMap& http_headers) {
  std::string asset_to_load;
  std::map<std::string, std::string> http_headers_;

  std::unique_ptr<VideoPlayer> player;
  if (asset && !asset->empty()) {
    asset_to_load = "file://";
    std::filesystem::path path;
    if (asset->c_str()[0] == '/') {
      path /= asset->c_str();
    } else {
      path = registrar_->flutter_asset_folder();
      SPDLOG_DEBUG("path: [{}]", registrar_->flutter_asset_folder());
      path /= asset->c_str();
    }
    if (!exists(path)) {
      spdlog::error("[VideoPlayer] Asset Path does not exist. {}",
                    path.c_str());
      return FlutterError("asset_load_failed", "Asset Path does not exist.");
    }
    asset_to_load += path.c_str();
  } else if (uri && !uri->empty()) {
    asset_to_load = *uri;

    for (const auto& [key, value] : http_headers) {
      if (std::holds_alternative<std::string>(key) &&
          std::holds_alternative<std::string>(value)) {
        http_headers_[std::get<std::string>(key)] =
            std::get<std::string>(value);
      }
    }
  } else {
    return FlutterError("not_implemented", "Set either an asset or a uri");
  }

  SPDLOG_DEBUG("[VideoPlayer] asset: {}", asset_to_load);

  try {
    // Get stream information
    int width, height;
    gint64 duration;
    AVCodecID codec_id;
    if (!get_video_info(asset_to_load.c_str(), width, height, duration,
                        codec_id)) {
      spdlog::error("Failed to get video info");
    }

    const auto gst_codec = map_ffmpeg_plugin(codec_id);
    if (!gst_codec[0]) {
      spdlog::critical("[VideoPlayer] Failed to find codec: {}", gst_codec);
    }
    const auto decoder_factory = gst_element_factory_find(gst_codec);
    if (decoder_factory == nullptr) {
      spdlog::error(
          "[VideoPlayer] Failed to find decoder: {}.  May be a missing "
          "runtime package",
          gst_codec);
    }

    player = std::make_unique<VideoPlayer>(registrar_, asset_to_load.c_str(),
                                           std::move(http_headers_), width,
                                           height, duration, decoder_factory);

  } catch (std::exception& e) {
    return FlutterError("uri_load_failed", e.what());
  }

  player->Init(registrar_->messenger());

  auto texture_id = player->GetTextureId();

  videoPlayers.insert(std::make_pair(texture_id, std::move(player)));

  return texture_id;
}

std::optional<FlutterError> VideoPlayerPlugin::Dispose(int64_t texture_id) {
  auto searchPlayer = videoPlayers.find(texture_id);
  if (searchPlayer == videoPlayers.end()) {
    return FlutterError("player_not_found", "This player ID was not found");
  }
  if (searchPlayer->second->IsValid()) {
    searchPlayer->second->Dispose();
    videoPlayers.erase(texture_id);
  }

  return {};
}

std::optional<FlutterError> VideoPlayerPlugin::SetLooping(int64_t texture_id,
                                                          bool is_looping) {
  auto searchPlayer = videoPlayers.find(texture_id);
  if (searchPlayer == videoPlayers.end()) {
    return FlutterError("player_not_found", "This player ID was not found");
  }
  if (searchPlayer->second->IsValid()) {
    searchPlayer->second->SetLooping(is_looping);
  }

  return {};
}

std::optional<FlutterError> VideoPlayerPlugin::SetVolume(int64_t texture_id,
                                                         double volume) {
  auto searchPlayer = videoPlayers.find(texture_id);
  if (searchPlayer == videoPlayers.end()) {
    return FlutterError("player_not_found", "This player ID was not found");
  }
  if (searchPlayer->second->IsValid()) {
    searchPlayer->second->SetVolume(volume);
  }

  return {};
}

std::optional<FlutterError> VideoPlayerPlugin::SetPlaybackSpeed(
    int64_t texture_id,
    double speed) {
  auto searchPlayer = videoPlayers.find(texture_id);
  if (searchPlayer == videoPlayers.end()) {
    return FlutterError("player_not_found", "This player ID was not found");
  }
  if (searchPlayer->second->IsValid()) {
    searchPlayer->second->SetPlaybackSpeed(speed);
  }

  return {};
}

std::optional<FlutterError> VideoPlayerPlugin::Play(int64_t texture_id) {
  auto searchPlayer = videoPlayers.find(texture_id);
  if (searchPlayer == videoPlayers.end()) {
    return FlutterError("player_not_found", "This player ID was not found");
  }
  if (searchPlayer->second->IsValid()) {
    searchPlayer->second->Play();
  }

  return {};
}

ErrorOr<int64_t> VideoPlayerPlugin::GetPosition(int64_t texture_id) {
  auto searchPlayer = videoPlayers.find(texture_id);
  int64_t position = 0;
  if (searchPlayer != videoPlayers.end()) {
    const std::unique_ptr<VideoPlayer>& player = searchPlayer->second;
    if (player->IsValid()) {
      position = player->GetPosition();
      //      player->SendBufferingUpdate();
    }
  }
  return position;
}

std::optional<FlutterError> VideoPlayerPlugin::SeekTo(int64_t texture_id,
                                                      int64_t position) {
  auto searchPlayer = videoPlayers.find(texture_id);
  if (searchPlayer == videoPlayers.end()) {
    return FlutterError("player_not_found", "This player ID was not found");
  }
  if (searchPlayer->second->IsValid()) {
    searchPlayer->second->SeekTo(position);
  }

  return std::nullopt;
}

std::optional<FlutterError> VideoPlayerPlugin::Pause(int64_t texture_id) {
  auto searchPlayer = videoPlayers.find(texture_id);
  if (searchPlayer == videoPlayers.end()) {
    return FlutterError("player_not_found", "This player ID was not found");
  }
  if (searchPlayer->second->IsValid()) {
    searchPlayer->second->Pause();
  }

  return std::nullopt;
}

bool VideoPlayerPlugin::get_video_info(const char* url,
                                       int& width,
                                       int& height,
                                       gint64& duration,
                                       AVCodecID& codec_id) {
  AVFormatContext* fmt_ctx = avformat_alloc_context();

  if (avformat_open_input(&fmt_ctx, url, nullptr, nullptr) < 0) {
    spdlog::error("[VideoPlayer] Unable to open: {}", url);
    avformat_free_context(fmt_ctx);
    return false;
  }

  if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
    spdlog::error("[VideoPlayer] Cannot find stream information: {}", url);
    avformat_free_context(fmt_ctx);
    return false;
  }

  const int ret =
      av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (ret < 0) {
    spdlog::error("[VideoPlayer] Cannot find a video stream in the input file");
    avformat_free_context(fmt_ctx);
    return false;
  }
  const int video_stream_index = ret;
  const AVStream* stream = fmt_ctx->streams[video_stream_index];
  const AVCodecParameters* par = stream->codecpar;

  codec_id = par->codec_id;
  width = par->width;
  height = par->height;
  duration = fmt_ctx->duration;

#if GSTREAMER_DEBUG
  av_dump_format(fmt_ctx, 0, url, 0);
#endif

  avformat_free_context(fmt_ctx);
  return true;
}

const char* VideoPlayerPlugin::map_ffmpeg_plugin(AVCodecID codec_id) {
  switch (codec_id) {
    case AV_CODEC_ID_NONE:
      return "none";
    case AV_CODEC_ID_4XM:
      return "avdec_4xm";
    case AV_CODEC_ID_8BPS:
      return "avdec_8bps";
    case AV_CODEC_ID_8SVX_EXP:
      return "avdec_8svx_exp";
    case AV_CODEC_ID_8SVX_FIB:
      return "avdec_8svx_fib";
    case AV_CODEC_ID_AASC:
      return "avdec_aasc";
    case AV_CODEC_ID_AIC:
      return "avdec_aic";
    case AV_CODEC_ID_AMV:
      return "avdec_amv";
    case AV_CODEC_ID_ASV1:
      return "avdec_asv1";
    case AV_CODEC_ID_ASV2:
      return "avdec_asv2";
    case AV_CODEC_ID_AVS:
      return "avdec_avs";
    case AV_CODEC_ID_BMV_VIDEO:
      return "avdec_bmv_video";
    case AV_CODEC_ID_CAVS:
      return "avdec_cavs";
    case AV_CODEC_ID_CFHD:
      return "avdec_cfhd";
    case AV_CODEC_ID_CINEPAK:
      return "avdec_cinepak";
    case AV_CODEC_ID_CLEARVIDEO:
      return "avdec_clearvideo";
    case AV_CODEC_ID_CLJR:
      return "avdec_cljr";
    case AV_CODEC_ID_CYUV:
      return "avdec_cyuv";
    case AV_CODEC_ID_DDS:
      return "avdec_dds";
    case AV_CODEC_ID_DFA:
      return "avdec_dfa";
    case AV_CODEC_ID_DIRAC:
      return "avdec_dirac";
    case AV_CODEC_ID_DNXHD:
      return "avdec_dnxhd";
    case AV_CODEC_ID_DPX:
      return "avdec_dpx";
    case AV_CODEC_ID_DSICINVIDEO:
      return "avdec_dsicinvideo";
    case AV_CODEC_ID_DVVIDEO:
      return "avdec_dvvideo";
    case AV_CODEC_ID_DXA:
      return "avdec_dxa";
    case AV_CODEC_ID_DXTORY:
      return "avdec_dxtory";
    case AV_CODEC_ID_DXV:
      return "avdec_dxv";
    case AV_CODEC_ID_CMV:
      return "avdec_eacmv";
    case AV_CODEC_ID_MAD:
      return "avdec_eamad";
    case AV_CODEC_ID_TGQ:
      return "avdec_eatgq";
    case AV_CODEC_ID_TGV:
      return "avdec_eatgv";
    case AV_CODEC_ID_TQI:
      return "avdec_eatqi";
    case AV_CODEC_ID_ESCAPE124:
      return "avdec_escape124";
    case AV_CODEC_ID_ESCAPE130:
      return "avdec_escape130";
    case AV_CODEC_ID_EXR:
      return "avdec_exr";
    case AV_CODEC_ID_FFV1:
      return "avdec_ffv1";
    case AV_CODEC_ID_FFVHUFF:
      return "avdec_ffvhuff";
    case AV_CODEC_ID_FIC:
      return "avdec_fic";
    case AV_CODEC_ID_FITS:
      return "avdec_fits";
    case AV_CODEC_ID_FLASHSV:
      return "avdec_flashsv";
    case AV_CODEC_ID_FLASHSV2:
      return "avdec_flashsv2";
    case AV_CODEC_ID_FLIC:
      return "avdec_flic";
    case AV_CODEC_ID_FLV1:
      return "avdec_flv";
    case AV_CODEC_ID_FMVC:
      return "avdec_fmvc";
    case AV_CODEC_ID_FRAPS:
      return "avdec_fraps";
    case AV_CODEC_ID_FRWU:
      return "avdec_frwu";
    case AV_CODEC_ID_G2M:
      return "avdec_g2m";
    case AV_CODEC_ID_GDV:
      return "advec_gdv";
    case AV_CODEC_ID_H261:
      return "avdec_h261";
    case AV_CODEC_ID_H263:
      return "avdec_h263";
    case AV_CODEC_ID_H263I:
      return "avdec_h263i";
    case AV_CODEC_ID_H263P:
      return "avdec_h263p";
    case AV_CODEC_ID_H264:
      return "avdec_h264";
    case AV_CODEC_ID_H265:
      return "avdec_h265";
    case AV_CODEC_ID_HAP:
      return "avdec_hap";
    case AV_CODEC_ID_HNM4_VIDEO:
      return "avdec_hmn4video";
    case AV_CODEC_ID_HQ_HQA:
      return "avdec_hq_hqa";
    case AV_CODEC_ID_HQX:
      return "avdec_hqx";
    case AV_CODEC_ID_HUFFYUV:
      return "avdec_huffyuv";
    case AV_CODEC_ID_HYMT:
      return "avdec_hymt";
      // case AV_CODEC_ID_IDCINVIDEO:
      //   return "avdec_idcinvideo";
    case AV_CODEC_ID_IDF:
      return "avdec_idf";
      // case AV_CODEC_ID_IFF:
      //   return "avdec_iff";
    case AV_CODEC_ID_IMM4:
      return "avdec_imm4";
    case AV_CODEC_ID_INDEO2:
      return "avdec_indeo2";
    case AV_CODEC_ID_INDEO3:
      return "avdec_indeo3";
    case AV_CODEC_ID_INDEO4:
      return "avdec_indeo4";
    case AV_CODEC_ID_INDEO5:
      return "avdec_indeo5";
    case AV_CODEC_ID_INTERPLAY_VIDEO:
      return "avdec_interplayvideo";
    case AV_CODEC_ID_JPEG2000:
      return "avdec_jpeg2000";
    case AV_CODEC_ID_JPEGLS:
      return "avdec_jpegls";
    case AV_CODEC_ID_JV:
      return "avdec_jv";
    case AV_CODEC_ID_KGV1:
      return "avdec_kgv1";
    case AV_CODEC_ID_KMVC:
      return "avdec_kmvc";
    case AV_CODEC_ID_LAGARITH:
      return "avdec_lagarith";
    case AV_CODEC_ID_LOCO:
      return "avdec_loco";
    case AV_CODEC_ID_LSCR:
      return "avdec_lscr";
    case AV_CODEC_ID_M101:
      return "avdec_m101";
    case AV_CODEC_ID_MAGICYUV:
      return "avdec_magicyuv";
    case AV_CODEC_ID_MDEC:
      return "avdec_mdec";
    case AV_CODEC_ID_MIMIC:
      return "avdec_mimic";
    case AV_CODEC_ID_MJPEG:
      return "avdec_mjpeg";
    case AV_CODEC_ID_MJPEGB:
      return "avdec_mjpegb";
    case AV_CODEC_ID_MMVIDEO:
      return "avdec_mmvideo";
    case AV_CODEC_ID_MOTIONPIXELS:
      return "avdec_motionpixels";
    case AV_CODEC_ID_MPEG2VIDEO:
      return "avdec_mpeg2video";
    case AV_CODEC_ID_MPEG4:
      return "avdec_mpeg4";
    case AV_CODEC_ID_MPEG1VIDEO:
      return "avdec_mpegvideo";
    case AV_CODEC_ID_MSA1:
      return "avdec_msa1";
    case AV_CODEC_ID_MSCC:
      return "avdec_mscc";
    case AV_CODEC_ID_MSMPEG4V3:
      return "avdec_msmpeg4";
    case AV_CODEC_ID_MSMPEG4V1:
      return "avdec_msmpeg4v1";
    case AV_CODEC_ID_MSMPEG4V2:
      return "avdec_msmpeg4v2";
    case AV_CODEC_ID_MSRLE:
      return "avdec_msrle";
    case AV_CODEC_ID_MSS1:
      return "avdec_mss1";
    case AV_CODEC_ID_MSS2:
      return "avdec_mss2";
    case AV_CODEC_ID_MSVIDEO1:
      return "avdec_msvideo1";
    case AV_CODEC_ID_MSZH:
      return "avdec_mszh";
    case AV_CODEC_ID_MTS2:
      return "avdec_mts2";
    case AV_CODEC_ID_MVC1:
      return "avdec_mvc1";
    case AV_CODEC_ID_MVC2:
      return "avdec_mvc2";
    case AV_CODEC_ID_MWSC:
      return "avdec_mwsc";
    case AV_CODEC_ID_MXPEG:
      return "avdec_mxpeg";
    case AV_CODEC_ID_NUV:
      return "avdec_NUV";
    case AV_CODEC_ID_PAF_VIDEO:
      return "avdec_paf_video";
    case AV_CODEC_ID_PAM:
      return "avdec_pam";
    case AV_CODEC_ID_PBM:
      return "avdec_pbm";
    case AV_CODEC_ID_PCX:
      return "avdec_pcx";
    case AV_CODEC_ID_PGM:
      return "avdec_pgm";
    case AV_CODEC_ID_PGMYUV:
      return "avdec_pgmyuv";
    case AV_CODEC_ID_PICTOR:
      return "avdec_pictor";
    case AV_CODEC_ID_PIXLET:
      return "avdec_pixlet";
    case AV_CODEC_ID_PNG:
      return "avdec_png";
    case AV_CODEC_ID_PPM:
      return "avdec_ppm";
    case AV_CODEC_ID_PRORES:
      return "avdec_prores";
    case AV_CODEC_ID_PROSUMER:
      return "avdec_prosumer";
    case AV_CODEC_ID_PSD:
      return "avdec_psd";
    case AV_CODEC_ID_PTX:
      return "avdec_ptx";
    case AV_CODEC_ID_QDRAW:
      return "avdec_qdraw";
    case AV_CODEC_ID_QPEG:
      return "avdec_qpeg";
    case AV_CODEC_ID_QTRLE:
      return "avdec_qtrle";
    case AV_CODEC_ID_R10K:
      return "avdec_r10k";
    case AV_CODEC_ID_RASC:
      return "avdec_rasc";
    case AV_CODEC_ID_RL2:
      return "avdec_rl2";
    case AV_CODEC_ID_ROQ:
      return "avdec_roqvideo";
    case AV_CODEC_ID_RPZA:
      return "avdec_rpza";
    case AV_CODEC_ID_RSCC:
      return "avdec_rscc";
    case AV_CODEC_ID_RV10:
      return "avdec_rv10";
    case AV_CODEC_ID_RV20:
      return "avdec_rv20";
    case AV_CODEC_ID_RV30:
      return "avdec_rv30";
    case AV_CODEC_ID_RV40:
      return "avdec_rv40";
    case AV_CODEC_ID_SANM:
      return "avdec_sanm";
    case AV_CODEC_ID_SCPR:
      return "avdec_scpr";
    case AV_CODEC_ID_SCREENPRESSO:
      return "avdec_screenpresso";
    case AV_CODEC_ID_SGI:
      return "avdec_sgi";
    case AV_CODEC_ID_SGIRLE:
      return "avdec_sgirle";
    case AV_CODEC_ID_SHEERVIDEO:
      return "avdec_sheervideo";
    case AV_CODEC_ID_SMACKVIDEO:
      return "avdec_smackvid";
    case AV_CODEC_ID_SMC:
      return "avdec_smc";
    case AV_CODEC_ID_SMVJPEG:
      return "avdec_smvjpeg";
    case AV_CODEC_ID_SNOW:
      return "avdec_snow";
    case AV_CODEC_ID_SP5X:
      return "avdec_sp5x";
    case AV_CODEC_ID_SPEEDHQ:
      return "avdec_speedhq";
    case AV_CODEC_ID_SRGC:
      return "avdec_srgc";
    case AV_CODEC_ID_SUNRAST:
      return "avdec_sunrast";
    case AV_CODEC_ID_SVQ1:
      return "avdec_svq1";
    case AV_CODEC_ID_SVQ3:
      return "avdec_svq3";
    case AV_CODEC_ID_TARGA:
      return "avdec_targa";
    case AV_CODEC_ID_TARGA_Y216:
      return "avdec_targa_y216";
    case AV_CODEC_ID_TDSC:
      return "avdec_tdsc";
    case AV_CODEC_ID_THP:
      return "avdec_thp";
    case AV_CODEC_ID_TIERTEXSEQVIDEO:
      return "avdec_tiertexseqvideo";
    case AV_CODEC_ID_TIFF:
      return "avdec_tiff";
    case AV_CODEC_ID_TMV:
      return "avdec_tmv";
    case AV_CODEC_ID_TRUEHD:
      return "avdec_truehd";
    case AV_CODEC_ID_TRUEMOTION1:
      return "avdec_truemotion1";
    case AV_CODEC_ID_TRUEMOTION2:
      return "avdec_truemotion2";
    case AV_CODEC_ID_TRUEMOTION2RT:
      return "avdec_truemotion2rt";
    case AV_CODEC_ID_TSCC2:
      return "avdec_tscc2";
    case AV_CODEC_ID_TXD:
      return "avdec_txd";
      // case AV_CODEC_ID_ULTIMOTION:
      //   return "avdec_ultimotion";
    case AV_CODEC_ID_UTVIDEO:
      return "avdec_utvideo";
    case AV_CODEC_ID_VB:
      return "avdec_vb";
    case AV_CODEC_ID_VBLE:
      return "avdec_vble";
    case AV_CODEC_ID_VC1:
      return "avdec_vc1";
    case AV_CODEC_ID_VC1IMAGE:
      return "avdec_vc1image";
    case AV_CODEC_ID_VCR1:
      return "avdec_vcr1";
    case AV_CODEC_ID_VMDVIDEO:
      return "avdec_vmdvideo";
    case AV_CODEC_ID_VMNC:
      return "avdec_vmnc";
    case AV_CODEC_ID_VP3:
      return "avdec_vp3";
    case AV_CODEC_ID_VP4:
      return "avdec_vp4";
    case AV_CODEC_ID_VP5:
      return "avdec_vp5";
    case AV_CODEC_ID_VP6:
      return "avdec_vp6";
    case AV_CODEC_ID_VP6A:
      return "avdec_vp6a";
    case AV_CODEC_ID_VP6F:
      return "avdec_vp6f";
    case AV_CODEC_ID_VP7:
      return "avdec_vp7";
    case AV_CODEC_ID_VP8:
      return "avdec_vp8";
    case AV_CODEC_ID_VP9:
      return "avdec_vp9";
    case AV_CODEC_ID_WS_VQA:
      return "avdec_vqavideo";
    case AV_CODEC_ID_WCMV:
      return "avdec_wcmv";
    case AV_CODEC_ID_WEBP:
      return "avdec_webp";
    case AV_CODEC_ID_WMV1:
      return "avdec_wmv1";
    case AV_CODEC_ID_WMV2:
      return "avdec_wmv2";
    case AV_CODEC_ID_WMV3:
      return "avdec_wmv3";
    case AV_CODEC_ID_WMV3IMAGE:
      return "avdec_wmv3image";
    case AV_CODEC_ID_WNV1:
      return "avdec_wnv1";
    case AV_CODEC_ID_XAN_WC3:
      return "avdec_xan_wc3";
    case AV_CODEC_ID_XAN_WC4:
      return "avdec_xan_wc4";
    case AV_CODEC_ID_XBIN:
      return "avdec_xbin";
    case AV_CODEC_ID_XBM:
      return "avdec_xbm";
    case AV_CODEC_ID_XFACE:
      return "avdec_xface";
      // case AV_CODEC_ID_XL:
      //   return "avdec_xl";
    case AV_CODEC_ID_XPM:
      return "avdec_xpm";
    case AV_CODEC_ID_XWD:
      return "avdec_xwd";
    case AV_CODEC_ID_YLC:
      return "avdec_ylc";
    case AV_CODEC_ID_YOP:
      return "avdec_yop";
    case AV_CODEC_ID_ZEROCODEC:
      return "avdec_zerocodec";
    case AV_CODEC_ID_ZMBV:
      return "avdec_zmbv";
    default:
      SPDLOG_ERROR("not supported");
      return "";
  }
}

}  // namespace video_player_linux