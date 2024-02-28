#include "gtest/gtest.h"

#include <dlfcn.h>
#include <cassert>
#include <sstream>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app.h"
#include "engine.h"
#include "view/flutter_view.h"
#include "backend/headless.h"
#include "backend/osmesa_process_resolver.h"
#include "backend/gl_process_resolver.h"
#include "configuration/configuration.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/osmesa.h>
#include <GLES2/gl2.h>

/****************************************************************
Test Case Name.Test Name： HomescreenAppLoop_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test Loop without window_type
***************************************************************/
void utils_write_targa(GLubyte* buf, const char *filename, int width, int height)
{
    FILE *f = fopen( filename, "w" );
  if (f) {
    int i, x, y;
    printf ("Writing buffer to Targa file. \n");
    fputc (0x00, f);  /* ID Length, 0 => No ID  */
    fputc (0x00, f);  /* Color Map Type, 0 => No color map included */
    fputc (0x02, f);  /* Image Type, 2 => Uncompressed, True-color Image */
    fputc (0x00, f);  /* Next five bytes are about the color map entries */
    fputc (0x00, f);  /* 2 bytes Index, 2 bytes length, 1 byte size */
    fputc (0x00, f);
    fputc (0x00, f);
    fputc (0x00, f);
    fputc (0x00, f);  /* X-origin of Image  */
    fputc (0x00, f);
    fputc (0x00, f);  /* Y-origin of Image  */
    fputc (0x00, f);
    fputc (width & 0xff, f);      /* Image Width  */
    fputc ((width>>8) & 0xff, f);
    fputc (height & 0xff, f);     /* Image Height */
    fputc ((height>>8) & 0xff, f);
    fputc (0x18, f);    /* Pixel Depth, 0x18 => 24 Bits */
    fputc (0x20, f);    /* Image Descriptor */
    fclose(f);
    f = fopen( filename, "ab" );  /* reopen in binary append mode */
    for (y=height-1; y>=0; y--) {
       for (x=0; x<width; x++) {
          i = (y*width + x) * 4;
          fputc(buf[i+2], f); /* write blue */
          fputc(buf[i+1], f); /* write green */
          fputc(buf[i], f);   /* write red */
       }
    }
    fclose(f);
  }
  else {
    printf("File/Path not found\n");
  }
}

TEST(HomescreenAppHeadless, Lv1Normal001) {
  struct Configuration::Config config {};
  config.view.bundle_path = "/home/tcna/dev/workspace-automation/app/gallery/.desktop-homescreen";
  config.json_configuration_path = "/home/tcna/dev/workspace-automation/app/gallery/.desktop-homescreen/default_config.json";

  // call target function
  std::vector<struct Configuration::Config> configs =
      Configuration::ParseConfig(config);

  App app(configs);
  int ret = app.Loop();

  using namespace std::chrono_literals;
  std::this_thread::sleep_for(2s);

  // run the application until failure or queue empty
  do {
    ret = app.Loop();
  } while (ret > 0);

  utils_write_targa(app.getViewRenderBuf(0), "HomescreenAppHeadless_Lv1Normal001_Test.tga", configs[0].view.width, configs[0].view.height);

  EXPECT_EQ(0, ret);
}

