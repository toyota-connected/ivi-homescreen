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

#pragma once

#include <GLES3/gl3.h>

#include <plugins/common/common.h>

namespace video_player_linux::nv12 {

static const GLchar* kVertexSource = R"glsl(
  #version 300 es
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

static const GLchar* kFragmentSource = R"glsl(
  #version 300 es
  precision highp float;
  in vec2 Texcoord;
  uniform sampler2D textureY;
  uniform sampler2D textureUV;
  layout(location = 0) out vec4 fragColor;
  void main() {
    float r, g, b, y, u, v;
    vec2 coord = vec2(Texcoord.x, 1.0 - Texcoord.y);
    y = texture(textureY, coord).r - 0.0625;
    u = texture(textureUV, coord).r - 0.5;
    v = texture(textureUV, coord).g - 0.5;
    r = clamp(y + 1.370705 * v, 0.0, 1.0);
    g = clamp(y - 0.337633 * u - 0.698001 * v, 0.0, 1.0);
    b = clamp(y + 1.732446 * u, 0.0, 1.0);
    fragColor = vec4(r, g, b, 1.0);
  }
)glsl";

class Shader {
 public:
  GLuint textureId{};
  GLuint framebuffer{};
  GLuint program;
  GLsizei width, height;
  GLuint vertex_arr_id_{};

  Shader(GLsizei _width, GLsizei _height) : width(_width), height(_height) {
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    glGenVertexArrays(1, &vertex_arr_id_);
    glBindVertexArray(vertex_arr_id_);

    program = load_shaders();
    texY = glGetUniformLocation(program, "textureY");
    texUV = glGetUniformLocation(program, "textureUV");
    glUseProgram(program);

    glGenTextures(2, &innerTexture[0]);
    glGenTextures(1, &textureId);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindTexture(GL_TEXTURE_2D, textureId);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           textureId, 0);

    auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
      spdlog::error("FramebufferStatus: 0x{:X}", status);
    }

    auto size = static_cast<unsigned long>(width) *
                static_cast<unsigned long>(height) * 3;
    auto pixels = new unsigned char[size]{0};
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, static_cast<GLsizei>(width),
                 static_cast<GLsizei>(height), 0, GL_RGB, GL_UNSIGNED_BYTE,
                 pixels);
    delete[] pixels;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_2D);

    glGenBuffers(1, &vertex_buffer_);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
    static constexpr GLfloat g_vertex_buffer_data[] = {
        -0.5f, 0.5f,  0.0f, 0.5f,  0.5f,  0.0f, 0.5f,  -0.5f, 0.0f,
        0.5f,  -0.5f, 0.0f, -0.5f, -0.5f, 0.0f, -0.5f, 0.5f,  0.0f,
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof(g_vertex_buffer_data),
                 g_vertex_buffer_data, GL_STATIC_DRAW);

    glGenBuffers(1, &coord_buffer_);
    glBindBuffer(GL_ARRAY_BUFFER, coord_buffer_);
    static constexpr GLfloat coord_buffer_data[] = {
        0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f,
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof(coord_buffer_data), coord_buffer_data,
                 GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, coord_buffer_);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(0);

    glFinish();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }

  ~Shader() {
    glDeleteBuffers(1, &coord_buffer_);
    glDeleteBuffers(1, &vertex_buffer_);
    glDeleteVertexArrays(1, &vertex_arr_id_);
    glDeleteTextures(1, &textureId);
    glDeleteTextures(1, &innerTexture[0]);
    glDeleteFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }

  /**
   * @brief Load pixels
   * @param[in] y_buf Pointer to image data for luminance signal
   * @param[in] uv_buf Pointer to image data for color difference signal
   * @param[in] y_p_s No use
   * @param[in] y_s Texture image width for luminance signal
   * @param[in] uv_p_s No use
   * @param[in] uv_s Texture image width for color difference signal
   * @return void
   * @relation
   * flutter
   */
  void load_pixels(unsigned char* y_buf,
                   unsigned char* uv_buf,
                   GLsizei y_p_s,
                   GLsizei y_s,
                   GLsizei uv_p_s,
                   GLsizei uv_s) {
    (void)y_p_s;
    (void)uv_p_s;
    SPDLOG_TRACE("[VideoPlayer] load_pixels");
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, innerTexture[0]);
    glUniform1i(texY, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, y_s, height, 0, GL_RED,
                 GL_UNSIGNED_BYTE, y_buf);
    glGenerateMipmap(GL_TEXTURE_2D);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, innerTexture[1]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glUniform1i(texUV, 1);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, uv_s / 2, height / 2, 0, GL_RG,
                 GL_UNSIGNED_BYTE, uv_buf);
    glGenerateMipmap(GL_TEXTURE_2D);

    auto fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbo_status != GL_FRAMEBUFFER_COMPLETE)
      spdlog::error("[VideoPlayer] Framebuffer is not complete: 0x{:X}",
                    fbo_status);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }

  GLuint load_shaders(const GLchar* vsource = kVertexSource,
                      const GLchar* fsource = kFragmentSource) {
    GLint result;
    GLsizei length;
    GLchar* info{};

    vertex_shader_ = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader_, 1, &vsource, nullptr);
    glCompileShader(vertex_shader_);
    glGetShaderiv(vertex_shader_, GL_COMPILE_STATUS, &result);
    if (result == GL_FALSE) {
      glGetShaderInfoLog(vertex_shader_, 1000, &length, info);
      SPDLOG_ERROR("Failed to compile {}", info);
      return 0;
    }

    fragment_shader_ = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader_, 1, &fsource, nullptr);
    glCompileShader(fragment_shader_);
    glGetShaderiv(fragment_shader_, GL_COMPILE_STATUS, &result);
    if (result == GL_FALSE) {
      glGetShaderInfoLog(fragment_shader_, 1000, &length, info);
      SPDLOG_ERROR("Fail to compile {}", info);
      return 0;
    }

    const GLuint shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vertex_shader_);
    glAttachShader(shaderProgram, fragment_shader_);
    glLinkProgram(shaderProgram);

    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &result);
    if (result == GL_FALSE) {
      glGetProgramInfoLog(shaderProgram, 1000, &length, info);
      SPDLOG_ERROR("Fail to link {}", info);
      return 0;
    }

    glDetachShader(shaderProgram, vertex_shader_);
    glDetachShader(shaderProgram, fragment_shader_);
    glDeleteShader(vertex_shader_);
    glDeleteShader(fragment_shader_);
    return shaderProgram;
  }

  void draw_core() const {
    SPDLOG_TRACE("[VideoPlayer] draw_core");
    glViewport(-width / 2, -height / 2, width * 2, height * 2);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(program);

    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, coord_buffer_);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);

    glFinish();
  }

  void load_rgb_pixels(const unsigned char* data) const {
    SPDLOG_DEBUG("[VideoPlayer] load_rgb_pixels");
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

 private:
  GLint texY{};
  GLint texUV{};
  GLuint innerTexture[2]{};

  GLuint vertex_shader_{};
  GLuint fragment_shader_{};

  GLuint vertex_buffer_{};
  GLuint coord_buffer_{};
};

}  // namespace video_player_linux::nv12
