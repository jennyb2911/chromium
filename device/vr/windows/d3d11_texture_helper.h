// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_VR_WINDOWS_D3D11_TEXTURE_HELPER_H
#define DEVICE_VR_WINDOWS_D3D11_TEXTURE_HELPER_H

#include <D3D11_1.h>
#include <DXGI1_4.h>
#include <wrl.h>

#include "base/win/scoped_handle.h"
#include "mojo/public/cpp/system/platform_handle.h"
#include "ui/gfx/geometry/rect_f.h"

namespace device {

class D3D11TextureHelper {
 public:
  D3D11TextureHelper();
  ~D3D11TextureHelper();

  void Reset();

  bool EnsureInitialized();
  bool SetAdapterIndex(int32_t index);
  bool SetAdapterLUID(const LUID& luid);

  void CleanupNoSubmit();
  void SetSourceAndOverlayVisible(bool source_visible, bool overlay_visible);

  bool CompositeToBackBuffer();
  bool SetSourceTexture(base::win::ScopedHandle texture_handle,
                        gfx::RectF left,
                        gfx::RectF right);
  bool SetOverlayTexture(base::win::ScopedHandle texture_handle,
                         gfx::RectF left,
                         gfx::RectF right);

  void AllocateBackBuffer();
  const Microsoft::WRL::ComPtr<ID3D11Texture2D>& GetBackbuffer();

  bool UpdateBackbufferSizes();
  gfx::RectF BackBufferLeft() { return target_left_; }
  gfx::RectF BackBufferRight() { return target_right_; }
  gfx::Size BackBufferSize() { return target_size_; }
  void SetBackbuffer(Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer);
  Microsoft::WRL::ComPtr<ID3D11Device> GetDevice();

  void SetDefaultSize(gfx::Size size) { default_size_ = size; }

 private:
  struct LayerData {
    LayerData();
    ~LayerData();

    Microsoft::WRL::ComPtr<ID3D11Texture2D> source_texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shader_resource_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed_mutex_;
    gfx::RectF left_;   // 0 to 1 in each direction
    gfx::RectF right_;  // 0 to 1 in each direction
    bool submitted_this_frame_ = false;
  };

  bool EnsureRenderTargetView();
  bool EnsureShaders();
  bool EnsureInputLayout();
  bool EnsureVertexBuffer();
  bool UpdateVertexBuffer(LayerData& layer);
  bool EnsureSampler(LayerData& layer);
  Microsoft::WRL::ComPtr<IDXGIAdapter> GetAdapter();
  bool CompositeLayer(LayerData& layer);
  bool BindTarget();
  void CleanupLayerData(LayerData& layer);

  struct RenderState {
    RenderState();
    ~RenderState();

    Microsoft::WRL::ComPtr<ID3D11Device1> d3d11_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11_device_context_;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_view_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> flip_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> flip_vertex_shader_;

    Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> target_texture_;

    LayerData source_;
    LayerData overlay_;
  };

  bool overlay_visible_ = true;
  bool source_visible_ = true;

  gfx::RectF target_left_;   // 0 to 1 in each direction
  gfx::RectF target_right_;  // 0 to 1 in each direction
  gfx::Size target_size_;

  gfx::Size default_size_;

  RenderState render_state_;
  int32_t adapter_index_ = -1;
  LUID adapter_luid_ = {};
};
}  // namespace device

#endif  // DEVICE_VR_WINDOWS_D3D11_TEXTURE_HELPER_H
