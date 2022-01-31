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

#define LOG_TAG "hwc-drm-device"

#include "DrmDevice.h"

#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <sstream>
#include <string>

#include "drm/DrmPlane.h"
#include "utils/log.h"
#include "utils/properties.h"

namespace android {

DrmDevice::DrmDevice() {
  drm_fb_importer_ = std::make_unique<DrmFbImporter>(*this);
}

// NOLINTNEXTLINE (readability-function-cognitive-complexity): Fixme
std::tuple<int, int> DrmDevice::Init(const char *path, int num_displays) {
  /* TODO: Use drmOpenControl here instead */
  fd_ = UniqueFd(open(path, O_RDWR | O_CLOEXEC));
  if (!fd_) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): Fixme
    ALOGE("Failed to open dri %s: %s", path, strerror(errno));
    return std::make_tuple(-ENODEV, 0);
  }

  int ret = drmSetClientCap(GetFd(), DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  if (ret != 0) {
    ALOGE("Failed to set universal plane cap %d", ret);
    return std::make_tuple(ret, 0);
  }

  ret = drmSetClientCap(GetFd(), DRM_CLIENT_CAP_ATOMIC, 1);
  if (ret != 0) {
    ALOGE("Failed to set atomic cap %d", ret);
    return std::make_tuple(ret, 0);
  }

#ifdef DRM_CLIENT_CAP_WRITEBACK_CONNECTORS
  ret = drmSetClientCap(GetFd(), DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1);
  if (ret != 0) {
    ALOGI("Failed to set writeback cap %d", ret);
    ret = 0;
  }
#endif

  uint64_t cap_value = 0;
  if (drmGetCap(GetFd(), DRM_CAP_ADDFB2_MODIFIERS, &cap_value) != 0) {
    ALOGW("drmGetCap failed. Fallback to no modifier support.");
    cap_value = 0;
  }
  HasAddFb2ModifiersSupport_ = cap_value != 0;

  drmSetMaster(GetFd());
  if (drmIsMaster(GetFd()) == 0) {
    ALOGE("DRM/KMS master access required");
    return std::make_tuple(-EACCES, 0);
  }

  auto res = MakeDrmModeResUnique(GetFd());
  if (!res) {
    ALOGE("Failed to get DrmDevice resources");
    return std::make_tuple(-ENODEV, 0);
  }

  min_resolution_ = std::pair<uint32_t, uint32_t>(res->min_width,
                                                  res->min_height);
  max_resolution_ = std::pair<uint32_t, uint32_t>(res->max_width,
                                                  res->max_height);

  for (int i = 0; i < res->count_crtcs; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto crtc = DrmCrtc::CreateInstance(*this, res->crtcs[i], i);
    if (crtc) {
      crtcs_.emplace_back(std::move(crtc));
    }
  }

  for (int i = 0; i < res->count_encoders; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto enc = DrmEncoder::CreateInstance(*this, res->encoders[i], i);
    if (enc) {
      encoders_.emplace_back(std::move(enc));
    }
  }

  for (int i = 0; i < res->count_connectors; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto conn = DrmConnector::CreateInstance(*this, res->connectors[i], i);

    if (!conn) {
      continue;
    }

    if (conn->IsWriteback()) {
      writeback_connectors_.emplace_back(std::move(conn));
    } else {
      connectors_.emplace_back(std::move(conn));
    }
  }

  auto add_displays = [this, &num_displays](bool internal, bool connected) {
    for (auto &conn : connectors_) {
      bool is_connected = conn->IsConnected();
      if ((internal ? conn->IsInternal() : conn->IsExternal()) &&
          (connected ? is_connected : !is_connected)) {
        bound_connectors_[num_displays] = conn.get();
        connectors_to_display_id_[conn.get()] = num_displays;
        ++num_displays;
      }
    }
  };

  /* Put internal first to ensure Primary display will be internal
   * in case at least 1 internal is available
   */
  add_displays(/*internal = */ true, /*connected = */ true);
  add_displays(/*internal = */ false, /*connected = */ true);
  add_displays(/*internal = */ true, /*connected = */ false);
  add_displays(/*internal = */ false, /*connected = */ false);

  // Catch-all for the above loops
  if (ret != 0)
    return std::make_tuple(ret, 0);

  auto plane_res = MakeDrmModePlaneResUnique(GetFd());
  if (!plane_res) {
    ALOGE("Failed to get plane resources");
    return std::make_tuple(-ENOENT, 0);
  }

  for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto plane = DrmPlane::CreateInstance(*this, plane_res->planes[i]);

    if (plane) {
      planes_.emplace_back(std::move(plane));
    }
  }

  for (auto &conn : connectors_) {
    ret = CreateDisplayPipe(conn.get());
    if (ret != 0) {
      ALOGE("Failed CreateDisplayPipe %d with %d", conn->GetId(), ret);
      return std::make_tuple(ret, 0);
    }
  }
  return std::make_tuple(ret, bound_connectors_.size());
}

bool DrmDevice::HandlesDisplay(int display) const {
  return bound_connectors_.count(display) != 0;
}

DrmConnector *DrmDevice::GetConnectorForDisplay(int display) const {
  return bound_connectors_.at(display);
}

DrmCrtc *DrmDevice::GetCrtcForDisplay(int display) const {
  return bound_crtcs_.at(display);
}

