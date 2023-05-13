
#include "gstreamer_egl.h"

#include <flutter/fml/paths.h>
#include <flutter/standard_message_codec.h>
#include <flutter/standard_method_codec.h>
#include <gst/gst.h>
#include <gst/video/video.h>

extern "C" {
#include <libavformat/avformat.h>
}

#include <cassert>
#include <thread>

#include "backend/wayland_egl.h"
#include "engine.h"
#include "hexdump.h"
#include "nv12.h"
#include "platform_channel.h"
#include "textures/texture.h"

#define GSTREAMER_DEBUG 0

using namespace fml;

static GLuint vertexShader, fragmentShader;

static GLuint vertex_arr_id;

static GLuint vertex_buffer;
static GLuint coord_buffer;

constexpr char kUriPrefixFile[] = "file://";

static const GLchar* vertexSource = R"glsl(
  #version 320 es
  precision highp float;

  layout(location = 0) in vec3 vertexPosition_modelspace;
  in vec2 texcoord;
  out vec2 Texcoord;
  void main()
  {
    Texcoord = texcoord;
    gl_Position.xyz = vertexPosition_modelspace;
    gl_Position.w = 1.0;
  }
)glsl";

typedef enum {
  GST_PLAY_FLAG_AUDIO = (1 << 0),
  GST_PLAY_FLAG_VIDEO = (1 << 1),
  GST_PLAY_FLAG_TEXT = (1 << 2)
} GstPlayFlags;

class CustomData {
 public:
  bool initialized = false;
  GstElement *pipeline{}, *playbin{}, *decoder{}, *video_convert{},
      *video_scale{}, *sink{};
  GMainLoop* main_loop{};
  gint n_video{};
  gint current_video{};
  gint width{}, height{};
  GstVideoInfo info{};
  AVCodecID codec_id{};
  gint64 position = 0, duration = 0;
  gdouble rate = 0.0;
  std::string uri;
  Texture* texture{};
  std::thread gthread;
  nv12::Shader* shader{};
  Engine* engine{};
  bool is_looping = false, is_buffering = false, is_live = false;
  bool events_enabled = false;
  GstState target_state = GST_STATE_PAUSED;
  double volume = 0.0;

  CustomData() : gthread{} {}

  CustomData(CustomData&&) = default;
};

static std::mutex gst_mutex;

static std::map<int64_t, std::shared_ptr<CustomData>> global_map;

#if GSTREAMER_DEBUG
#define PrintMessageAsHex(index, message)                              \
  std::stringstream ss;                                                \
  ss << Hexdump((message)->message, (message)->message_size);          \
  LOG(INFO) << "(" << (index) << ") Channel: \"" << (message)->channel \
            << "\"\n"                                                  \
            << ss.str();
#else
#define PrintMessageAsHex(a, b) \
  {                             \
    (void)a;                    \
    (void)b;                    \
  }
#endif

/**
 * @brief Send a response message
 * @param[in] engine Pointer to flutter engine
 * @param[in] response_handle Pointer to response handle
 * @return void
 * @relation
 * flutter
 */
void SendSuccess(Engine* engine,
                 const FlutterPlatformMessageResponseHandle* response_handle) {
  auto& codec = flutter::StandardMessageCodec::GetInstance();

  flutter::EncodableValue value(flutter::EncodableMap{
      {flutter::EncodableValue("result"), flutter::EncodableValue()},
      {flutter::EncodableValue("error"), flutter::EncodableValue()}});

  auto encoded = codec.EncodeMessage(value);
  engine->SendPlatformMessageResponse(response_handle, encoded->data(),
                                      encoded->size());
}

/**
 * @brief Get video info
 * @param[in] url URL of the stream
 * @param[out] width Buffer to store width info
 * @param[out] height Buffer to store height info
 * @param[out] duration Buffer to store duration info
 * @param[out] codec_id Buffer to store codec info
 * @return bool
 * @retval true Normal end
 * @retval false Abnormal end
 * @relation
 * flutter
 */
bool get_video_info(const char* url,
                    int& width,
                    int& height,
                    gint64& duration,
                    AVCodecID& codec_id) {
  AVFormatContext* fmt_ctx = avformat_alloc_context();

  if (avformat_open_input(&fmt_ctx, url, nullptr, nullptr) < 0) {
    LOG(ERROR) << "Unable to open: " << url;
    avformat_free_context(fmt_ctx);
    return false;
  }

  if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
    LOG(ERROR) << "Cannot find stream information: " << url;
    avformat_free_context(fmt_ctx);
    return false;
  }

#if defined(AVFORMAT_WITH_CONST)
  const AVCodec* dec = nullptr;
#else
  AVCodec* dec;
#endif
  int ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
  if (ret < 0) {
    DLOG(ERROR) << "Cannot find a video stream in the input file";
    avformat_free_context(fmt_ctx);
    return false;
  }
  int video_stream_index = ret;
  AVStream* stream = fmt_ctx->streams[video_stream_index];
  AVCodecParameters* par = stream->codecpar;

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

/**
 * @brief Load RGB pixels
 * @param[in] textureId Texture image id
 * @param[in] data Pointer to image data
 * @param[in] width Texture image width
 * @param[in] height Texture image height
 * @return void
 * @relation
 * flutter
 */
void loadRGBPixels(GLuint textureId,
                   unsigned char* data,
                   int width,
                   int height) {
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, textureId);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB,
               GL_UNSIGNED_BYTE, data);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  GL_LINEAR_MIPMAP_LINEAR);
  glGenerateMipmap(GL_TEXTURE_2D);
}

/**
 * @brief Draw core
 * @param[in] shader Pointer to shader
 * @return void
 * @relation
 * flutter
 */
void draw_core(nv12::Shader* shader) {
  glViewport(-shader->width / 2, -shader->height / 2, shader->width * 2,
             shader->height * 2);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glUseProgram(shader->program);

  glEnableVertexAttribArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

  glEnableVertexAttribArray(1);
  glBindBuffer(GL_ARRAY_BUFFER, coord_buffer);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

  glDrawArrays(GL_TRIANGLES, 0, 6);

  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);

  glFinish();
}

/**
 * @brief Callback called when fakesink receives new frame data
 * @param[in] fakesink No use
 * @param[in] buffer Pointer to New frame data
 * @param[in] pad No use
 * @param[in,out] _data Pointer to User data
 * @return void
 * @relation
 * flutter
 */
