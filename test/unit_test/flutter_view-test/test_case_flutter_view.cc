#include <stdexcept>
#include <stdlib.h>
#include "gtest/gtest.h"
#include "utils.h"
#include "wayland/display.h"
#include "view/flutter_view.h"

FlutterView* createFlutterViewInstance() {
    // setup paraeter
    struct Configuration::Config config {};
    config.view.bundle_path = "/home/root/";
    config.view.wl_output_index = 1;
    std::vector<Configuration::Config> configs = Configuration::ParseConfig(config);

    // use mock of Display class
    std::shared_ptr<Display> wayland_display = std::make_shared<Display>(false, "", "", configs);
    FlutterView* view = new FlutterView(config, 0, wayland_display);
    return view;
}

/****************************************************************
Test Case Name.Test Name： HomescreenFlutterViewConstructor_Lv1Normal001
Use Case Name: Provide wayland client function
Test Summary：Test the constructor of FlutterView class
***************************************************************/
TEST(HomescreenFlutterViewConstructor, Lv1Normal001) {
    // call target function
    FlutterView* view = createFlutterViewInstance();

    // confirm to create instance
    EXPECT_TRUE(view != NULL);
}

/****************************************************************
Test Case Name.Test Name： HomescreenFlutterViewCreateSurface_Lv1Normal001
Use Case Name: Provide wayland client function
Test Summary：Test the function of CreateSurface
***************************************************************/
TEST(HomescreenFlutterViewCreateSurface, Lv1Normalcase001) {
    FlutterView* view = createFlutterViewInstance();

    // call target function
    int h_module = 0;
    size_t surf_index = view->CreateSurface(
        &h_module, "", "", "",
        CompositorSurface::PARAM_SURFACE_T::egl,
        CompositorSurface::PARAM_Z_ORDER_T::above,
        CompositorSurface::PARAM_SYNC_T::sync,
        kDefaultViewWidth,
        kDefaultViewHeight,
        0,
        0
    );

    // check comp_surf array index and the array is not empty
    EXPECT_EQ(0, surf_index);
    EXPECT_FALSE(view->m_comp_surf.empty());
}


/****************************************************************
Test Case Name.Test Name： HomescreenFlutterViewDisposeSurface_Lv1Normal001
Use Case Name: Provide wayland client function
Test Summary：Test the function of DisposeSurface
***************************************************************/
TEST(HomescreenFlutterViewDisposeSurface, Lv1Normalcase001) {
    FlutterView* view = createFlutterViewInstance();

    int h_module = 0;
    view->CreateSurface(
        &h_module, "", "", "",
        CompositorSurface::PARAM_SURFACE_T::egl,
        CompositorSurface::PARAM_Z_ORDER_T::above,
        CompositorSurface::PARAM_SYNC_T::sync,
        kDefaultViewWidth,
        kDefaultViewHeight,
        0,
        0
    );

    // check comp_surf array is not empty
    EXPECT_FALSE(view->m_comp_surf.empty());

    // call target function
    view->DisposeSurface(0);

    // check comp_surf array content was deleted
    EXPECT_TRUE(view->m_comp_surf.empty());
}