// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_SYNC_DRIVER_MODEL_TYPE_CONTROLLER_H_
#define COMPONENTS_SYNC_DRIVER_MODEL_TYPE_CONTROLLER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/callback.h"
#include "base/containers/flat_map.h"
#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "components/sync/base/model_type.h"
#include "components/sync/driver/configure_context.h"
#include "components/sync/driver/data_type_controller.h"
#include "components/sync/model/model_error.h"
#include "components/sync/model/model_type_controller_delegate.h"
#include "components/sync/model/sync_error.h"

namespace syncer {

struct DataTypeActivationResponse;

// DataTypeController implementation for Unified Sync and Storage model types.
class ModelTypeController : public DataTypeController {
 public:
  ModelTypeController(
      ModelType type,
      std::unique_ptr<ModelTypeControllerDelegate> delegate_on_disk);
  // For datatypes that have support for STORAGE_IN_MEMORY.
  ModelTypeController(
      ModelType type,
      std::unique_ptr<ModelTypeControllerDelegate> delegate_on_disk,
      std::unique_ptr<ModelTypeControllerDelegate> delegate_in_memory);
  ~ModelTypeController() override;

  // DataTypeController implementation.
  bool ShouldLoadModelBeforeConfigure() const override;
  void BeforeLoadModels(ModelTypeConfigurer* configurer) override;
  void LoadModels(const ConfigureContext& configure_context,
                  const ModelLoadCallback& model_load_callback) override;
  void RegisterWithBackend(base::Callback<void(bool)> set_downloaded,
                           ModelTypeConfigurer* configurer) override;
  void StartAssociating(const StartCallback& start_callback) override;
  void ActivateDataType(ModelTypeConfigurer* configurer) override;
  void DeactivateDataType(ModelTypeConfigurer* configurer) override;
  void Stop(SyncStopMetadataFate metadata_fate, StopCallback callback) override;
  State state() const override;
  void GetAllNodes(const AllNodesCallback& callback) override;
  void GetStatusCounters(const StatusCountersCallback& callback) override;
  void RecordMemoryUsageAndCountsHistograms() override;

 protected:
  void ReportModelError(SyncError::ErrorType error_type,
                        const ModelError& error);

 private:
  void RecordStartFailure(ConfigureResult result) const;

  // If the DataType controller is waiting for models to load, once the models
  // are loaded this function should be called to let the base class
  // implementation know that it is safe to continue with the activation.
  // The error indicates whether the loading completed successfully.
  void LoadModelsDone(ConfigureResult result, const SyncError& error);

  // The function will do the real work when OnProcessorStarted got called. This
  // is called on the UI thread.
  void OnProcessorStarted(
      std::unique_ptr<DataTypeActivationResponse> activation_response);

  base::flat_map<ConfigureContext::StorageOption,
                 std::unique_ptr<ModelTypeControllerDelegate>>
      delegate_map_;

  // State of this datatype controller.
  State state_ = NOT_RUNNING;

  // Owned by |delegate_map_|. Null while NOT_RUNNING.
  ModelTypeControllerDelegate* delegate_ = nullptr;

  // Callback for use when starting the datatype (usually MODEL_STARTING, but
  // STOPPING if abort requested while starting).
  ModelLoadCallback model_load_callback_;

  // Callbacks for use when stopping the datatype (STOPPING), which also
  // includes aborting a start. This is important because STOPPING is a state
  // used to make sure we don't request two starts in parallel to the delegate,
  // which is hard to support (most notably in ClientTagBasedProcessor). We
  // use a vector because it's allowed to call Stop() multiple times (i.e. while
  // STOPPING).
  std::vector<StopCallback> model_stop_callbacks_;
  SyncStopMetadataFate model_stop_metadata_fate_;

  // Controller receives |activation_response_| from
  // ClientTagBasedModelTypeProcessor callback and must temporarily own it until
  // ActivateDataType is called.
  std::unique_ptr<DataTypeActivationResponse> activation_response_;

  // This is a hack to prevent reconfigurations from crashing, because USS
  // activation is not idempotent. RegisterWithBackend only needs to actually do
  // something the first time after the type is enabled.
  // TODO(crbug.com/647505): Remove this once the DTM handles things better.
  bool activated_ = false;

  DISALLOW_COPY_AND_ASSIGN(ModelTypeController);
};

}  // namespace syncer

#endif  // COMPONENTS_SYNC_DRIVER_MODEL_TYPE_CONTROLLER_H_