void handoff_handler(GstElement* fakesink,
                     GstBuffer* buffer,
                     GstPad* pad,
                     gpointer _data) {
  (void)fakesink;
  (void)pad;
  auto data = (CustomData*)_data;
  assert(data->texture);
  int64_t textureId = data->texture->GetId();

  GstVideoFrame frame;

  if (!data->initialized) {
    return;
  }
  if (data->info.finfo == nullptr) {
    // skip
    return;
  }

  std::lock_guard<std::mutex> lock(gst_mutex);
  if (gst_video_frame_map(&frame, &data->info, buffer, GST_MAP_READ)) {
    auto e = (WaylandEglBackend*)data->engine->GetBackend();
    e->MakeTextureCurrent();
    glBindVertexArray(vertex_arr_id);
    glClear(GL_COLOR_BUFFER_BIT);

    guint n_planes = GST_VIDEO_INFO_N_PLANES(&data->info);
    if (n_planes == 2) {
      // Assume NV12
      gpointer y_buf = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
      GLsizei y_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
      GLsizei y_pixel_stride = GST_VIDEO_FRAME_COMP_PSTRIDE(&frame, 0);
      gpointer uv_buf = GST_VIDEO_FRAME_PLANE_DATA(&frame, 1);
      GLsizei uv_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
      GLsizei uv_pixel_stride = GST_VIDEO_FRAME_COMP_PSTRIDE(&frame, 1);
      data->shader->loadPixels((unsigned char*)y_buf, (unsigned char*)uv_buf,
                               y_pixel_stride, y_stride, uv_pixel_stride,
                               uv_stride);
    } else {
      // Assume RGB
      gpointer video_frame_plane_buffer = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
      loadRGBPixels(static_cast<GLuint>(textureId),
                    (unsigned char*)video_frame_plane_buffer, data->info.width,
                    data->info.height);
    }
    gst_video_frame_unmap(&frame);

    glBindFramebuffer(GL_FRAMEBUFFER, data->shader->framebuffer);
    draw_core(data->shader);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    draw_core(data->shader);
    auto surface = (WaylandEglBackend*)data->engine->GetBackend();
    surface->ClearCurrent();
    data->texture->FrameReady();
  } else {
    DLOG(ERROR) << "Cannot read video frame out from buffer";
  }
}

/**
 * @brief Prepare
 * @param[in,out] data Pointer to User data
 * @return void
 * @relation
 * flutter
 */
static void prepare(CustomData* data) {
  GstElement* playbin = data->playbin;
  g_object_get(playbin, "n-video", &(data->n_video), nullptr);
  DLOG(INFO) << data->n_video << " video streams";
  g_object_get(playbin, "current-video", &(data->current_video), nullptr);
  GstPad* pad = nullptr;
  g_signal_emit_by_name(playbin, "get-video-pad", data->current_video, &pad);
  if (!pad) {
    DLOG(INFO) << "Failed to get video pad, stream number might not exist";
    g_main_loop_quit(data->main_loop);
    return;
  }
  GstCaps* caps = gst_pad_get_current_caps(pad);
  assert(caps);
  std::lock_guard<std::mutex> lock(gst_mutex);
  if (!gst_video_info_from_caps(&data->info, caps)) {
    DLOG(ERROR) << "Fail to get video info from the cap";
    g_main_loop_quit(data->main_loop);
    return;
  }
  DLOG(INFO) << "original video width: " << data->info.width
             << ", height: " << data->info.height;
  // set to the target
  if (!gst_video_info_set_format(&data->info, GST_VIDEO_FORMAT_NV12,
                                 static_cast<guint>(data->width),
                                 static_cast<guint>(data->height))) {
    DLOG(ERROR) << "Failed to set the video info to target NV12";
  }
  data->initialized = true;
}

/**
 * @brief Analyze Gst message and process the message
 * @param[in] bus Gst bus
 * @param[in] msg Gst message
 * @param[in] data Pointer to User data
 * @return gboolean
 * @retval true Normal end
 * @retval false Abnormal end
 * @relation
 * flutter
 */
