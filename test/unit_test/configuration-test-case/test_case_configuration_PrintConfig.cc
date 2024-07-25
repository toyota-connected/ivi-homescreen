#include <filesystem>
#include <stdexcept>
#include <gtest/gtest.h>
#include <iostream>

#include <configuration/configuration.h>

/****************************************************************
Test Case Name.Test Name： HomescreenConfigurationPrintConfig_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test PrintConfig to visually check
***************************************************************/

TEST(HomescreenConfigurationPrintConfig, Lv1Normal001) {
  // set test parameters
  struct Configuration::Config config {};
  config.app_id = "homescreen";
  config.bundle_paths.emplace_back("/usr/share/");
  config.cursor_theme = "DMZ-White";
  config.disable_cursor = true;
  config.wayland_event_mask = "keyboard";
  config.debug_backend = true;
  config.view.vm_args.push_back("--enable-asserts");
  config.view.vm_args.push_back("--pause-isolates-on-start");
  config.view.bundle_path = "/home/";
  config.view.window_type = "NORMAL";
  config.view.wl_output_index = 1;
  config.view.accessibility_features = 1;
  config.view.width = 1280;
  config.view.height = 720;
  config.view.pixel_ratio = 1.2;
  config.view.ivi_surface_id = 1;
  config.view.fullscreen = true;

  std::cout << "\n##################### Reference Output #######################\n" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] {branch} @ {commit id}" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] **********" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] * Global *" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] **********" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] Application Id: .......... homescreen" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] Cursor Theme: ............ DMZ-White" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] Disable Cursor: .......... true" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] Wayland Event Mask: ...... keyboard" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] Debug Backend: ........... true" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] ********" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] * View *" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] ********" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] VM Args:" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] --enable-asserts" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] --pause-isolates-on-start" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] Bundle Path: .............. /home/" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] Window Type: .............. NORMAL" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] Output Index: ............. 1" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] Size: ..................... 1280 x 720" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] Pixel Ratio: .............. 1.2" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] Fullscreen: ............... true" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] Accessibility Features: ... 1" << std::endl;
  std::cout << "[20xx-xx-xx xx:xx:xx.xxx] [info] Ivi Surface ID: ........... 1" << std::endl;
  std::cout << "\n################## Please check visually #####################\n" << std::endl;

  Configuration::PrintConfig(config);
}
