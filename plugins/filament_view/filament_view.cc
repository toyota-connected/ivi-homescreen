
#if 0  // TODO
void FilamentView::OnPlatformChannel(const FlutterPlatformMessage* message,
                                     void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  auto engine = static_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();
  auto arguments = obj->arguments();

  SPDLOG_DEBUG("[PlatformViewFilament::OnPlatformChannel]");
  SPDLOG_DEBUG("\tmethod: {}", method);
  // Utils::PrintFlutterEncodableValue("arguments", *arguments);

  if (method == kMethodChangeAnimationByIndex) {
    /// Updates the current animation by index.
    /// Returns int32_t the updated animation index.
    int32_t change_animation_by_index = 0;
    auto args = std::get_if<flutter::EncodableMap>(obj->arguments());
    for (auto& it : *args) {
      auto key = std::get<std::string>(it.first);
      if (key == kKeyChangeAnimationByIndex &&
          std::holds_alternative<int32_t>(it.second)) {
        change_animation_by_index = std::get<int32_t>(it.second);
      }
    }
    spdlog::debug("{}: {}", kMethodChangeAnimationByIndex,
                  change_animation_by_index);

    auto res = flutter::EncodableValue(change_animation_by_index);
    result = codec.EncodeSuccessEnvelope(&res);

  } else if (method == kMethodChangeAnimationByName) {
    /// param - args
    /// kKeyChangeAnimationByName: String? animationName
    /// Updates the current animation by animation name.
    /// Returns int the updated animation index.
    std::string change_animation_by_name;
    auto args = std::get_if<flutter::EncodableMap>(obj->arguments());
    for (auto& it : *args) {
      auto key = std::get<std::string>(it.first);
      if (key == kKeyChangeAnimationByName &&
          std::holds_alternative<std::string>(it.second)) {
        change_animation_by_name = std::get<std::string>(it.second);
      }
    }
    spdlog::debug("{}: {}", kMethodChangeAnimationByName,
                  change_animation_by_name);

    int32_t animation_index = 0;
    auto res = flutter::EncodableValue(animation_index);
    result = codec.EncodeSuccessEnvelope(&res);

  } else if (method == kMethodGetAnimationNames) {
    /// param - none
    /// Get current model animation names.
    /// Returns String List<Object?> that it's succeeded.
    spdlog::debug("{}: TODO", kMethodGetAnimationNames);

  } else if (method == kMethodGetAnimationCount) {
    /// param - none
    /// Get current model animation count.
    /// Returns int
    spdlog::debug("{}: TODO", kMethodGetAnimationCount);

    int32_t animation_count = 1;
    auto res = flutter::EncodableValue(animation_count);
    result = codec.EncodeSuccessEnvelope(&res);

  } else if (method == kMethodGetCurrentAnimationIndex) {
    /// Get current animation index.
    /// Returns int
    spdlog::debug("{}: TODO", kMethodGetCurrentAnimationIndex);

    int32_t current_animation_index = 1;
    auto res = flutter::EncodableValue(current_animation_index);
    result = codec.EncodeSuccessEnvelope(&res);

  } else if (method == kMethodGetAnimationNameByIndex) {
    /// param - args
    /// kKeyGetAnimationNameByIndex: int? index
    /// Get Animation name by given index.
    /// Returns String
    spdlog::debug("{}: TODO", kMethodGetAnimationNameByIndex);

  } else if (method == kMethodChangeSkyboxByAsset) {
    /// param - args
    /// kKeyChangeSkyboxByAsset: String? path
    /// Updates skybox by ktx file from the assets.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeSkyboxByAsset);

  } else if (method == kMethodChangeSkyboxByUrl) {
    /// param - args
    /// kKeyChangeSkyboxByUrl: String? url
    /// Updates skybox by ktx file from [url].
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeSkyboxByUrl);

  } else if (method == kMethodChangeSkyboxByHdrAsset) {
    /// param - args
    /// kKeyChangeSkyboxByHdrAsset: String? path
    /// Updates skybox by hdr file from the assets.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeSkyboxByHdrAsset);

  } else if (method == kMethodChangeSkyboxByHdrUrl) {
    /// param - args
    /// kKeyChangeSkyboxByHdrUrl: String? url
    /// Updates skybox by hdr file from [url].
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeSkyboxByHdrUrl);

  } else if (method == kMethodChangeSkyboxColor) {
    /// param - args
    /// kKeyChangeSkyboxColor: environmentColor (ascii base 16 hex)
    /// Updates skybox by given color.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeSkyboxColor);

  } else if (method == kMethodChangeToTransparentSkybox) {
    /// param - none
    /// Updates skybox to be transparent.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeToTransparentSkybox);

  } else if (method == kMethodChangeLightByKtxAsset) {
    /// param - args
    /// kKeyChangeLightByKtxAsset: String? path,
    /// kKeyChangeLightByKtxAssetIntensity: double? intensity
    /// Updates scene indirect light by ktx file from assets.
    /// also update scene indirect light [intensity] if provided.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeLightByKtxAsset);

  } else if (method == kMethodChangeLightByKtxUrl) {
    /// param - args
    /// kKeyChangeLightByKtxUrl: String? url,
    /// kKeyChangeLightByKtxUrlIntensity: double? intensity
    /// Updates scene indirect light by ktx file from url.
    /// also update scene indirect light [intensity] if provided.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeLightByKtxUrl);

  } else if (method == kMethodChangeLightByIndirectLight) {
    /// param - args
    /// kKeyChangeLightByHdrAsset: String? path
    /// kKeyChangeLightByHdrAssetIntensity: double? intensity
    /// Updates scene indirect light by HDR file from assets.
    /// also update scene indirect light [intensity] if provided.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeLightByIndirectLight);

  } else if (method == kMethodChangeLightByHdrUrl) {
    /// Updates scene indirect light by HDR file from assets.
    /// also update scene indirect light [intensity] if provided.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeLightByHdrUrl);

  } else if (method == kMethodChangeToDefaultIndirectLight) {
    /// Updates scene indirect light to [DefaultIndirectLight] with default
    /// values of :
    /// * intensity which is 40_000.0.
    /// * radiance bands of 1.0,
    /// * radiance sh of [1,1,1]
    /// * irradiance bands of 1.0,
    /// * irradiance sh of [1,1,1]
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeToDefaultIndirectLight);

  } else if (method == kMethodChangeLight) {
    /// Updates scene direct light .
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeLight);

  } else if (method == kMethodChangeToDefaultLight) {
    /// Updates scene direct light To the default value.
    /// With intensity value of 100_000.0.
    /// and color temperature bands of 6_500.0,
    /// and direction of [0.0, -1.0, 0.0],
    /// and cast shadows true.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeToDefaultLight);

  } else if (method == kMethodLoadGlbModelFromAssets) {
    /// Load glb model from assets and replace current model with it.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodLoadGlbModelFromAssets);

  } else if (method == kMethodLoadGlbModelFromUrl) {
    /// Load glb model from url and replace current model with it.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodLoadGlbModelFromUrl);

  } else if (method == kMethodLoadGltfModelFromAssets) {
    /// Load gltf model from assets and replace current model with it.
    /// If the images path that in the gltf file different from the flutter
    /// asset path, consider adding [prefix] to the images path to be before the
    /// image. Or [postfix] to the images path to be after the image. For
    /// example if in the gltf file, the image path is textures/texture and in
    /// assets the image path is assets/models/textures/texture.png [prefix]
    /// must be 'assets/models/' and [postfix] should be '.png'. Returns String
    /// message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodLoadGltfModelFromAssets);

  } else if (method == kMethodChangeModelScale) {
    /// param - double? scale
    /// Updates current model scale.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeModelScale);

  } else if (method == kMethodChangeModelPosition) {
    /// param - Position
    /// Updates current model center position in the world space.
    /// By taking x,y,z coordinates of [centerPosition] in the world space.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeModelPosition);

  } else if (method == kMethodGetCurrentModelState) {
    /// param - none
    /// Get current model state.
    /// Return String (serialized ModelState)
    spdlog::debug("{}: TODO", kMethodGetCurrentModelState);

  } else if (method == kMethodUpdateCamera) {
    /// param - Camera? camera - flutter::EncodableMap
    /// Replace current camera with a new [camera].
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodUpdateCamera);

  } else if (method == kMethodUpdateExposure) {
    /// param - Exposure? exposure - flutter::EncodableMap
    /// Update the current camera exposure.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodUpdateExposure);

  } else if (method == kMethodUpdateProjection) {
    /// param - Projection? projection - flutter::EncodableMap
    /// Update the current camera projection.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodUpdateProjection);

  } else if (method == kMethodUpdateLensProjection) {
    /// param - LensProjection? lensProjection - flutter::EncodableMap
    /// Update the current camera lens projection.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodUpdateLensProjection);

  } else if (method == kMethodUpdateCameraShift) {
    /// param - arg
    /// kKeyUpdateCameraShift: List<double>? shift
    /// Update the current camera shifting.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodUpdateCameraShift);

  } else if (method == kMethodUpdateCameraScaling) {
    /// param - arg
    /// kKeyUpdateCameraScaling: List<double>? scaling
    /// Update the current camera scaling.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodUpdateCameraScaling);

  } else if (method == kMethodSetDefaultCamera) {
    /// param - none
    /// Update the current camera to it's default settings.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodSetDefaultCamera);

  } else if (method == kMethodLookAtDefaultPosition) {
    /// param - none
    /// Makes the camera looks at its default position.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodLookAtDefaultPosition);

  } else if (method == kMethodGetLookAt) {
    /// param - none
    /// Gets the current positions the camera is looking at.
    /// Returns List of 9 elements. List<Object?>?
    /// From [0:2] elements are eye position.
    /// From [3:5] elements are target position.
    /// From [6:8]  elements are upward position
    spdlog::debug("{}: TODO", kMethodGetLookAt);

  } else if (method == kMethodLookAtPosition) {
    /// param - args
    /// kKeyEyeArray: List<double>? eyePos
    /// kKeyTargetArrayKey: List<double>? targetPos
    /// kKeyUpwardArrayKey: List<double>? upwardPos
    /// Sets the camera's model matrix.
    ///
    ///[eyePos] consists of 3 elements :
    /// eyeX – x-axis position of the camera in world space
    /// eyeY – y-axis position of the camera in world space
    /// eyeZ – z-axis position of the camera in world space
    ///
    ///[targetPos] consists of 3 elements :
    /// centerX – x-axis position of the point in world space the camera is
    /// looking at centerY – y-axis position of the point in world space the
    /// camera is looking at centerZ – z-axis position of the point in world
    /// space the camera is looking at
    ///
    ///[upwardPos] consists of 3 elements :
    /// upX – x-axis coordinate of a unit vector denoting the camera's "up"
    /// direction upY – y-axis coordinate of a unit vector denoting the camera's
    /// "up" direction upZ – z-axis coordinate of a unit vector denoting the
    /// camera's "up" direction Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodLookAtPosition);

  } else if (method == kMethodCameraScroll) {
    /// param - args
    /// kKeyCameraScrollX: num? x
    /// kKeyCameraScrollYKey: num? y
    /// kKeyCameraScrollDeltaKey: double? scrollDelta
    /// In MAP and ORBIT modes, dollys the camera along the viewing direction.
    /// In FREE_FLIGHT mode, adjusts the move speed of the camera.
    ///
    /// [x] X-coordinate for point of interest in viewport space, ignored in
    /// FREE_FLIGHT mode [y] Y-coordinate for point of interest in viewport
    /// space, ignored in FREE_FLIGHT mode [scrolldelta] In MAP and ORBIT modes,
    /// negative means "zoom in", positive means "zoom out"
    ///  In FREE_FLIGHT mode, negative means "slower", positive means "faster"
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodCameraScroll);

  } else if (method == kMethodCameraGrabBegin) {
    /// param - args
    /// kKeyCameraGrabBeginX: num? x
    /// kKeyCameraGrabBeginY: num? y
    /// Updates a grabbing session. This must be called at least once between
    /// [beginCameraGrab] / [endCameraGrab] to dirty the camera.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodCameraGrabBegin);

  } else if (method == kMethodCameraGrabUpdate) {
    /// param - args
    /// kKeyCameraGrabUpdateX: num? x,
    /// kKeyCameraGrabUpdateY: num? y,
    /// kKeyCameraGrabBeginStrafeKey: bool? strafe
    /// Starts a grabbing session (i.e. the user is dragging around in the
    /// viewport).
    /// In MAP mode, this starts a panning session. In ORBIT mode, this starts
    /// either rotating or strafing. In FREE_FLIGHT mode, this starts a nodal
    /// panning session. [x] – X-coordinate for point of interest in viewport
    /// space. [y] – Y-coordinate for point of interest in viewport space.
    /// [strafe] – ORBIT mode only; if true, starts a translation rather than a
    /// rotation. Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodCameraGrabUpdate);

  } else if (method == kMethodCameraGrabEnd) {
    /// param - none
    /// Ends a grabbing session.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodCameraGrabEnd);

  } else if (method == kMethodCameraRayCast) {
    /// param - none
    /// kKeyCameraRayCastX: num? x
    /// kKeyCameraRayCastY: num? y
    /// Given a viewport coordinate, picks a point in the ground plane.
    /// returns List<double>?
    spdlog::debug("{}: TODO", kMethodCameraRayCast);

  } else if (method == kMethodUpdateGround) {
    /// param - none
    /// kKeyUpdateGround: Ground? ground - EncodableMap
    /// Updates current Ground by updating it's width, height, and
    /// whether is below model or not without updating material.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodCameraRayCast);

  } else if (method == kMethodUpdateGroundMaterial) {
    /// param - none
    /// kKeyUpdateGroundMaterial: Material material - EncodableMap
    /// Updates current Ground material.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodUpdateGroundMaterial);

  } else if (method == kMethodAddShape) {
    /// param - none
    /// kKeyAddShape: Shape shape - EncodableMap
    /// Add a shape to the scene.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodAddShape);

  } else if (method == kMethodRemoveShape) {
    /// param - none
    /// kKeyRemoveShape: int id
    /// Removes a shape from the scene by given [id].
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodRemoveShape);

  } else if (method == kMethodUpdateShape) {
    /// param - args
    /// kKeyUpdateShape: Shape shape - EncodableMap
    /// kKeyUpdateShapeId: int id - int32
    /// Updates a shape with given [id] with new [shape].
    /// if no shape was found with the given [id], new shape will be created.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodUpdateShape);

  } else if (method == kMethodGetCurrentCreatedShapesIds) {
    /// param - args
    /// Gets current created shapes ids.
    /// Returns List<int>
    spdlog::debug("{}: TODO", kMethodGetCurrentCreatedShapesIds);

  } else {
    spdlog::error(
        "[PlatformViewFilament::OnPlatformChannel] method {} is unhandled",
        method);
    Utils::PrintFlutterEncodableValue("unhandled", *arguments);
    result = codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  }
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
#endif

