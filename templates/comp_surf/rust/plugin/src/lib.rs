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

use std::ffi::{CStr, c_void, c_int, c_char};

use comp_surf_context::CompSurfContext;

#[no_mangle]
pub extern "C" fn comp_surf_version() -> u32 {
    CompSurfContext::version()
}

pub type LoaderFunction = Option<
    unsafe extern "C" fn(
        arg1: *mut c_void,
        arg2: *const c_char,
    ) -> *const c_void,
>;

#[no_mangle]
pub extern "C" fn comp_surf_load_functions(ctx: *mut CompSurfContext, loader: LoaderFunction) {
    if ctx.is_null() {
        return;
    }

    let context = {
        assert!(!ctx.is_null());
        unsafe { &mut *ctx }
    };

    context.load_functions(loader);
}

#[no_mangle]
pub extern "C" fn comp_surf_initialize(
        access_token: *const c_char,
        width: c_int,
        height: c_int,
        native_window: *mut c_void,
        assets_path: *const c_char,
        cache_path: *const c_char,
    ) -> *mut CompSurfContext {

    let access_token_cstr = unsafe { CStr::from_ptr(access_token )};
    let assets_path_cstr = unsafe { CStr::from_ptr(assets_path )};
    let cache_path_cstr = unsafe { CStr::from_ptr(cache_path )};

    let context = CompSurfContext::new(
        native_window,
        width,
        height,
        String::from_utf8_lossy(access_token_cstr.to_bytes()).to_string(),
        String::from_utf8_lossy(assets_path_cstr.to_bytes()).to_string(),
        String::from_utf8_lossy(cache_path_cstr.to_bytes()).to_string()
    );

    context.dump();

    Box::into_raw(Box::new(context))
}

#[no_mangle]
pub extern "C" fn comp_surf_de_initialize(ctx: *mut CompSurfContext) {
    if ctx.is_null() {
        return;
    }
    print!("comp_surf_de_initialize: {:?}", ctx);
    let _ = unsafe { Box::from_raw(ctx) };
}

#[no_mangle]
pub extern "C" fn comp_surf_run_task(ctx: *mut CompSurfContext) {
    if ctx.is_null() {
        return;
    }

    let context = {
        assert!(!ctx.is_null());
        unsafe { &mut *ctx }
    };

    context.run_task();
}

#[no_mangle]
pub extern "C" fn comp_surf_draw_frame(ctx: *mut CompSurfContext, time: f64) {
    if ctx.is_null() {
        return;
    }

    let context = {
        assert!(!ctx.is_null());
        unsafe { &mut *ctx }
    };

    context.draw_frame(time);
}

#[no_mangle]
pub extern "C" fn comp_surf_resize(
    ctx: *mut CompSurfContext,
    width: c_int,
    height: c_int,
    ) {
    if ctx.is_null() {
        return;
    }

    let context = {
        assert!(!ctx.is_null());
        unsafe { &mut *ctx }
    };

    context.resize(width, height);
}
