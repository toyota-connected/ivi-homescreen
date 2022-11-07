/*
 * Copyright 2022 Toyota Connected North America
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

use std::ffi::c_void;

use comp_surf_types::LoaderFunction;

#[repr(C)]
pub struct NativeWindow {
    pub display: *mut c_void,
    pub surface: *mut c_void,
    /* egl entries below are not populated if vulkan */
    pub egl_display: *mut c_void,
    pub egl_window: *mut c_void,
}

pub struct CompSurfContext {
    access_token: String,
    width: i32,
    height: i32,
    assets_path: String,
    cache_path: String,
    native_window: *mut NativeWindow,
}

impl CompSurfContext {
    pub fn new(native: *const c_void,
           width: i32,
           height: i32,
           access_token: String,
           assets_path: String,
           cache_path: String) -> CompSurfContext {

        println!("[comp_surf_rs]");

        let native_window: &mut NativeWindow = unsafe { &mut *(native as *mut NativeWindow) };

        CompSurfContext {
            width,
            height,
            access_token,
            assets_path,
            cache_path,
            native_window,
        }
    }

    pub fn version() -> u32 {
        0x00010000
    }

    pub fn dump(&self) {
        println!("Width: {}", self.width);
        println!("Height: {}", self.height);
        println!("AssetsPath: [{}]", self.assets_path);
        println!("CachePath: [{}]", self.cache_path);
        println!("AccessToken: [{}]", self.access_token);
        println!("Native Display: {:#04X?}", unsafe { (*self.native_window).display as usize });
        println!("Native Surface: {:#04X?}", unsafe { (*self.native_window).surface as usize });
        println!("Native EGL Display: {:#04X?}", unsafe { (*self.native_window).egl_display as usize });
        println!("Native EGL Window: {:#04X?}", unsafe { (*self.native_window).egl_window as usize });
    }

    pub fn load_functions(&self, loader: LoaderFunction) {
    }

    pub fn run_task(&self) {
    }

    pub fn draw_frame(&self, time: f64) {
    }

    pub fn resize(&mut self, width: i32, height: i32) {
        print!("resize: {} x {}", width, height);
        self.width = width;
        self.height = height;
    }
}