#if 0
void FilamentView::OnModelState(const FlutterPlatformMessage* message,
                                void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  const auto engine = static_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  const auto obj =
      codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();
  const auto arguments = obj->arguments();

  SPDLOG_DEBUG("[FilamentView::OnModelState]");
  SPDLOG_DEBUG("\tmethod: {}", method);
  Utils::PrintFlutterEncodableValue("arguments", *arguments);

  if (method == kKeyListen) {
    result = codec.EncodeSuccessEnvelope();
  } else {
    spdlog::error("[FilamentView::OnModelState] method {} is unhandled",
                  method);
    Utils::PrintFlutterEncodableValue("unhandled", *arguments);
    result = codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  }
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}

void FilamentView::OnSceneState(const FlutterPlatformMessage* message,
                                void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  const auto engine = static_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  const auto obj =
      codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();
  const auto arguments = obj->arguments();

  SPDLOG_DEBUG("[FilamentView::OnSceneState]");
  SPDLOG_DEBUG("\tmethod: {}", method);
  Utils::PrintFlutterEncodableValue("arguments", *arguments);

  if (method == kKeyListen) {
    result = codec.EncodeSuccessEnvelope();
  } else {
    spdlog::error("[FilamentView::OnSceneState] method {} is unhandled",
                  method);
    Utils::PrintFlutterEncodableValue("unhandled", *arguments);
    result = codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  }
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}

void FilamentView::OnShapeState(const FlutterPlatformMessage* message,
                                void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  const auto engine = static_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  const auto obj =
      codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();
  const auto arguments = obj->arguments();

  SPDLOG_DEBUG("[FilamentView::OnShapeState]");
  SPDLOG_DEBUG("\tmethod: {}", method);
  Utils::PrintFlutterEncodableValue("arguments", *arguments);

  if (method == kKeyListen) {
    result = codec.EncodeSuccessEnvelope();
  } else {
    spdlog::error("[FilamentView::OnShapeState] method {} is unhandled",
                  method);
    Utils::PrintFlutterEncodableValue("unhandled", *arguments);
    result = codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  }
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
