// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GPU_COMMAND_BUFFER_SERVICE_CONTEXT_GROUP_H_
#define GPU_COMMAND_BUFFER_SERVICE_CONTEXT_GROUP_H_

#include <stdint.h>

#include <memory>
#include <vector>

#include "base/containers/hash_tables.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "gpu/command_buffer/common/constants.h"
#include "gpu/command_buffer/common/gles2_cmd_format.h"
#include "gpu/command_buffer/common/gles2_cmd_utils.h"
#include "gpu/command_buffer/service/feature_info.h"
#include "gpu/command_buffer/service/framebuffer_completeness_cache.h"
#include "gpu/command_buffer/service/shader_translator_cache.h"
#include "gpu/config/gpu_feature_info.h"
#include "gpu/config/gpu_preferences.h"
#include "gpu/gpu_gles2_export.h"

namespace gpu {

class ImageFactory;
struct GpuPreferences;
class MailboxManager;
class TransferBufferManager;
class ServiceDiscardableManager;
class DecoderContext;

namespace gles2 {

class ProgramCache;
class BufferManager;
class ImageManager;
class RenderbufferManager;
class PathManager;
class ProgramManager;
class ProgressReporter;
class SamplerManager;
class ShaderManager;
class TextureManager;
class MemoryTracker;
struct DisallowedFeatures;
struct PassthroughResources;

DisallowedFeatures AdjustDisallowedFeatures(
    ContextType context_type,
    const DisallowedFeatures& disallowed_features);

// A Context Group helps manage multiple DecoderContexts that share
// resources.
class GPU_GLES2_EXPORT ContextGroup : public base::RefCounted<ContextGroup> {
 public:
  ContextGroup(const GpuPreferences& gpu_preferences,
               bool supports_passthrough_command_decoders,
               MailboxManager* mailbox_manager,
               std::unique_ptr<MemoryTracker> memory_tracker,
               ShaderTranslatorCache* shader_translator_cache,
               FramebufferCompletenessCache* framebuffer_completeness_cache,
               const scoped_refptr<FeatureInfo>& feature_info,
               bool bind_generates_resource,
               ImageManager* image_manager,
               gpu::ImageFactory* image_factory,
               ProgressReporter* progress_reporter,
               const GpuFeatureInfo& gpu_feature_info,
               ServiceDiscardableManager* discardable_manager);

  // This should only be called by a DecoderContext. This must be paired with a
  // call to destroy if it succeeds.
  gpu::ContextResult Initialize(DecoderContext* decoder,
                                ContextType context_type,
                                const DisallowedFeatures& disallowed_features);

  // Destroys all the resources when called for the last context in the group.
  // It should only be called by DecoderContext.
  void Destroy(DecoderContext* decoder, bool have_context);

  MailboxManager* mailbox_manager() const { return mailbox_manager_; }

  MemoryTracker* memory_tracker() const { return memory_tracker_.get(); }

  ShaderTranslatorCache* shader_translator_cache() const {
    return shader_translator_cache_;
  }

  FramebufferCompletenessCache* framebuffer_completeness_cache() const {
    return framebuffer_completeness_cache_;
  }

  bool bind_generates_resource() {
    return bind_generates_resource_;
  }

  uint32_t max_vertex_attribs() const { return max_vertex_attribs_; }

  uint32_t max_texture_units() const { return max_texture_units_; }

  uint32_t max_texture_image_units() const { return max_texture_image_units_; }

  uint32_t max_vertex_texture_image_units() const {
    return max_vertex_texture_image_units_;
  }

  uint32_t max_fragment_uniform_vectors() const {
    return max_fragment_uniform_vectors_;
  }

  uint32_t max_varying_vectors() const { return max_varying_vectors_; }

  uint32_t max_vertex_uniform_vectors() const {
    return max_vertex_uniform_vectors_;
  }

  uint32_t max_color_attachments() const { return max_color_attachments_; }

  uint32_t max_draw_buffers() const { return max_draw_buffers_; }

  uint32_t max_dual_source_draw_buffers() const {
    return max_dual_source_draw_buffers_;
  }

  uint32_t max_vertex_output_components() const {
    return max_vertex_output_components_;
  }

  uint32_t max_fragment_input_components() const {
    return max_fragment_input_components_;
  }

  int32_t min_program_texel_offset() const { return min_program_texel_offset_; }

  int32_t max_program_texel_offset() const { return max_program_texel_offset_; }

  uint32_t max_transform_feedback_separate_attribs() const {
    return max_transform_feedback_separate_attribs_;
  }

  uint32_t max_uniform_buffer_bindings() const {
    return max_uniform_buffer_bindings_;
  }

  uint32_t uniform_buffer_offset_alignment() const {
    return uniform_buffer_offset_alignment_;
  }

  FeatureInfo* feature_info() {
    return feature_info_.get();
  }

  ImageManager* image_manager() const { return image_manager_; }

  gpu::ImageFactory* image_factory() const { return image_factory_; }

  const GpuPreferences& gpu_preferences() const {
    return gpu_preferences_;
  }

  BufferManager* buffer_manager() const {
    return buffer_manager_.get();
  }