int DrmDevice::TryEncoderForDisplay(int display, DrmEncoder *enc) {
  /* First try to use the currently-bound crtc */
  auto *crtc = FindCrtcById(enc->GetCurrentCrtcId());
  if (crtc != nullptr && bound_crtcs_.count(display) == 0) {
    bound_crtcs_[display] = crtc;
    bound_encoders_[crtc] = enc;
    return 0;
  }

  /* Try to find a possible crtc which will work */
  for (auto &crtc : crtcs_) {
    /* Crtc not supported or we've already tried this earlier */
    if (!enc->SupportsCrtc(*crtc) || crtc->GetId() == enc->GetCurrentCrtcId()) {
      continue;
    }

    if (bound_crtcs_.count(display) == 0) {
      bound_crtcs_[display] = crtc.get();
      bound_encoders_[crtc.get()] = enc;
      return 0;
    }
  }

  /* We can't use the encoder, but nothing went wrong, try another one */
  return -EAGAIN;
}

int DrmDevice::CreateDisplayPipe(DrmConnector *connector) {
  int display = connectors_to_display_id_.at(connector);
  /* Try to use current setup first */
  auto *enc0 = FindEncoderById(connector->GetCurrentEncoderId());
  if (enc0 != nullptr && encoders_to_display_id_.count(enc0) == 0) {
    int ret = TryEncoderForDisplay(display, enc0);
    if (ret == 0) {
      encoders_to_display_id_[enc0] = display;
      return 0;
    }

    if (ret != -EAGAIN) {
      ALOGE("Could not set mode %d/%d", display, ret);
      return ret;
    }
  }

  for (auto &enc : encoders_) {
    if (!connector->SupportsEncoder(*enc) ||
        encoders_to_display_id_.count(enc.get()) != 0) {
      continue;
    }

    int ret = TryEncoderForDisplay(display, enc.get());
    if (ret == 0) {
      encoders_to_display_id_[enc.get()] = display;
      return 0;
    }

    if (ret != -EAGAIN) {
      ALOGE("Could not set mode %d/%d", display, ret);
      return ret;
    }
  }
  ALOGE("Could not find a suitable encoder/crtc for display %d", display);
  return -ENODEV;
}

auto DrmDevice::RegisterUserPropertyBlob(void *data, size_t length) const
    -> DrmModeUserPropertyBlobUnique {
  struct drm_mode_create_blob create_blob {};
  create_blob.length = length;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  create_blob.data = (__u64)data;

  int ret = drmIoctl(GetFd(), DRM_IOCTL_MODE_CREATEPROPBLOB, &create_blob);
  if (ret != 0) {
    ALOGE("Failed to create mode property blob %d", ret);
    return {};
  }

  return DrmModeUserPropertyBlobUnique(
      new uint32_t(create_blob.blob_id), [this](const uint32_t *it) {
        struct drm_mode_destroy_blob destroy_blob {};
        destroy_blob.blob_id = (__u32)*it;
        int err = drmIoctl(GetFd(), DRM_IOCTL_MODE_DESTROYPROPBLOB,
                           &destroy_blob);
        if (err != 0) {
          ALOGE("Failed to destroy mode property blob %" PRIu32 "/%d", *it,
                err);
        }
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        delete it;
      });
}

int DrmDevice::GetProperty(uint32_t obj_id, uint32_t obj_type,
                           const char *prop_name, DrmProperty *property) const {
  drmModeObjectPropertiesPtr props = nullptr;

  props = drmModeObjectGetProperties(GetFd(), obj_id, obj_type);
  if (props == nullptr) {
    ALOGE("Failed to get properties for %d/%x", obj_id, obj_type);
    return -ENODEV;
  }

  bool found = false;
  for (int i = 0; !found && (size_t)i < props->count_props; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    drmModePropertyPtr p = drmModeGetProperty(GetFd(), props->props[i]);
    if (strcmp(p->name, prop_name) == 0) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      property->Init(obj_id, p, props->prop_values[i]);
      found = true;
    }
    drmModeFreeProperty(p);
  }

  drmModeFreeObjectProperties(props);
  return found ? 0 : -ENOENT;
}

std::string DrmDevice::GetName() const {
  auto *ver = drmGetVersion(GetFd());
  if (ver == nullptr) {
    ALOGW("Failed to get drm version for fd=%d", GetFd());
    return "generic";
  }

  std::string name(ver->name);
  drmFreeVersion(ver);
  return name;
}

auto DrmDevice::IsKMSDev(const char *path) -> bool {
  auto fd = UniqueFd(open(path, O_RDWR | O_CLOEXEC));
  if (!fd) {
    return false;
  }

  auto res = MakeDrmModeResUnique(fd.Get());
  if (!res) {
    return false;
  }

  bool is_kms = res->count_crtcs > 0 && res->count_connectors > 0 &&
                res->count_encoders > 0;

  return is_kms;
}

auto DrmDevice::GetConnectors()
    -> const std::vector<std::unique_ptr<DrmConnector>> & {
  return connectors_;
}

auto DrmDevice::GetPlanes() -> const std::vector<std::unique_ptr<DrmPlane>> & {
  return planes_;
}

auto DrmDevice::GetCrtcs() -> const std::vector<std::unique_ptr<DrmCrtc>> & {
  return crtcs_;
}

auto DrmDevice::GetEncoders()
    -> const std::vector<std::unique_ptr<DrmEncoder>> & {
  return encoders_;
}

}  // namespace android
