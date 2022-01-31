/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ANDROID_DRM_H_
#define ANDROID_DRM_H_

#include <cstdint>
#include <map>
#include <tuple>

#include "DrmConnector.h"
#include "DrmCrtc.h"
#include "DrmEncoder.h"
#include "DrmFbImporter.h"
#include "utils/UniqueFd.h"

namespace android {

class DrmFbImporter;
class DrmPlane;

class DrmDevice {
 public:
  DrmDevice();
  ~DrmDevice() = default;

  std::tuple<int, int> Init(const char *path, int num_displays);

  auto GetFd() const {
    return fd_.Get();
  }

  auto GetConnectors() -> const std::vector<std::unique_ptr<DrmConnector>> &;
  auto GetPlanes() -> const std::vector<std::unique_ptr<DrmPlane>> &;
  auto GetCrtcs() -> const std::vector<std::unique_ptr<DrmCrtc>> &;
  auto GetEncoders() -> const std::vector<std::unique_ptr<DrmEncoder>> &;

  auto GetMinResolution() const {
    return min_resolution_;
  }

  auto GetMaxResolution() const {
    return max_resolution_;
  }

  DrmConnector *GetConnectorForDisplay(int display) const;
  DrmCrtc *GetCrtcForDisplay(int display) const;

  std::string GetName() const;

  auto RegisterUserPropertyBlob(void *data, size_t length) const
      -> DrmModeUserPropertyBlobUnique;

  bool HandlesDisplay(int display) const;

  bool HasAddFb2ModifiersSupport() const {
    return HasAddFb2ModifiersSupport_;
  }

  auto &GetDrmFbImporter() {
    return *drm_fb_importer_;
  }

  static auto IsKMSDev(const char *path) -> bool;

  auto FindCrtcById(uint32_t id) const -> DrmCrtc * {
    for (const auto &crtc : crtcs_) {
      if (crtc->GetId() == id) {
        return crtc.get();
      }
    };

    return nullptr;
  }

  auto FindEncoderById(uint32_t id) const -> DrmEncoder * {
    for (const auto &enc : encoders_) {
      if (enc->GetId() == id) {
        return enc.get();
      }
    };

    return nullptr;
  }

  auto GetDisplayId(DrmConnector *conn) {
    return connectors_to_display_id_.at(conn);
  }

  int GetProperty(uint32_t obj_id, uint32_t obj_type, const char *prop_name,
                  DrmProperty *property) const;

 private:
  int TryEncoderForDisplay(int display, DrmEncoder *enc);

  int CreateDisplayPipe(DrmConnector *connector);

  UniqueFd fd_;

  std::vector<std::unique_ptr<DrmConnector>> connectors_;
  std::vector<std::unique_ptr<DrmConnector>> writeback_connectors_;
  std::vector<std::unique_ptr<DrmEncoder>> encoders_;
  std::vector<std::unique_ptr<DrmCrtc>> crtcs_;
  std::vector<std::unique_ptr<DrmPlane>> planes_;

  std::pair<uint32_t, uint32_t> min_resolution_;
  std::pair<uint32_t, uint32_t> max_resolution_;

  std::map<int /*display*/, DrmCrtc *> bound_crtcs_;
  std::map<int /*display*/, DrmConnector *> bound_connectors_;
  std::map<DrmConnector *, int /*display*/> connectors_to_display_id_;
  std::map<DrmEncoder *, int /*display*/> encoders_to_display_id_;
  std::map<DrmCrtc *, DrmEncoder *> bound_encoders_;

  bool HasAddFb2ModifiersSupport_{};

  std::unique_ptr<DrmFbImporter> drm_fb_importer_;
};
}  // namespace android

#endif  // ANDROID_DRM_H_
