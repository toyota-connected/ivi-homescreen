#include "gtest/gtest.h"
#include "utils.h"
#include "engine.h"
#include "view/flutter_view.h"
#include "wayland/display.h"
#include "textures/texture.h"

static constexpr char kSourceRoot[] = SOURCE_ROOT_DIR;
constexpr int64_t kTestTextureObjectId = 5150;
const std::string kCallCreateCb = "Call Create Callback";
const flutter::EncodableValue kFlutterTestKey = flutter::EncodableValue("Test Key");
const flutter::EncodableValue kFlutterTestVal = flutter::EncodableValue("Test Val");
bool callDisposeCallback = false;

/**
 * @brief Get FlutterView instance
 */
FlutterView* createFlutterViewInstance() {
  // setup parameter
  struct Configuration::Config config {};
  config.view.bundle_path = "/home/tcna/dev/workspace-automation/app/gallery/.desktop-homescreen";
  config.view.wl_output_index = 1;
  auto configs = Configuration::ParseConfig(config);

  auto wayland_display = std::make_shared<Display>(false, "", "", configs);
  auto* view = new FlutterView(config, 0, wayland_display);
  return view;
}

/**
 * @brief Get Engine instance
 */
Engine* createEngineInstance() {
  // setup parameter
  FlutterView* view = createFlutterViewInstance();
  std::vector<const char*> vm_args_c;

  Engine *engine = new Engine(view, 1, vm_args_c, "/home/tcna/dev/workspace-automation/app/gallery/.desktop-homescreen", 1);
  return engine;
}

/**
 * @brief Callback function to registered instead of Texture::Create
 */
flutter::EncodableValue create_callback(void* userdata,
    const flutter::EncodableMap* args) {
  // check object Texture-ID
  auto* obj = reinterpret_cast<Texture*>(userdata);
  int64_t texture_id = obj->GetId();
  EXPECT_EQ(kTestTextureObjectId, texture_id);

  // check args key & value
  EXPECT_TRUE(args->at(kFlutterTestKey) == kFlutterTestVal);

  return flutter::EncodableValue(kCallCreateCb);
}

/**
 * @brief Callback function to registered instead of Texture::Dispose
 */
void dispose_callback(void* userdata, GLuint name) {
  // check object Texture-ID
  auto* obj = reinterpret_cast<Texture*>(userdata);
  int64_t texture_id = obj->GetId();
  EXPECT_EQ(kTestTextureObjectId, texture_id);

  // check name parameter
  EXPECT_TRUE(kTestTextureObjectId == name);

  callDisposeCallback = true;
}

/****************************************************************
Test Case Name.Test Name： HomescreenTextureGetFlutterOpenGLTexture_Lv1Normal001
Use Case Name: Set OpenGL texture
Test Summary: Check that the value set in the constructor can be retrieved
***************************************************************/
TEST(HomescreenTextureGetFlutterOpenGLTexture, Lv1Normal001) {
  // setup parameter
  Texture *texture = new Texture(kTestTextureObjectId, GL_TEXTURE_2D,
      GL_RGBA8, nullptr, nullptr);
  ASSERT_TRUE(texture != nullptr);

  // call target function
  FlutterOpenGLTexture texture_out;
  texture->GetFlutterOpenGLTexture(&texture_out);

  // confirm to create instance
  EXPECT_EQ(GL_TEXTURE_2D, texture_out.target);
  EXPECT_EQ(GL_RGBA8, texture_out.format);
}

/****************************************************************
Test Case Name.Test Name： HomescreenTextureCreate_Lv1Normal001
Use Case Name: Set OpenGL texture
Test Summary: Dont set callback and check default encodable value
***************************************************************/
TEST(HomescreenTextureCreate, Lv1Normal001) {
  // setup parameter
  Texture *texture = new Texture(kTestTextureObjectId, GL_TEXTURE_2D,
      GL_RGBA8, nullptr, nullptr);
  ASSERT_TRUE(texture != nullptr);

  // call target function
  flutter::EncodableValue create = texture->Create(100, 200, nullptr);

  // confirm to create instance
  flutter::EncodableValue expect_val = flutter::EncodableValue(
    flutter::EncodableMap{
      {flutter::EncodableValue("result"),
       flutter::EncodableValue(-1)},
      {flutter::EncodableValue("error"),
       flutter::EncodableValue("Create callback not set")}});
  EXPECT_TRUE(expect_val == create);
}