static gboolean sync_bus_call(GstBus* bus, GstMessage* msg, CustomData* data) {
  GError* err;
  gchar* debug_info;
  int64_t textureId = data->texture == nullptr ? 0 : data->texture->GetId();
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR:
      gst_message_parse_error(msg, &err, &debug_info);
      DLOG(ERROR) << "Error received from element " << GST_OBJECT_NAME(msg->src)
                  << ":" << err->message;
      DLOG(ERROR) << "Debug information " << (debug_info ? debug_info : "none");
      gst_object_unref(bus);
      g_clear_error(&err);
      g_free(debug_info);
      g_main_loop_quit(data->main_loop);
      break;
    case GST_MESSAGE_EOS: {
      DLOG(INFO) << "EOS " << textureId;
      if (data->is_looping) {
        if (!gst_element_seek_simple(
                data->playbin, GST_FORMAT_TIME,
                (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                0)) {
          DLOG(ERROR) << "loop seek to 0 fail";
        }
        return TRUE;
      }
      // send completed event
      auto& codec = flutter::StandardMessageCodec::GetInstance();
      flutter::EncodableValue res(flutter::EncodableMap{
          {flutter::EncodableValue("event"),
           flutter::EncodableValue("completed")},
      });
      auto result = codec.EncodeMessage(res);

      std::stringstream ss_event;
      ss_event << GstreamerEgl::kChannelGstreamerEventPrefix << textureId;
      auto event_name = ss_event.str();
      DLOG(INFO) << "send event completed " << event_name;
      data->engine->SendPlatformMessage(event_name.c_str(), result->data(),
                                        result->size());
      break;
    }
    case GST_MESSAGE_STATE_CHANGED: {
      GstState old_state, new_state, pending_state;
      gst_message_parse_state_changed(msg, &old_state, &new_state,
                                      &pending_state);
      if (GST_MESSAGE_SRC(msg) == GST_OBJECT(data->playbin)) {
        if (new_state == GST_STATE_PLAYING) {
          DLOG(INFO) << "message state changed, start playing " << textureId;
          prepare(data);
        } else if (new_state == GST_STATE_READY) {
          DLOG(INFO) << "message state changed, ready " << textureId;
        }
      }
      break;
    }
#if 0
    case GST_MESSAGE_DURATION_CHANGED: {
      GstQuery* query;
      gboolean res;
      query = gst_query_new_duration(GST_FORMAT_TIME);
      res = gst_element_query(data->playbin, query);
      if (res) {
        gst_query_parse_duration(query, nullptr, &data->duration);
        g_print("duration = %" GST_TIME_FORMAT, GST_TIME_ARGS(data->duration));
      } else {
        DLOG(INFO) << "Duration query failed...";
      }
      gst_query_unref(query);
      break;
    }
#endif
    case GST_MESSAGE_LATENCY: {
      DLOG(INFO) << "Latency";
      break;
    }
    case GST_MESSAGE_WARNING: {
      DLOG(INFO) << "Warning";
      break;
    }
    case GST_MESSAGE_ASYNC_DONE: {
      DLOG(INFO) << "Async Done";
      // bufferingEnd
      break;
    }
    case GST_MESSAGE_NEW_CLOCK: {
      GstClock* clock;
      gst_message_parse_new_clock(msg, &clock);
      GstClockTime time = gst_clock_get_time(clock);
      DLOG(INFO) << "New Clock: " << time;
      break;
    }
    case GST_MESSAGE_BUFFERING: {
      // no state management needed for live pipelines
      if (data->is_live)
        break;

      gint percent;
      gst_message_parse_buffering(msg, &percent);
      // DLOG(INFO) << "Buffering: " << percent << "%";

      // TODO - bufferingUpdate

      if (percent == 100) {
        // a 100% message means buffering is done
        if (data->is_buffering) {
          data->is_buffering = false;
          flutter::EncodableValue res(flutter::EncodableMap{
              {flutter::EncodableValue("event"),
               flutter::EncodableValue("bufferingEnd")},
          });
          auto& codec = flutter::StandardMethodCodec::GetInstance();
          auto result = codec.EncodeSuccessEnvelope(&res);
          std::stringstream ss_event_name;
          ss_event_name << GstreamerEgl::kChannelGstreamerEventPrefix
                        << data->texture;
          auto event_name = ss_event_name.str();
          data->engine->SendPlatformMessage(event_name.c_str(), result->data(),
                                            result->size());
        }
        // if the desired state is playing, go back
        if (data->target_state == GST_STATE_PLAYING) {
          gst_element_set_state(data->playbin, GST_STATE_PLAYING);
        }
      } else {
        // buffering busy
        if (!data->is_buffering && data->target_state == GST_STATE_PLAYING) {
          // we were not buffering but PLAYING, PAUSE the pipeline
          gst_element_set_state(data->playbin, GST_STATE_PAUSED);
        }
        if (!data->is_buffering) {
          data->is_buffering = true;
          flutter::EncodableValue res(flutter::EncodableMap{
              {flutter::EncodableValue("event"),
               flutter::EncodableValue("bufferingStart")},
          });
          auto& codec = flutter::StandardMethodCodec::GetInstance();
          auto result = codec.EncodeSuccessEnvelope(&res);
          std::stringstream ss_event_name;
          ss_event_name << GstreamerEgl::kChannelGstreamerEventPrefix
                        << data->texture;
          auto event_name = ss_event_name.str();
          data->engine->SendPlatformMessage(event_name.c_str(), result->data(),
                                            result->size());
        }
      }
      break;
    }
#if 0
            case GST_MESSAGE_STREAM_STATUS: {
              GstStreamStatusType type;
              GstElement* owner;
              const GValue* val;
              gchar* path;
              GstTask* task = nullptr;

              DLOG(INFO) << "STREAM_STATUS:";
              gst_message_parse_stream_status(msg, &type, &owner);

              val = gst_message_get_stream_status_object(msg);

              DLOG(INFO) << "\ttype:   " << type;
              path = gst_object_get_path_string(GST_MESSAGE_SRC(msg));
              DLOG(INFO) << "\tsource: " << path;
              g_free(path);
              path = gst_object_get_path_string(GST_OBJECT(owner));
              DLOG(INFO) << "\towner:  " << path;
              g_free(path);
              DLOG(INFO) << "\tobject: type " << G_VALUE_TYPE_NAME(val)
                             << ", value " << g_value_get_object(val);

              /* see if we know how to deal with this object */
              if (G_VALUE_TYPE(val) == GST_TYPE_TASK) {
                task = GST_TASK(g_value_get_object(val));
              }

              switch (type) {
                case GST_STREAM_STATUS_TYPE_CREATE:
                  DLOG(INFO) << "Created task: " << task;
                  break;
                case GST_STREAM_STATUS_TYPE_ENTER:
                  // DLOG(INFO) << "raising task priority";
                  // setpriority (PRIO_PROCESS, 0, -10);
                  break;
                case GST_STREAM_STATUS_TYPE_LEAVE:
                  break;
                default:
                  break;
              }
              break;
            }
#endif
    case GST_MESSAGE_RESET_TIME: {
      GstClockTime running_time;
      gst_message_parse_reset_time(msg, &running_time);
      if (running_time > 0) {
        g_print("reset-time: %" GST_TIME_FORMAT, GST_TIME_ARGS(running_time));
      }
      break;
    }
#if GSTREAMER_DEBUG
    // element specific message
    case GST_MESSAGE_ELEMENT: {
      DLOG(INFO) << "message-element: "
                 << gst_structure_get_name(gst_message_get_structure(msg));
      break;
    }
    case GST_MESSAGE_TAG: {
      GstTagList* tags = nullptr;
      gst_message_parse_tag(msg, &tags);
      DLOG(INFO) << "Got tags from element " << GST_OBJECT_NAME(msg->src);
      // handle_tags (tags);
      gst_tag_list_unref(tags);
      break;
    }
#endif
    default:
#if GSTREAMER_DEBUG
    {
      DLOG(INFO) << "GST Message Type: "
                 << gst_message_type_get_name(GST_MESSAGE_TYPE(msg));
      break;
    }
#else
        ;
#endif
  }
  return TRUE;
}

/**
 * @brief Load shaders
 * @param[in] vsource Source code to load into vertexShader
 * @param[in] fsource Source code to load into fragmentShader
 * @return GLuint
 * @retval Non-zero Normal end
 * @retval 0 Abnormal end
 * @relation
 * flutter
 */
GLuint LoadShaders(const GLchar* vsource, const GLchar* fsource) {
  GLuint shaderProgram;
  GLint result;
  GLsizei length;
  GLchar* info{};

  vertexShader = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vertexShader, 1, &vsource, nullptr);
  glCompileShader(vertexShader);
  glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &result);
  if (result == GL_FALSE) {
    glGetShaderInfoLog(vertexShader, 1000, &length, info);
    DLOG(ERROR) << "Failed to compile " << info;
    return 0;
  }

  fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fragmentShader, 1, &fsource, nullptr);
  glCompileShader(fragmentShader);
  glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &result);
  if (result == GL_FALSE) {
    glGetShaderInfoLog(fragmentShader, 1000, &length, info);
    DLOG(ERROR) << "Fail to compile " << info;
    return 0;
  }

  shaderProgram = glCreateProgram();
  glAttachShader(shaderProgram, vertexShader);
  glAttachShader(shaderProgram, fragmentShader);
  glLinkProgram(shaderProgram);

  glGetProgramiv(shaderProgram, GL_LINK_STATUS, &result);
  if (result == GL_FALSE) {
    glGetProgramInfoLog(shaderProgram, 1000, &length, info);
    DLOG(ERROR) << "Fail to link " << info;
    return 0;
  }

  glDetachShader(shaderProgram, vertexShader);
  glDetachShader(shaderProgram, fragmentShader);
  glDeleteShader(vertexShader);
  glDeleteShader(fragmentShader);
  return shaderProgram;
}

