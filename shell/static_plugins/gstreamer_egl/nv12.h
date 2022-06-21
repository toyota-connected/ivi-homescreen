#include "wayland/window.h"

#include <GLES3/gl3.h>

namespace nv12 {

const GLchar* fragmentSource = R"glsl(
  #version 320 es
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
  GLint texY;
  GLint texUV;
  GLuint innerTexture[2]{};
  GLuint program;
  GLuint framebuffer{};
#if NV12_DEPTH_RENDERBUFFER
  GLuint depth_renderbuffer;
#endif
  GLuint textureId;
  GLsizei width, height;

  Shader(GLuint _program,
         GLuint _textureId,
         unsigned int _width,
         unsigned int _height)
      : program(_program),
        textureId(_textureId),
        width(_width),
        height(_height) {
    texY = glGetUniformLocation(program, "textureY");
    texUV = glGetUniformLocation(program, "textureUV");
    glUseProgram(program);

    glGenTextures(2, &innerTexture[0]);

    glBindTexture(GL_TEXTURE_2D, textureId);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP_HINT, GL_TRUE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, 0);
#if NV12_DEPTH_RENDERBUFFER
    glGenRenderbuffers(1, &depth_renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
#endif

    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           textureId, 0);
#if NV12_DEPTH_RENDERBUFFER
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, depth_renderbuffer);
#endif
    GLenum DrawBuffers[] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, DrawBuffers);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      switch (glCheckFramebufferStatus(GL_FRAMEBUFFER)) {
        case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
          FML_LOG(ERROR)
              << "failed to draw to framebuffer: "
                 "the framebuffer attachment points are framebuffer incomplete";
          break;
        case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
          FML_LOG(ERROR) << "failed to draw to framebuffer: "
                            "the framebuffer does not have at least one image "
                            "attached to it";
          break;
        case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS:
          FML_LOG(ERROR) << "failed to draw to framebuffer: "
                            "GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS";
          break;
        case GL_FRAMEBUFFER_UNSUPPORTED:
        default:
          FML_LOG(ERROR)
              << "failed to draw to framebuffer: target is the default "
                 "framebuffer, but the default framebuffer does not exist";
          break;
      }
      return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }

  void loadPixels(unsigned char* y_buf,
                  unsigned char* uv_buf,
                  guint y_p_s,
                  guint y_s,
                  guint uv_p_s,
                  guint uv_s) {
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
  }
};

};  // namespace nv12
