/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef RESOURCEMANAGER_H
#define RESOURCEMANAGER_H

#include <cstring>

#include "DrmDevice.h"
#include "DrmFbImporter.h"
#include "UEventListener.h"

namespace android {

class ResourceManager {
 public:
  ResourceManager();
  ResourceManager(const ResourceManager &) = delete;
  ResourceManager &operator=(const ResourceManager &) = delete;
  ~ResourceManager();

  int Init();
  auto GetPipeline(int display) -> DrmDisplayPipeline *;
  auto &GetDrmDevices() const {
    return drms_;
  }
  int GetDisplayCount() const {
    return num_displays_;
  }
  bool ForcedScalingWithGpu() const {
    return scale_with_gpu_;
  }

  UEventListener *GetUEventListener() {
    return &uevent_listener_;
  }

  auto &GetMainLock() {
    return main_lock_;
  }

 private:
  int AddDrmDevice(std::string const &path);

  int num_displays_;
  std::vector<std::unique_ptr<DrmDevice>> drms_;

  bool scale_with_gpu_{};

  UEventListener uevent_listener_;

  std::mutex main_lock_;
};
}  // namespace android

#endif  // RESOURCEMANAGER_H