/**
 * @brief Return GStreamer plugin for codec ID
 * @param[in] codec_id Codec ID
 * @return char*
 * @retval GStreamer plugin
 * @relation
 * flutter
 */
const char* map_ffmpeg_plugin(AVCodecID codec_id) {
  switch (codec_id) {
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
      // case AV_CODEC_ID_CAMSTUDIO:
      //   return "avdec_camstudio";
      // case AV_CODEC_ID_CAMTASIA:
      //   return "avdec_camtasia";
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
      DLOG(ERROR) << "not supported";
      return "";
  }
}

/**
 * @brief Main Loop
 * @param[in,out] data Pointer to User data
 * @return void
 * @relation
 * flutter
 */
void main_loop(CustomData* data) {
  DLOG(INFO) << "(" << data->engine->GetIndex()
             << ") [main_loop] start data thread "
             << "- textureId: " << data->texture->GetId();
  GMainContext* context = g_main_context_new();
  g_main_context_push_thread_default(context);

  data->playbin = gst_element_factory_make("playbin", nullptr);
  assert(data->playbin);
  g_object_set(data->playbin, "uri", data->uri.c_str(), nullptr);

  gint flags = 0;
  g_object_get(data->playbin, "flags", &flags, nullptr);
  flags |= GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO;
  flags &= ~GST_PLAY_FLAG_TEXT;
  g_object_set(data->playbin, "flags", flags, nullptr);
  g_object_set(data->playbin, "connection-speed", 56, nullptr);

  data->sink = gst_element_factory_make("fakesink", nullptr);
  assert(data->sink);
  g_object_set(data->sink, "sync", TRUE, nullptr);
  g_object_set(data->sink, "signal-handoffs", TRUE, nullptr);
  g_object_set(data->sink, "can-activate-pull", TRUE, nullptr);
  gulong sig =
      g_signal_connect(data->sink, "handoff", (GCallback)handoff_handler, data);
  DLOG(INFO) << "(" << data->engine->GetIndex()
             << ") [main_loop] register signal " << sig;

  auto decoder_factory =
      gst_element_factory_find(map_ffmpeg_plugin(data->codec_id));
  if (decoder_factory == nullptr) {
    LOG(ERROR) << "(" << data->engine->GetIndex()
               << ") Failed to find decoder: "
               << map_ffmpeg_plugin(data->codec_id);
  }
  data->decoder = gst_element_factory_create(decoder_factory, "decoder");
  assert(data->decoder);

  data->video_convert = gst_element_factory_make("videoconvert", nullptr);
  assert(data->video_convert);

  GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING,
                                      "NV12", nullptr);

  data->video_scale = gst_element_factory_make("videoscale", nullptr);
  assert(data->video_scale);

  GstCaps* scale =
      gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT, data->width,
                          "height", G_TYPE_INT, data->height, nullptr);

  data->pipeline = gst_bin_new(nullptr);

  gst_bin_add_many((GstBin*)data->pipeline, data->decoder, data->video_convert,
                   data->video_scale, data->sink, nullptr);
  if (!gst_element_link(data->decoder, data->video_convert)) {
    DLOG(ERROR) << "(" << data->engine->GetIndex()
                << ") Failed to link decoder with videoconvert";
  }

  if (!gst_element_link_filtered(data->video_convert, data->video_scale,
                                 caps)) {
    DLOG(ERROR) << "(" << data->engine->GetIndex()
                << ") Failed to link videoconvert with videoscale using filter";
  }

  gst_caps_unref(caps);

  GstPad* pad = gst_element_get_static_pad(data->decoder, "sink");
  if (gst_pad_is_linked(pad)) {
    DLOG(ERROR) << "(" << data->engine->GetIndex()
                << ") already linked, ignore";
    return;
  }
  GstPad* ghost_pad = gst_ghost_pad_new("sink", pad);
  gst_pad_set_active(ghost_pad, TRUE);
  gst_element_add_pad(data->pipeline, ghost_pad);
  gst_object_unref(pad);

  if (!gst_element_link_filtered(data->video_scale, data->sink, scale)) {
    DLOG(ERROR) << "(" << data->engine->GetIndex()
                << ") Failed to link videoscale with fakesink using filter";
  }
  gst_caps_unref(scale);

  g_object_set(data->playbin, "video-sink", data->pipeline, nullptr);

  GstBus* bus = gst_element_get_bus(data->playbin);
  GSource* bus_source = gst_bus_create_watch(bus);
  g_source_set_callback(bus_source, (GSourceFunc)gst_bus_async_signal_func,
                        nullptr, nullptr);
  g_source_attach(bus_source, context);
  g_source_unref(bus_source);
  g_signal_connect(bus, "message", (GCallback)sync_bus_call, data);
  gst_object_unref(bus);

  data->main_loop = g_main_loop_new(context, FALSE);
  g_main_loop_run(data->main_loop);
  g_main_loop_unref(data->main_loop);
  data->main_loop = nullptr;
  DLOG(INFO) << "(" << data->engine->GetIndex() << ") [main_loop] mainloop end";
}

void GstreamerEgl::OnInitialize(const FlutterPlatformMessage* message,
                                void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  PrintMessageAsHex(engine->GetIndex(), message);

  auto platform_channel = PlatformChannel::GetInstance();
  platform_channel->RegisterCallback(kChannelGstreamerCreate, OnCreate);
  platform_channel->RegisterCallback(kChannelGstreamerDispose, OnDispose);
  platform_channel->RegisterCallback(kChannelGstreamerSetLooping, OnSetLooping);
  platform_channel->RegisterCallback(kChannelGstreamerSetVolume, OnSetVolume);
  platform_channel->RegisterCallback(kChannelGstreamerSetPlaybackSpeed,
                                     OnSetPlaybackSpeed);
  platform_channel->RegisterCallback(kChannelGstreamerPlay, OnPlay);
  platform_channel->RegisterCallback(kChannelGstreamerPosition, OnPosition);
  platform_channel->RegisterCallback(kChannelGstreamerSeekTo, OnSeekTo);
  platform_channel->RegisterCallback(kChannelGstreamerPause, OnPause);
  platform_channel->RegisterCallback(kChannelGstreamerSetMixWithOthers,
                                     &GstreamerEgl::OnSetMixWithOthers);

  SendSuccess(engine, message->response_handle);
}