  RenderbufferManager* renderbuffer_manager() const {
    return renderbuffer_manager_.get();
  }

  TextureManager* texture_manager() const {
    return texture_manager_.get();
  }

  PathManager* path_manager() const { return path_manager_.get(); }

  ProgramManager* program_manager() const {
    return program_manager_.get();
  }

  bool has_program_cache() const { return program_cache_ != nullptr; }

  void set_program_cache(ProgramCache* program_cache) {
    program_cache_ = program_cache;
  }

  ShaderManager* shader_manager() const {
    return shader_manager_.get();
  }

  TransferBufferManager* transfer_buffer_manager() const {
    return transfer_buffer_manager_.get();
  }

  SamplerManager* sampler_manager() const {
    return sampler_manager_.get();
  }

  ServiceDiscardableManager* discardable_manager() const {
    return discardable_manager_;
  }

  uint32_t GetMemRepresented() const;

  // Loses all the context associated with this group.
  void LoseContexts(error::ContextLostReason reason);

  bool GetBufferServiceId(GLuint client_id, GLuint* service_id) const;

  void AddSyncId(GLuint client_id, GLsync service_id) {
    syncs_id_map_[client_id] = service_id;
  }

  bool GetSyncServiceId(GLuint client_id, GLsync* service_id) const {
    base::hash_map<GLuint, GLsync>::const_iterator iter =
        syncs_id_map_.find(client_id);
    if (iter == syncs_id_map_.end())
      return false;
    if (service_id)
      *service_id = iter->second;
    return true;
  }

  void RemoveSyncId(GLuint client_id) {
    syncs_id_map_.erase(client_id);
  }

  bool use_passthrough_cmd_decoder() const {
    return use_passthrough_cmd_decoder_;
  }

  PassthroughResources* passthrough_resources() const {
    return passthrough_resources_.get();
  }

  const GpuFeatureInfo& gpu_feature_info() const { return gpu_feature_info_; }

  void ReportProgress();

 private:
  friend class base::RefCounted<ContextGroup>;
  ~ContextGroup();

  bool CheckGLFeature(GLint min_required, GLint* v);
  bool CheckGLFeatureU(GLint min_required, uint32_t* v);
  bool QueryGLFeature(GLenum pname, GLint min_required, GLint* v);
  bool QueryGLFeatureU(GLenum pname, GLint min_required, uint32_t* v);
  bool HaveContexts();

  // It's safer to make a copy of the GpuPreferences struct rather
  // than refer to the one passed in to the constructor.
  const GpuPreferences gpu_preferences_;
  MailboxManager* mailbox_manager_;
  std::unique_ptr<MemoryTracker> memory_tracker_;
  ShaderTranslatorCache* shader_translator_cache_;
  FramebufferCompletenessCache* framebuffer_completeness_cache_;
  std::unique_ptr<TransferBufferManager> transfer_buffer_manager_;

  bool enforce_gl_minimums_;
  bool bind_generates_resource_;

  uint32_t max_vertex_attribs_;
  uint32_t max_texture_units_;
  uint32_t max_texture_image_units_;
  uint32_t max_vertex_texture_image_units_;
  uint32_t max_fragment_uniform_vectors_;
  uint32_t max_varying_vectors_;
  uint32_t max_vertex_uniform_vectors_;
  uint32_t max_color_attachments_;
  uint32_t max_draw_buffers_;
  uint32_t max_dual_source_draw_buffers_;

  uint32_t max_vertex_output_components_;
  uint32_t max_fragment_input_components_;
  int32_t min_program_texel_offset_;
  int32_t max_program_texel_offset_;

  uint32_t max_transform_feedback_separate_attribs_;
  uint32_t max_uniform_buffer_bindings_;
  uint32_t uniform_buffer_offset_alignment_;

  ProgramCache* program_cache_;

  std::unique_ptr<BufferManager> buffer_manager_;

  std::unique_ptr<RenderbufferManager> renderbuffer_manager_;

  std::unique_ptr<TextureManager> texture_manager_;

  std::unique_ptr<PathManager> path_manager_;

  std::unique_ptr<ProgramManager> program_manager_;

  std::unique_ptr<ShaderManager> shader_manager_;

  std::unique_ptr<SamplerManager> sampler_manager_;

  scoped_refptr<FeatureInfo> feature_info_;

  ImageManager* image_manager_;

  gpu::ImageFactory* image_factory_;

  std::vector<base::WeakPtr<DecoderContext>> decoders_;

  // Mappings from client side IDs to service side IDs.
  base::hash_map<GLuint, GLsync> syncs_id_map_;

  bool use_passthrough_cmd_decoder_;
  std::unique_ptr<PassthroughResources> passthrough_resources_;

  // Used to notify the watchdog thread of progress during destruction,
  // preventing time-outs when destruction takes a long time. May be null when
  // using in-process command buffer.
  ProgressReporter* progress_reporter_;

  GpuFeatureInfo gpu_feature_info_;

  ServiceDiscardableManager* discardable_manager_;

  DISALLOW_COPY_AND_ASSIGN(ContextGroup);
};

}  // namespace gles2
}  // namespace gpu

#endif  // GPU_COMMAND_BUFFER_SERVICE_CONTEXT_GROUP_H_
