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


TEST(HomescreenGlProcessResolver_Headless, Lv1Normal001) {
  // static const int MAX_DEVICES = 4;
  // EGLDeviceEXT eglDevs[MAX_DEVICES];
  // EGLint numDevices;

  auto gl_process = GlProcessResolver_Headless::GetInstance();
  PFNGLGETSTRINGPROC process_resolver = (PFNGLGETSTRINGPROC)gl_process.process_resolver("glGetString");
  // auto gl_process = GlProcessResolver::GetInstance();
  // PFNGLGETSTRINGPROC process_resolver = (PFNGLGETSTRINGPROC)gl_process.process_resolver("glGetString");

  // PFNEGLQUERYDEVICESEXTPROC eglQueryDevicesEXT =
  //     (PFNEGLQUERYDEVICESEXTPROC)gl_process.process_resolver("eglQueryDevicesEXT");

  // eglQueryDevicesEXT(MAX_DEVICES, eglDevs, &numDevices);

  // printf("Detected %d devices\n", numDevices);

  printf("\nGL_VERSION: %s\n\n", process_resolver(GL_VERSION));
  EXPECT_TRUE(process_resolver != NULL);
}

TEST(HomescreenAppHeadless, Lv1Normal001) {
  struct Configuration::Config config {};
  config.view.bundle_path = "/home/tcna/dev/workspace-automation/app/gallery/.desktop-homescreen";

  // call target function
  std::vector<struct Configuration::Config> configs =
      Configuration::ParseConfig(config);

  App app(configs);
  int ret = app.Loop();

  using namespace std::chrono_literals;
  std::this_thread::sleep_for(30s);

  auto headlessBackend = reinterpret_cast<HeadlessBackend*>(app.GetFlutterView(0)->GetBackend());
  utils_write_targa(headlessBackend->getHeadlessBuffer(), "testimage.tga", 800, 800);

  // ret value is set at mock Display::PollEvents()
  EXPECT_EQ(1, ret);
}

/****************************************************************
Test Case Name.Test Name： HomescreenAppLoop_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test Loop with window_type BG
***************************************************************/

// TEST(HomescreenAppLoop, Lv1Normal002) {
//   struct Configuration::Config config {};
//   config.view.bundle_path = "/home/root/";
//   config.view.window_type = "BG";

//   // call target function
//   std::vector<struct Configuration::Config> configs =
//       Configuration::ParseConfig(config);

//   App app(configs);
//   int ret = app.Loop();

//   // ret value is set at mock Display::PollEvents()
//   EXPECT_EQ(1, ret);
// }