/**
 * @brief OnEvent method
 * @param[in] message Receive message
 * @param[in] userdata Pointer to User data
 * @return void
 * @relation
 * flutter
 */
void OnEvent(const FlutterPlatformMessage* message, void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  PrintMessageAsHex(engine->GetIndex(), message);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);
  auto method = obj->method_name();

  auto textureId = static_cast<GLuint>(
      static_cast<GLuint>(strtol(&message->channel[34], nullptr, 10)));
  std::shared_ptr<CustomData> data = global_map[textureId];

  if (method == "listen") {
    data->events_enabled = true;
    DLOG(INFO) << "Video Player Event Register: listen " << textureId;

    // send initialized event
    flutter::EncodableValue res(flutter::EncodableMap{
        {flutter::EncodableValue("event"),
         flutter::EncodableValue("initialized")},
        {flutter::EncodableValue("duration"),
         flutter::EncodableValue((int)(data->duration / AV_TIME_BASE))},
        {flutter::EncodableValue("width"),
         flutter::EncodableValue(data->info.width)},
        {flutter::EncodableValue("height"),
         flutter::EncodableValue(data->info.height)},
    });
    auto result = codec.EncodeSuccessEnvelope(&res);
    engine->SendPlatformMessage(message->channel, result->data(),
                                result->size());
    return;
  } else if (method == "cancel") {
    DLOG(INFO) << "Video Player Event cancel " << textureId;
    data->events_enabled = false;
    auto result = codec.EncodeSuccessEnvelope();
    engine->SendPlatformMessageResponse(message->response_handle,
                                        result->data(), result->size());
  }
}

void GstreamerEgl::OnCreate(const FlutterPlatformMessage* message,
                            void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  PrintMessageAsHex(engine->GetIndex(), message);
  auto& codec = flutter::StandardMessageCodec::GetInstance();
  auto obj = codec.DecodeMessage(message->message, message->message_size);
  flutter::EncodableValue val = *obj;
  auto args = std::get_if<flutter::EncodableMap>(&val);
  auto data = new CustomData();
  data->engine = engine;
  data->initialized = false;
  data->events_enabled = false;
  auto it = args->find(flutter::EncodableValue("uri"));
  if (it != args->end() && !it->second.IsNull()) {
    if (std::holds_alternative<std::string>(it->second)) {
      data->uri = std::get<std::string>(it->second);
      if (data->uri.empty()) {
        DLOG(ERROR) << "(" << data->engine->GetIndex() << ") uri is empty";
        return;
      }
      DLOG(INFO) << "(" << data->engine->GetIndex()
                 << ") load uri: " << data->uri;
    }
  }
  it = args->find(flutter::EncodableValue("asset"));
  if (it != args->end() && !it->second.IsNull()) {
    if (std::holds_alternative<std::string>(it->second)) {
      std::string asset_path = std::get<std::string>(it->second);
      DLOG(INFO) << "(" << data->engine->GetIndex()
                 << ") asset_path: " << asset_path;
      if (asset_path[0] == '/') {
        data->uri = paths::JoinPaths({kUriPrefixFile, asset_path});
      } else {
        data->uri = paths::JoinPaths(
            {kUriPrefixFile, engine->GetAssetDirectory(), asset_path});
      }
      DLOG(INFO) << "(" << data->engine->GetIndex()
                 << ") asset uri: " << data->uri;
    }
  }

  // Get stream information
  if (!get_video_info(data->uri.c_str(), data->info.width, data->info.height,
                      data->duration, data->codec_id)) {
    LOG(ERROR) << "(" << data->engine->GetIndex()
               << ") Failed to get video info";
  }

  it = args->find(flutter::EncodableValue("width"));
  if (it != args->end() && !it->second.IsNull()) {
    flutter::EncodableValue encodedValue = it->second;

    data->width = std::get<int32_t>(encodedValue);
  } else {
    data->width = data->info.width;
  }
  it = args->find(flutter::EncodableValue("height"));
  if (it != args->end() && !it->second.IsNull()) {
    flutter::EncodableValue encodedValue = it->second;
    data->height = std::get<int32_t>(encodedValue);
  } else {
    data->height = data->info.height;
  }

  it = args->find(flutter::EncodableValue("packageName"));
  if (it != args->end() && !it->second.IsNull()) {
    if (std::holds_alternative<std::string>(it->second)) {
      DLOG(INFO) << "(" << data->engine->GetIndex()
                 << ") packageName: " << std::get<std::string>(it->second);
    }
  }
  it = args->find(flutter::EncodableValue("formatHint"));
  if (it != args->end() && !it->second.IsNull()) {
    if (std::holds_alternative<std::string>(it->second)) {
      DLOG(INFO) << "(" << data->engine->GetIndex()
                 << ") formatHint: " << std::get<std::string>(it->second);
    }
  }
  it = args->find(flutter::EncodableValue("httpHeaders"));
  if (it != args->end() && !it->second.IsNull()) {
    if (std::holds_alternative<std::string>(it->second)) {
      DLOG(INFO) << "(" << data->engine->GetIndex()
                 << ") httpHeaders: " << std::get<std::string>(it->second);
    }
  }

  auto textureId = static_cast<GLuint>(-1);

  std::lock_guard<std::mutex> lock(gst_mutex);

  gst_init(nullptr, nullptr);
  auto surface = (WaylandEglBackend*)data->engine->GetBackend();
  surface->MakeTextureCurrent();

  glGenVertexArrays(1, &vertex_arr_id);
  glBindVertexArray(vertex_arr_id);

  GLuint texture;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  textureId = texture;

  glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  gint size = data->width * data->height * 3;
  auto buffer = new unsigned char[static_cast<unsigned long>(size)]{0};

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, data->width, data->height, 0, GL_RGB,
               GL_UNSIGNED_BYTE, buffer);
  delete[] buffer;

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenerateMipmap(GL_TEXTURE_2D);

  DLOG(INFO) << "(" << data->engine->GetIndex()
             << ") fetch Texture: " << textureId;
  data->texture =
      new Texture(textureId, GL_TEXTURE_2D, GL_RGBA8, nullptr, nullptr);
  auto engine_shr = std::shared_ptr<Engine>(engine);
  data->texture->SetEngine(engine_shr.get());

  data->shader =
      new nv12::Shader(LoadShaders(vertexSource, nv12::fragmentSource),
                       textureId, data->width, data->height);

  static const GLfloat g_vertex_buffer_data[] = {
      -0.5f, 0.5f,  0.0f, 0.5f,  0.5f,  0.0f, 0.5f,  -0.5f, 0.0f,

      0.5f,  -0.5f, 0.0f, -0.5f, -0.5f, 0.0f, -0.5f, 0.5f,  0.0f,
  };

  glGenBuffers(1, &vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(g_vertex_buffer_data),
               g_vertex_buffer_data, GL_STATIC_DRAW);

  static const GLfloat coord_buffer_data[] = {
      0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f,

      1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f,
  };
  glGenBuffers(1, &coord_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, coord_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(coord_buffer_data), coord_buffer_data,
               GL_STATIC_DRAW);

  glEnableVertexAttribArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(1);
  glBindBuffer(GL_ARRAY_BUFFER, coord_buffer);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(0);

  glFinish();
  surface->ClearCurrent();
  data->texture->Enable(textureId);
  DLOG(INFO) << "(" << engine->GetIndex() << ") Register "
             << data->texture->GetId() << " done";

  std::stringstream ss_event_name;
  ss_event_name << kChannelGstreamerEventPrefix << textureId;
  auto event_name = ss_event_name.str();
  DLOG(INFO) << "(" << engine->GetIndex()
             << ") Register Stream: " << event_name;

  global_map[textureId] = std::shared_ptr<CustomData>(data);
  PlatformChannel::GetInstance()->RegisterCallback(event_name.c_str(), OnEvent);

  data->gthread = std::thread{main_loop, data};

  flutter::EncodableValue result(
      flutter::EncodableMap{{flutter::EncodableValue("textureId"),
                             flutter::EncodableValue((int32_t)textureId)}});

  flutter::EncodableValue value(flutter::EncodableMap{
      {flutter::EncodableValue("result"), result},
      {flutter::EncodableValue("error"), flutter::EncodableValue()}});

  auto encoded = codec.EncodeMessage(value);
  engine->SendPlatformMessageResponse(message->response_handle, encoded->data(),
                                      encoded->size());
}