/****************************************************************
Test Case Name.Test Name： HomescreenTextureCreate_Lv1Normal002
Use Case Name: Set OpenGL texture
Test Summary: set create callback and check if the callback is called
***************************************************************/
TEST(HomescreenTextureCreate, Lv1Normal002) {
  // setup parameter
  Texture *texture = new Texture(kTestTextureObjectId, GL_TEXTURE_2D,
      GL_RGBA8, create_callback, nullptr);
  ASSERT_TRUE(texture != nullptr);

  flutter::EncodableMap test_args = flutter::EncodableMap{
        {kFlutterTestKey, kFlutterTestVal}};

  // call target function
  flutter::EncodableValue create = texture->Create(100, 200, &test_args);

  // confirm to call create_callback function
  flutter::EncodableValue expect_val = flutter::EncodableValue(kCallCreateCb);
  EXPECT_TRUE(expect_val == create);
}

/****************************************************************
Test Case Name.Test Name： HomescreenTextureDispose_Lv1Normal001
Use Case Name: Set OpenGL texture
Test Summary: set dispose callback and check if the callback is called
***************************************************************/
TEST(HomescreenTextureDispose, Lv1Normal001) {
  // setup parameter
  Texture *texture = new Texture(kTestTextureObjectId, GL_TEXTURE_2D,
      GL_RGBA8, nullptr, dispose_callback);
  ASSERT_TRUE(texture != nullptr);
  callDisposeCallback = false;

  flutter::EncodableMap test_args = flutter::EncodableMap{
        {kFlutterTestKey, kFlutterTestVal}};

  // call target function
  texture->Dispose(kTestTextureObjectId);

  // confirm to call create_callback function
  EXPECT_TRUE(callDisposeCallback);
}

/****************************************************************
Test Case Name.Test Name： HomescreenTextureDispose_Lv1Normal002
Use Case Name: Set OpenGL texture
Test Summary: Dont set dispose callback and check if the callback is not called
***************************************************************/
TEST(HomescreenTextureDispose, Lv1Normal002) {
  // setup parameter
  Texture *texture = new Texture(kTestTextureObjectId, GL_TEXTURE_2D,
      GL_RGBA8, nullptr, nullptr);
  ASSERT_TRUE(texture != nullptr);
  callDisposeCallback = false;

  flutter::EncodableMap test_args = flutter::EncodableMap{
        {kFlutterTestKey, kFlutterTestVal}};

  // call target function
  texture->Dispose(kTestTextureObjectId);

  // confirm to call create_callback function
  EXPECT_FALSE(callDisposeCallback);
}


#if 0
/****************************************************************
Test Case Name.Test Name： HomescreenTextureEnable_Lv1Normal001
Use Case Name: Set OpenGL texture
Test Summary: Check the texture ID is set
***************************************************************/
TEST(HomescreenTextureEnable, Lv1Normal001) {
  // setup parameter
  Texture *texture = new Texture(kTestTextureObjectId, GL_TEXTURE_2D,
      GL_RGBA8, nullptr, nullptr);
  ASSERT_TRUE(texture != nullptr);

  Engine* engine = createEngineInstance();
  texture->SetEngine(engine);

  // call target function
  texture->Enable(kTestTextureObjectId);

  // confirm to create instance
  EXPECT_EQ(texture->m_enabled, true);

  // confirm to add name
  auto it = std::find(texture->m_name.begin(), texture->m_name.end(), kTestTextureObjectId);
  EXPECT_TRUE(texture->m_name.end() != it);
}

/****************************************************************
Test Case Name.Test Name： HomescreenTextureDisable_Lv1Normal001
Use Case Name: Set OpenGL texture
Test Summary: Check the texture ID is deleted
***************************************************************/
TEST(HomescreenTextureDisable, Lv1Normal001) {
  // setup parameter
  Texture *texture = new Texture(kTestTextureObjectId, GL_TEXTURE_2D,
      GL_RGBA8, nullptr, nullptr);
  ASSERT_TRUE(texture != nullptr);

  Engine* engine = createEngineInstance();
  texture->SetEngine(engine);
  texture->Enable(kTestTextureObjectId);

  // call target function
  texture->Disable(kTestTextureObjectId);

  // confirm to create instance and add name
  EXPECT_EQ(texture->m_enabled, false);

  auto it = std::find(texture->m_name.begin(), texture->m_name.end(),
      kTestTextureObjectId);
  EXPECT_TRUE(texture->m_name.end() == it);
}
#endif
