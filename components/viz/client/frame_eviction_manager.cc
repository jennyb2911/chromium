// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/client/frame_eviction_manager.h"

#include <algorithm>

#include "base/bind.h"
#include "base/logging.h"
#include "base/memory/memory_pressure_listener.h"
#include "base/memory/memory_pressure_monitor.h"
#include "base/memory/shared_memory.h"
#include "base/sys_info.h"
#include "build/build_config.h"

namespace viz {
namespace {

const int kModeratePressurePercentage = 50;
const int kCriticalPressurePercentage = 10;

}  // namespace

FrameEvictionManager* FrameEvictionManager::GetInstance() {
  return base::Singleton<FrameEvictionManager>::get();
}

FrameEvictionManager::~FrameEvictionManager() {}

void FrameEvictionManager::AddFrame(FrameEvictionManagerClient* frame,
                                    bool locked) {
  RemoveFrame(frame);
  if (locked)
    locked_frames_[frame] = 1;
  else
    unlocked_frames_.push_front(frame);
  CullUnlockedFrames(GetMaxNumberOfSavedFrames());
}

void FrameEvictionManager::RemoveFrame(FrameEvictionManagerClient* frame) {
  std::map<FrameEvictionManagerClient*, size_t>::iterator locked_iter =
      locked_frames_.find(frame);
  if (locked_iter != locked_frames_.end())
    locked_frames_.erase(locked_iter);
  unlocked_frames_.remove(frame);
}

void FrameEvictionManager::LockFrame(FrameEvictionManagerClient* frame) {
  std::list<FrameEvictionManagerClient*>::iterator unlocked_iter =
      std::find(unlocked_frames_.begin(), unlocked_frames_.end(), frame);
  if (unlocked_iter != unlocked_frames_.end()) {
    DCHECK(locked_frames_.find(frame) == locked_frames_.end());
    unlocked_frames_.remove(frame);
    locked_frames_[frame] = 1;
  } else {
    DCHECK(locked_frames_.find(frame) != locked_frames_.end());
    locked_frames_[frame]++;
  }
}

void FrameEvictionManager::UnlockFrame(FrameEvictionManagerClient* frame) {
  DCHECK(locked_frames_.find(frame) != locked_frames_.end());
  size_t locked_count = locked_frames_[frame];
  DCHECK(locked_count);
  if (locked_count > 1) {
    locked_frames_[frame]--;
  } else {
    RemoveFrame(frame);
    unlocked_frames_.push_front(frame);
    CullUnlockedFrames(GetMaxNumberOfSavedFrames());
  }
}

size_t FrameEvictionManager::GetMaxNumberOfSavedFrames() const {
  int percentage = 100;
  base::MemoryPressureMonitor* monitor = base::MemoryPressureMonitor::Get();

  if (!monitor)
    return max_number_of_saved_frames_;

  // Until we have a global OnMemoryPressureChanged event we need to query the
  // value from our specific pressure monitor.
  switch (monitor->GetCurrentPressureLevel()) {
    case base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_NONE:
      percentage = 100;
      break;
    case base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_MODERATE:
      percentage = kModeratePressurePercentage;
      break;
    case base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_CRITICAL:
      percentage = kCriticalPressurePercentage;
      break;
  }
  size_t frames = (max_number_of_saved_frames_ * percentage) / 100;
  return std::max(static_cast<size_t>(1), frames);
}

FrameEvictionManager::FrameEvictionManager()
    : memory_pressure_listener_(new base::MemoryPressureListener(
          base::Bind(&FrameEvictionManager::OnMemoryPressure,
                     base::Unretained(this)))) {
  max_number_of_saved_frames_ =
#if defined(OS_ANDROID)
      // If the amount of memory on the device is >= 3.5 GB, save up to 5
      // frames.
      base::SysInfo::AmountOfPhysicalMemoryMB() < 1024 * 3.5f ? 1 : 5;
#else
      std::min(5, 2 + (base::SysInfo::AmountOfPhysicalMemoryMB() / 256));
#endif
}

void FrameEvictionManager::CullUnlockedFrames(size_t saved_frame_limit) {
  while (!unlocked_frames_.empty() &&
         unlocked_frames_.size() + locked_frames_.size() > saved_frame_limit) {
    size_t old_size = unlocked_frames_.size();
    // Should remove self from list.
    unlocked_frames_.back()->EvictCurrentFrame();
    DCHECK_EQ(unlocked_frames_.size() + 1, old_size);
  }
}

void FrameEvictionManager::OnMemoryPressure(
    base::MemoryPressureListener::MemoryPressureLevel memory_pressure_level) {
  switch (memory_pressure_level) {
    case base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_MODERATE:
      PurgeMemory(kModeratePressurePercentage);
      break;
    case base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_CRITICAL:
      PurgeMemory(kCriticalPressurePercentage);
      break;
    case base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_NONE:
      // No need to change anything when there is no pressure.
      return;
  }
}

void FrameEvictionManager::PurgeMemory(int percentage) {
  int saved_frame_limit = max_number_of_saved_frames_;
  if (saved_frame_limit <= 1)
    return;
  CullUnlockedFrames(std::max(1, (saved_frame_limit * percentage) / 100));
}

void FrameEvictionManager::PurgeAllUnlockedFrames() {
  CullUnlockedFrames(0);
}

}  // namespace viz