/**
 * @brief Encode error messages for OnDispose method
 * @param[in] error_msg Error message
 * @return flutter::EncodableValue
 * @retval Value after encoding
 * @relation
 * flutter
 */
flutter::EncodableValue dispose_error(const char* error_msg) {
  DLOG(ERROR) << "[dispose error] " << error_msg;
  return flutter::EncodableValue(flutter::EncodableMap{
      {flutter::EncodableValue("result"), flutter::EncodableValue()},
      {flutter::EncodableValue("error"),
       flutter::EncodableValue(flutter::EncodableMap{
           {flutter::EncodableValue("code"), flutter::EncodableValue("")},
           {flutter::EncodableValue("message"),
            flutter::EncodableValue("dispose error")},
           {flutter::EncodableValue("details"),
            flutter::EncodableValue(error_msg)},
       })},
  });
}

void GstreamerEgl::OnDispose(const FlutterPlatformMessage* message,
                             void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  PrintMessageAsHex(engine->GetIndex(), message);
  auto& codec = flutter::StandardMessageCodec::GetInstance();
  auto obj = codec.DecodeMessage(message->message, message->message_size);
  flutter::EncodableValue val = *obj;
  auto args = std::get_if<flutter::EncodableMap>(&val);

  auto it = args->find(flutter::EncodableValue("textureId"));
  if (it == args->end() || it->second.IsNull()) {
    auto value = dispose_error("textureId not provided");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }

  auto textureId = static_cast<GLuint>(std::get<int>(it->second));

  engine->TextureDispose(textureId);
  auto search = global_map.find(textureId);
  if (search == global_map.end()) {
    auto value = dispose_error("Unable to find textureId");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }

  std::shared_ptr<CustomData> data = search->second;

  gst_object_unref(data->pipeline);
  data->target_state = GST_STATE_NULL;
  GstStateChangeReturn ret =
      gst_element_set_state(data->playbin, GST_STATE_NULL);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    auto value =
        dispose_error("Unable to see the pipeline change to play state");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    g_main_loop_quit(data->main_loop);
    gst_object_unref(data->pipeline);
    data->gthread.join();
    return;
  }
  g_main_loop_quit(data->main_loop);
  g_main_loop_unref(data->main_loop);
  data->gthread.join();
  DLOG(INFO) << "dispose done";

  SendSuccess(engine, message->response_handle);
}

/**
 * @brief Encode error messages for OnSetLooping method
 * @param[in] error_msg Error message
 * @return flutter::EncodableValue
 * @retval Value after encoding
 * @relation
 * flutter
 */
flutter::EncodableValue setLooping_error(const char* error_msg) {
  DLOG(ERROR) << "[setLooping error] " << error_msg;
  return flutter::EncodableValue(flutter::EncodableMap{
      {flutter::EncodableValue("result"), flutter::EncodableValue()},
      {flutter::EncodableValue("error"),
       flutter::EncodableValue(flutter::EncodableMap{
           {flutter::EncodableValue("code"), flutter::EncodableValue("")},
           {flutter::EncodableValue("message"),
            flutter::EncodableValue("setLooping error")},
           {flutter::EncodableValue("details"),
            flutter::EncodableValue(error_msg)},
       })},
  });
}

void GstreamerEgl::OnSetLooping(const FlutterPlatformMessage* message,
                                void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  PrintMessageAsHex(engine->GetIndex(), message);
  auto& codec = flutter::StandardMessageCodec::GetInstance();
  auto obj = codec.DecodeMessage(message->message, message->message_size);
  flutter::EncodableValue val = *obj;
  auto args = std::get_if<flutter::EncodableMap>(&val);

  auto it = args->find(flutter::EncodableValue("textureId"));
  if (it == args->end() || it->second.IsNull()) {
    auto value = setLooping_error("setLooping requires textureId");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }

  auto textureId = static_cast<GLuint>(std::get<int>(it->second));

  auto search = global_map.find(textureId);
  if (search == global_map.end()) {
    auto value = setLooping_error("setLooping textureId not found");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }
  std::shared_ptr<CustomData> data = search->second;
  it = args->find(flutter::EncodableValue("isLooping"));
  if (it == args->end()) {
    auto value = setLooping_error("setLooping requires isLooping");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }
  if (std::holds_alternative<bool>(it->second)) {
    data->is_looping = std::get<bool>(it->second);
    DLOG(INFO) << "is_looping: " << data->is_looping;
  }

  SendSuccess(engine, message->response_handle);
}

// TODO - implementation specific
void GstreamerEgl::OnSetVolume(const FlutterPlatformMessage* message,
                               void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  PrintMessageAsHex(engine->GetIndex(), message);
  auto& codec = flutter::StandardMessageCodec::GetInstance();
  auto obj = codec.DecodeMessage(message->message, message->message_size);
  flutter::EncodableValue val = *obj;
  auto args = std::get_if<flutter::EncodableMap>(&val);

  auto it = args->find(flutter::EncodableValue("textureId"));
  if (it == args->end() || it->second.IsNull()) {
    auto value = setLooping_error("setVolume requires textureId");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }

  auto textureId = static_cast<GLuint>(std::get<int>(it->second));

  auto search = global_map.find(textureId);
  if (search == global_map.end()) {
    auto value = setLooping_error("setVolume textureId not found");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }
  std::shared_ptr<CustomData> data = search->second;
  it = args->find(flutter::EncodableValue("volume"));
  if (it == args->end() || it->second.IsNull()) {
    auto value = setLooping_error("setVolume requires volume");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }
  data->volume = std::get<double>(it->second);

  DLOG(INFO) << "volume: " << data->volume;

  SendSuccess(engine, message->response_handle);
}

void GstreamerEgl::OnSetPlaybackSpeed(const FlutterPlatformMessage* message,
                                      void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  PrintMessageAsHex(engine->GetIndex(), message);
  auto& codec = flutter::StandardMessageCodec::GetInstance();
  auto obj = codec.DecodeMessage(message->message, message->message_size);
  flutter::EncodableValue val = *obj;
  auto args = std::get_if<flutter::EncodableMap>(&val);

  auto it = args->find(flutter::EncodableValue("textureId"));
  if (it == args->end() || it->second.IsNull()) {
    auto value = setLooping_error("setPlaybackSpeed requires textureId");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }

  auto textureId = static_cast<GLuint>(std::get<int>(it->second));

  auto search = global_map.find(textureId);
  if (search == global_map.end()) {
    auto value = setLooping_error("setPlaybackSpeed textureId not found");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }
  std::shared_ptr<CustomData> data = search->second;
  it = args->find(flutter::EncodableValue("speed"));
  if (it == args->end() || it->second.IsNull()) {
    auto value = setLooping_error("setPlaybackSpeed requires speed");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }
  data->rate = std::get<double>(it->second);

  gint64 position;
  if (!gst_element_query_position(data->playbin, GST_FORMAT_TIME, &position)) {
    LOG(ERROR) << "Unable to retrieve current position";
    return;
  }

  GstEvent* seek_event;
  if (data->rate > 0) {
    seek_event = gst_event_new_seek(
        data->rate, GST_FORMAT_TIME,
        (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        GST_SEEK_TYPE_SET, position, GST_SEEK_TYPE_END, 0);
  } else {
    seek_event = gst_event_new_seek(
        data->rate, GST_FORMAT_TIME,
        (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_SET, position);
  }

  gst_element_send_event(data->sink, seek_event);

  DLOG(INFO) << "Playback speed: " << data->rate;

  SendSuccess(engine, message->response_handle);
}

/**
 * @brief Encode error messages for OnPlay method
 * @param[in] error_msg Error message
 * @return flutter::EncodableValue
 * @retval Value after encoding
 * @relation
 * flutter
 */
flutter::EncodableValue play_error(const char* error_msg) {
  DLOG(ERROR) << "[play error] " << error_msg;
  return flutter::EncodableValue(flutter::EncodableMap{
      {flutter::EncodableValue("result"), flutter::EncodableValue()},
      {flutter::EncodableValue("error"),
       flutter::EncodableValue(flutter::EncodableMap{
           {flutter::EncodableValue("code"), flutter::EncodableValue("")},
           {flutter::EncodableValue("message"),
            flutter::EncodableValue("play error")},
           {flutter::EncodableValue("details"),
            flutter::EncodableValue(error_msg)},
       })},
  });
}

void GstreamerEgl::OnPlay(const FlutterPlatformMessage* message,
                          void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  PrintMessageAsHex(engine->GetIndex(), message);
  auto& codec = flutter::StandardMessageCodec::GetInstance();
  auto obj = codec.DecodeMessage(message->message, message->message_size);
  flutter::EncodableValue val = *obj;
  auto args = std::get_if<flutter::EncodableMap>(&val);

  auto it = args->find(flutter::EncodableValue("textureId"));
  if (it == args->end() || it->second.IsNull()) {
    auto value = play_error("play requires textureId");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }
  auto textureId = static_cast<GLuint>(std::get<int>(it->second));

  auto search = global_map.find(textureId);
  if (search == global_map.end()) {
    auto value = play_error("play textureId not found");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }
  std::shared_ptr<CustomData> data = search->second;
  data->target_state = GST_STATE_PLAYING;
  GstStateChangeReturn ret =
      gst_element_set_state(data->playbin, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    auto value = play_error("Unable to see the pipeline to play state");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }

  SendSuccess(engine, message->response_handle);
}

/**
 * @brief Encode error messages for OnPosition method
 * @param[in] error_msg Error message
 * @return flutter::EncodableValue
 * @retval Value after encoding
 * @relation
 * flutter
 */
flutter::EncodableValue position_error(const char* error_msg) {
  DLOG(ERROR) << "[position error] " << error_msg;
  return flutter::EncodableValue(flutter::EncodableMap{
      {flutter::EncodableValue("result"), flutter::EncodableValue()},
      {flutter::EncodableValue("error"),
       flutter::EncodableValue(flutter::EncodableMap{
           {flutter::EncodableValue("code"), flutter::EncodableValue("")},
           {flutter::EncodableValue("message"),
            flutter::EncodableValue("position error")},
           {flutter::EncodableValue("details"),
            flutter::EncodableValue(error_msg)},
       })},
  });
}

void GstreamerEgl::OnPosition(const FlutterPlatformMessage* message,
                              void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  PrintMessageAsHex(engine->GetIndex(), message);
  auto& codec = flutter::StandardMessageCodec::GetInstance();
  auto obj = codec.DecodeMessage(message->message, message->message_size);
  flutter::EncodableValue val = *obj;
  auto args = std::get_if<flutter::EncodableMap>(&val);

  auto it = args->find(flutter::EncodableValue("textureId"));
  if (it == args->end() || it->second.IsNull()) {
    auto value = position_error("textureId required");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }
  auto textureId = static_cast<GLuint>(std::get<int>(it->second));

  auto search = global_map.find(textureId);
  if (search == global_map.end()) {
    auto value = position_error("textureId not found");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }
  auto data = search->second;

  if (gst_element_query_position(data->playbin, GST_FORMAT_TIME,
                                 &data->position) &&
      gst_element_query_duration(data->playbin, GST_FORMAT_TIME,
                                 &data->duration)) {
#if GSTREAMER_DEBUG
    DLOG(INFO) << "position: " << data->position / AV_TIME_BASE;
#endif
  }

  flutter::EncodableMap output{
      {flutter::EncodableValue("result"),
       flutter::EncodableValue(flutter::EncodableMap{
           {flutter::EncodableValue("textureId"),
            flutter::EncodableValue((int32_t)textureId)},
           {flutter::EncodableValue("position"),
            flutter::EncodableValue(
                data->position ? (data->position / AV_TIME_BASE) : 0)},
       })},
      {flutter::EncodableValue("error"), flutter::EncodableValue()},
  };

  auto value = flutter::EncodableValue(output);
  auto encoded = codec.EncodeMessage(value);
  engine->SendPlatformMessageResponse(message->response_handle, encoded->data(),
                                      encoded->size());
}

/**
 * @brief Encode error messages for OnSeekTo method
 * @param[in] error_msg Error message
 * @return flutter::EncodableValue
 * @retval Value after encoding
 * @relation
 * flutter
 */
flutter::EncodableValue seekTo_error(const char* error_msg) {
  DLOG(ERROR) << "[seekTo error] " << error_msg;
  return flutter::EncodableValue(flutter::EncodableMap{
      {flutter::EncodableValue("result"), flutter::EncodableValue()},
      {flutter::EncodableValue("error"),
       flutter::EncodableValue(flutter::EncodableMap{
           {flutter::EncodableValue("code"), flutter::EncodableValue("")},
           {flutter::EncodableValue("message"),
            flutter::EncodableValue("seekTo error")},
           {flutter::EncodableValue("details"),
            flutter::EncodableValue(error_msg)},
       })},
  });
}

void GstreamerEgl::OnSeekTo(const FlutterPlatformMessage* message,
                            void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  PrintMessageAsHex(engine->GetIndex(), message);
  auto& codec = flutter::StandardMessageCodec::GetInstance();
  auto obj = codec.DecodeMessage(message->message, message->message_size);
  flutter::EncodableValue val = *obj;
  auto args = std::get_if<flutter::EncodableMap>(&val);

  auto it = args->find(flutter::EncodableValue("textureId"));
  if (it == args->end() || it->second.IsNull()) {
    auto value = seekTo_error("textureId required");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
  }
  auto textureId = static_cast<GLuint>(std::get<int>(it->second));

  auto search = global_map.find(textureId);
  if (search == global_map.end()) {
    auto value = seekTo_error("cannot find textureId");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }
  std::shared_ptr<CustomData> data = search->second;

  it = args->find(flutter::EncodableValue("position"));
  if (it == args->end() || it->second.IsNull()) {
    auto value = seekTo_error("position required");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }
  int pos = std::get<int>(it->second);
  gint64 position = pos * AV_TIME_BASE;

  if (!gst_element_seek_simple(
          data->playbin, GST_FORMAT_TIME,
          (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
          position)) {
    auto value = seekTo_error("seek failed");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }

  DLOG(INFO) << "(" << engine->GetIndex() << ") textureId: " << textureId
             << ", seek to " << position;

  SendSuccess(engine, message->response_handle);
}

/**
 * @brief Encode error messages for OnPause method
 * @param[in] error_msg Error message
 * @return flutter::EncodableValue
 * @retval Value after encoding
 * @relation
 * flutter
 */
flutter::EncodableValue pause_error(const char* error_msg) {
  DLOG(ERROR) << "[pause error] " << error_msg;
  return flutter::EncodableValue(flutter::EncodableMap{
      {flutter::EncodableValue("result"), flutter::EncodableValue()},
      {flutter::EncodableValue("error"),
       flutter::EncodableValue(flutter::EncodableMap{
           {flutter::EncodableValue("code"), flutter::EncodableValue("")},
           {flutter::EncodableValue("message"),
            flutter::EncodableValue("pause error")},
           {flutter::EncodableValue("details"),
            flutter::EncodableValue(error_msg)},
       })},
  });
}

void GstreamerEgl::OnPause(const FlutterPlatformMessage* message,
                           void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  PrintMessageAsHex(engine->GetIndex(), message);
  auto index = engine->GetIndex();
  auto& codec = flutter::StandardMessageCodec::GetInstance();
  auto obj = codec.DecodeMessage(message->message, message->message_size);
  flutter::EncodableValue val = *obj;
  auto args = std::get_if<flutter::EncodableMap>(&val);

  auto it = args->find(flutter::EncodableValue("textureId"));
  if (it == args->end() || it->second.IsNull()) {
    auto value = pause_error("textureId required");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }
  auto textureId = static_cast<GLuint>(std::get<int>(it->second));

  auto search = global_map.find(textureId);
  if (search == global_map.end()) {
    auto value = pause_error("texture not found");
    auto encoded = codec.EncodeMessage(value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  }

  DLOG(INFO) << "(" << index << ") Pause: " << textureId;

  std::shared_ptr<CustomData> data = search->second;
  GstState state;
  gst_element_get_state(data->playbin, &state, nullptr, GST_CLOCK_TIME_NONE);
  if (state != GST_STATE_NULL) {
    DLOG(INFO) << "(" << index << ") Set pipeline to pause state";
    data->target_state = GST_STATE_PAUSED;
    GstStateChangeReturn ret =
        gst_element_set_state(data->playbin, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
      auto value = pause_error("Unable to change pipeline to pause state.");
      auto encoded = codec.EncodeMessage(value);
      engine->SendPlatformMessageResponse(message->response_handle,
                                          encoded->data(), encoded->size());
      return;
    }
  }

  SendSuccess(engine, message->response_handle);
}

void GstreamerEgl::OnSetMixWithOthers(const FlutterPlatformMessage* message,
                                      void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  PrintMessageAsHex(engine->GetIndex(), message);
  auto& codec = flutter::StandardMessageCodec::GetInstance();
  auto obj = codec.DecodeMessage(message->message, message->message_size);
  flutter::EncodableValue val = *obj;
  auto args = std::get_if<flutter::EncodableMap>(&val);

  auto it = args->find(flutter::EncodableValue("mixWithOthers"));
  if (it != args->end() && !it->second.IsNull()) {
    DLOG(INFO) << "(" << engine->GetIndex()
               << ") mixWithOthers: " << std::get<bool>(it->second);
  }

  SendSuccess(engine, message->response_handle);
}
