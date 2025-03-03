// Copyright (c) 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/vr/windows/d3d11_texture_helper.h"
#include "mojo/public/c/system/platform_handle.h"

namespace {
#include "device/vr/windows/flip_pixel_shader.h"
#include "device/vr/windows/flip_vertex_shader.h"

constexpr int kAcquireWaitMS = 2000;

// 2 position, 2 texture.
const size_t kElementsPerVertex = 4;
constexpr size_t kSizeOfVertex = sizeof(float) * kElementsPerVertex;

// 2 triangles per eye
constexpr size_t kNumVerticesPerLayer = 12;
}

namespace device {

D3D11TextureHelper::RenderState::RenderState() {}
D3D11TextureHelper::RenderState::~RenderState() {}

D3D11TextureHelper::LayerData::LayerData() = default;
D3D11TextureHelper::LayerData::~LayerData() = default;

D3D11TextureHelper::D3D11TextureHelper() {}

D3D11TextureHelper::~D3D11TextureHelper() {}

void D3D11TextureHelper::Reset() {
  render_state_ = {};
}

void D3D11TextureHelper::SetSourceAndOverlayVisible(bool source_visible,
                                                    bool overlay_visible) {
  source_visible_ = source_visible;
  overlay_visible_ = overlay_visible;

  if (!source_visible_) {
    render_state_.source_.keyed_mutex_ = nullptr;
    render_state_.source_.source_texture_ = nullptr;
    render_state_.source_.shader_resource_ = nullptr;
    render_state_.source_.sampler_ = nullptr;
  }
  if (!overlay_visible_) {
    render_state_.overlay_.keyed_mutex_ = nullptr;
    render_state_.overlay_.source_texture_ = nullptr;
    render_state_.overlay_.shader_resource_ = nullptr;
    render_state_.overlay_.sampler_ = nullptr;
  }
}

void D3D11TextureHelper::CleanupNoSubmit() {
  render_state_.source_.keyed_mutex_ = nullptr;
  render_state_.source_.source_texture_ = nullptr;
  render_state_.source_.shader_resource_ = nullptr;
  render_state_.source_.sampler_ = nullptr;

  render_state_.overlay_.keyed_mutex_ = nullptr;
  render_state_.overlay_.source_texture_ = nullptr;
  render_state_.overlay_.shader_resource_ = nullptr;
  render_state_.overlay_.sampler_ = nullptr;
}

void D3D11TextureHelper::CleanupLayerData(LayerData& layer) {
  if (!layer.submitted_this_frame_) {
    layer.keyed_mutex_ = nullptr;
    layer.sampler_ = nullptr;
    layer.shader_resource_ = nullptr;
    layer.source_texture_ = nullptr;
  }

  // Set up for the next frame so we know if we submitted again.
  layer.submitted_this_frame_ = false;
}

bool D3D11TextureHelper::CompositeToBackBuffer() {
  if (!EnsureInitialized())
    return false;

  // Clear stale data:
  CleanupLayerData(render_state_.source_);
  CleanupLayerData(render_state_.overlay_);

  if (!render_state_.source_.source_texture_ &&
      !render_state_.overlay_.source_texture_)
    return false;
  if (!render_state_.target_texture_)
    return false;

  HRESULT hr = S_OK;
  if (render_state_.source_.keyed_mutex_) {
    hr = render_state_.source_.keyed_mutex_->AcquireSync(1, kAcquireWaitMS);
    if (FAILED(hr) || hr == WAIT_TIMEOUT || hr == WAIT_ABANDONED) {
      // We failed to acquire the lock.  We'll drop this frame, but subsequent
      // frames won't be affected.
      return false;
    }
  }

  if (render_state_.overlay_.keyed_mutex_) {
    hr = render_state_.overlay_.keyed_mutex_->AcquireSync(1, kAcquireWaitMS);
    if (FAILED(hr) || hr == WAIT_TIMEOUT || hr == WAIT_ABANDONED) {
      // We failed to acquire the lock.  We'll drop this frame, but subsequent
      // frames won't be affected.

      if (render_state_.source_.keyed_mutex_) {
        render_state_.source_.keyed_mutex_->ReleaseSync(0);
      }
      return false;
    }
  }

  if (!BindTarget()) {
    return false;
  }

  if (render_state_.overlay_.source_texture_ &&
      (!render_state_.source_.source_texture_ || !source_visible_)) {
    // If we have an overlay, but no WebXR texture under it, clear the target
    // first, since overlay may assume transparency.
    float color_rgba[4] = {0, 0, 0, 1};
    render_state_.d3d11_device_context_->ClearRenderTargetView(
        render_state_.render_target_view_.Get(), color_rgba);
  }

  bool success = true;
  if (render_state_.source_.source_texture_)
    success = success && CompositeLayer(render_state_.source_);
  if (render_state_.overlay_.source_texture_)
    success = success && CompositeLayer(render_state_.overlay_);

  if (render_state_.source_.keyed_mutex_)
    render_state_.source_.keyed_mutex_->ReleaseSync(0);
  if (render_state_.overlay_.keyed_mutex_)
    render_state_.overlay_.keyed_mutex_->ReleaseSync(0);

  return success;
}

bool D3D11TextureHelper::EnsureRenderTargetView() {
  if (!render_state_.render_target_view_) {
    D3D11_RENDER_TARGET_VIEW_DESC render_target_view_desc;
    render_target_view_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    render_target_view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    render_target_view_desc.Texture2D.MipSlice = 0;
    HRESULT hr = render_state_.d3d11_device_->CreateRenderTargetView(
        render_state_.target_texture_.Get(), &render_target_view_desc,
        &render_state_.render_target_view_);
    if (FAILED(hr))
      return false;
  }
  return true;
}

bool D3D11TextureHelper::EnsureShaders() {
  if (!render_state_.flip_vertex_shader_) {
    HRESULT hr = render_state_.d3d11_device_->CreateVertexShader(
        g_flip_vertex, _countof(g_flip_vertex), nullptr,
        &render_state_.flip_vertex_shader_);
    if (FAILED(hr))
      return false;
  }

  if (!render_state_.flip_pixel_shader_) {
    HRESULT hr = render_state_.d3d11_device_->CreatePixelShader(
        g_flip_pixel, _countof(g_flip_pixel), nullptr,
        &render_state_.flip_pixel_shader_);
    if (FAILED(hr))
      return false;
  }

  return true;
}

bool D3D11TextureHelper::EnsureInputLayout() {
  if (!render_state_.input_layout_) {
    D3D11_INPUT_ELEMENT_DESC vertex_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
         D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    HRESULT hr = render_state_.d3d11_device_->CreateInputLayout(
        vertex_desc, std::size(vertex_desc), g_flip_vertex,
        _countof(g_flip_vertex), &render_state_.input_layout_);
    if (FAILED(hr))
      return false;
  }
  return true;
}

bool D3D11TextureHelper::EnsureVertexBuffer() {
  if (!render_state_.vertex_buffer_) {
    // Pairs of x/y coordinates, and UVs for 2 triangles in a quad.
    CD3D11_BUFFER_DESC vertex_buffer_desc(kSizeOfVertex * kNumVerticesPerLayer,
                                          D3D11_BIND_VERTEX_BUFFER);
    HRESULT hr = render_state_.d3d11_device_->CreateBuffer(
        &vertex_buffer_desc, nullptr, &render_state_.vertex_buffer_);
    if (FAILED(hr))
      return false;
  }
  return true;
}

bool D3D11TextureHelper::EnsureSampler(LayerData& layer) {
  if (!layer.sampler_) {
    CD3D11_DEFAULT default_values;
    CD3D11_SAMPLER_DESC sampler_desc = CD3D11_SAMPLER_DESC(default_values);
    D3D11_SAMPLER_DESC sd = sampler_desc;
    HRESULT hr = render_state_.d3d11_device_->CreateSamplerState(
        &sd, layer.sampler_.GetAddressOf());
    if (FAILED(hr))
      return false;
  }
  return true;
}

bool D3D11TextureHelper::BindTarget() {
  if (!EnsureRenderTargetView())
    return false;

  render_state_.d3d11_device_context_->OMSetRenderTargets(
      1, render_state_.render_target_view_.GetAddressOf(), nullptr);
  return true;
}

void PushVertRect(std::vector<float>& data,
                  const gfx::RectF& rect,
                  const gfx::RectF& uv) {
  data.push_back(rect.x() * 2 - 1);
  data.push_back(rect.y() * 2 - 1);
  data.push_back(uv.x());
  data.push_back(uv.y());

  data.push_back(rect.x() * 2 - 1);
  data.push_back((rect.y() + rect.height()) * 2 - 1);
  data.push_back(uv.x());
  data.push_back(uv.y() + uv.height());

  data.push_back((rect.x() + rect.width()) * 2 - 1);
  data.push_back((rect.y() + rect.height()) * 2 - 1);
  data.push_back(uv.x() + uv.width());
  data.push_back(uv.y() + uv.height());

  data.push_back(rect.x() * 2 - 1);
  data.push_back(rect.y() * 2 - 1);
  data.push_back(uv.x());
  data.push_back(uv.y());

  data.push_back((rect.x() + rect.width()) * 2 - 1);
  data.push_back((rect.y() + rect.height()) * 2 - 1);
  data.push_back(uv.x() + uv.width());
  data.push_back(uv.y() + uv.height());

  data.push_back((rect.x() + rect.width()) * 2 - 1);
  data.push_back(rect.y() * 2 - 1);
  data.push_back(uv.x() + uv.width());
  data.push_back(uv.y());
}

bool D3D11TextureHelper::UpdateVertexBuffer(LayerData& layer) {
  std::vector<float> vertex_data;
  PushVertRect(vertex_data, target_left_, layer.left_);
  PushVertRect(vertex_data, target_right_, layer.right_);
  render_state_.d3d11_device_context_->UpdateSubresource(
      render_state_.vertex_buffer_.Get(), 0, nullptr, vertex_data.data(),
      sizeof(float) * kElementsPerVertex,
      vertex_data.size() / kElementsPerVertex);
  return true;
}

bool D3D11TextureHelper::CompositeLayer(LayerData& layer) {
  if (!EnsureShaders() || !EnsureInputLayout() || !EnsureVertexBuffer() ||
      !EnsureSampler(layer) || !UpdateVertexBuffer(layer))
    return false;

  render_state_.d3d11_device_context_->VSSetShader(
      render_state_.flip_vertex_shader_.Get(), nullptr, 0);
  render_state_.d3d11_device_context_->PSSetShader(
      render_state_.flip_pixel_shader_.Get(), nullptr, 0);
  render_state_.d3d11_device_context_->IASetInputLayout(
      render_state_.input_layout_.Get());

  UINT stride = kSizeOfVertex;
  UINT offset = 0;
  render_state_.d3d11_device_context_->IASetVertexBuffers(
      0, 1, render_state_.vertex_buffer_.GetAddressOf(), &stride, &offset);
  render_state_.d3d11_device_context_->PSSetSamplers(
      0, 1, layer.sampler_.GetAddressOf());

  D3D11_SHADER_RESOURCE_VIEW_DESC shader_resource_view_desc;
  shader_resource_view_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  shader_resource_view_desc.ViewDimension = D3D_SRV_DIMENSION_TEXTURE2D;
  shader_resource_view_desc.Texture2D.MostDetailedMip = 0;
  shader_resource_view_desc.Texture2D.MipLevels = 1;
  HRESULT hr = render_state_.d3d11_device_->CreateShaderResourceView(
      layer.source_texture_.Get(), &shader_resource_view_desc,
      layer.shader_resource_.ReleaseAndGetAddressOf());
  if (FAILED(hr))
    return false;
  render_state_.d3d11_device_context_->PSSetShaderResources(
      0, 1, layer.shader_resource_.GetAddressOf());

  D3D11_TEXTURE2D_DESC desc;
  render_state_.target_texture_->GetDesc(&desc);
  D3D11_VIEWPORT viewport = {0, 0, desc.Width, desc.Height, 0, 1};
  render_state_.d3d11_device_context_->RSSetViewports(1, &viewport);
  render_state_.d3d11_device_context_->IASetPrimitiveTopology(
      D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  render_state_.d3d11_device_context_->Draw(kNumVerticesPerLayer, 0);
  return true;
}

bool D3D11TextureHelper::SetSourceTexture(
    base::win::ScopedHandle texture_handle,
    gfx::RectF left,
    gfx::RectF right) {
  render_state_.source_.source_texture_ = nullptr;
  render_state_.source_.keyed_mutex_ = nullptr;
  render_state_.source_.left_ = left;
  render_state_.source_.right_ = right;
  render_state_.source_.submitted_this_frame_ = true;

  if (!EnsureInitialized())
    return false;
  HRESULT hr = render_state_.d3d11_device_->OpenSharedResource1(
      texture_handle.Get(),
      IID_PPV_ARGS(
          render_state_.source_.keyed_mutex_.ReleaseAndGetAddressOf()));
  if (FAILED(hr))
    return false;
  hr = render_state_.source_.keyed_mutex_.CopyTo(
      render_state_.source_.source_texture_.ReleaseAndGetAddressOf());
  if (FAILED(hr)) {
    render_state_.source_.keyed_mutex_ = nullptr;
    return false;
  }

  return true;
}

bool D3D11TextureHelper::SetOverlayTexture(
    base::win::ScopedHandle texture_handle,
    gfx::RectF left,
    gfx::RectF right) {
  render_state_.overlay_.source_texture_ = nullptr;
  render_state_.overlay_.keyed_mutex_ = nullptr;
  render_state_.overlay_.left_ = left;
  render_state_.overlay_.right_ = right;
  render_state_.overlay_.submitted_this_frame_ = true;

  if (!EnsureInitialized())
    return false;
  HRESULT hr = render_state_.d3d11_device_->OpenSharedResource1(
      texture_handle.Get(),
      IID_PPV_ARGS(
          render_state_.overlay_.keyed_mutex_.ReleaseAndGetAddressOf()));
  if (FAILED(hr))
    return false;
  hr = render_state_.overlay_.keyed_mutex_.CopyTo(
      render_state_.overlay_.source_texture_.ReleaseAndGetAddressOf());
  if (FAILED(hr)) {
    render_state_.overlay_.keyed_mutex_ = nullptr;
    return false;
  }

  return true;
}

bool D3D11TextureHelper::UpdateBackbufferSizes() {
  if (!EnsureInitialized())
    return false;

  if (!render_state_.source_.source_texture_ &&
      !render_state_.overlay_.source_texture_)
    return false;

  if (render_state_.source_.source_texture_ &&
      render_state_.overlay_.source_texture_) {
    target_left_ = gfx::RectF(0 /*x*/, 0 /*y*/, 0.5f /*width*/, 1 /*height*/);
    target_right_ =
        gfx::RectF(0.5f /*x*/, 0 /*y*/, 0.5f /*width*/, 1 /*height*/);
    target_size_ = default_size_;
    return true;
  }

  LayerData* layer = render_state_.overlay_.source_texture_
                         ? &render_state_.overlay_
                         : &render_state_.source_;
  D3D11_TEXTURE2D_DESC desc_desired;
  layer->source_texture_->GetDesc(&desc_desired);
  target_left_ = layer->left_;
  target_right_ = layer->right_;
  target_size_ = gfx::Size(desc_desired.Width, desc_desired.Height);
  return true;
}

void D3D11TextureHelper::AllocateBackBuffer() {
  if (!EnsureInitialized())
    return;

  // If we don't have anything to composite, just return.
  if (!render_state_.source_.source_texture_ &&
      !render_state_.overlay_.source_texture_)
    return;

  LayerData* layer = render_state_.overlay_.source_texture_
                         ? &render_state_.overlay_
                         : &render_state_.source_;

  D3D11_TEXTURE2D_DESC desc_desired;
  layer->source_texture_->GetDesc(&desc_desired);
  desc_desired.MiscFlags = 0;
  desc_desired.Width = target_size_.width();
  desc_desired.Height = target_size_.height();

  if (render_state_.target_texture_) {
    D3D11_TEXTURE2D_DESC desc_target;
    render_state_.target_texture_->GetDesc(&desc_target);
    // If the target should change size, format, or other properties reallocate
    // a new texture and new render target view.
    if (desc_desired.Width != desc_target.Width ||
        desc_desired.Height != desc_target.Height ||
        desc_desired.MipLevels != desc_target.MipLevels ||
        desc_desired.ArraySize != desc_target.ArraySize ||
        desc_desired.Format != desc_target.Format ||
        desc_desired.SampleDesc.Count != desc_target.SampleDesc.Count ||
        desc_desired.SampleDesc.Quality != desc_target.SampleDesc.Quality ||
        desc_desired.Usage != desc_target.Usage ||
        desc_desired.BindFlags != desc_target.BindFlags ||
        desc_desired.CPUAccessFlags != desc_target.CPUAccessFlags ||
        desc_desired.MiscFlags != desc_target.MiscFlags) {
      render_state_.target_texture_ = nullptr;
      render_state_.render_target_view_ = nullptr;
    }
  }

  if (!render_state_.target_texture_) {
    // Ignoring error - target_texture_ will be null on failure.
    render_state_.d3d11_device_->CreateTexture2D(
        &desc_desired, nullptr,
        render_state_.target_texture_.ReleaseAndGetAddressOf());
  }
}

const Microsoft::WRL::ComPtr<ID3D11Texture2D>&
D3D11TextureHelper::GetBackbuffer() {
  return render_state_.target_texture_;
}

void D3D11TextureHelper::SetBackbuffer(
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer) {
  if (render_state_.target_texture_ != back_buffer) {
    render_state_.render_target_view_ = nullptr;
  }
  render_state_.target_texture_ = back_buffer;
}

Microsoft::WRL::ComPtr<IDXGIAdapter> D3D11TextureHelper::GetAdapter() {
  Microsoft::WRL::ComPtr<IDXGIFactory1> dxgi_factory;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(dxgi_factory.GetAddressOf()));
  if (FAILED(hr))
    return nullptr;
  if (adapter_index_ >= 0) {
    dxgi_factory->EnumAdapters(adapter_index_, adapter.GetAddressOf());
  } else {
    // We don't have a valid adapter index, lets see if we have a valid LUID.
    Microsoft::WRL::ComPtr<IDXGIFactory4> dxgi_factory4;
    hr = dxgi_factory.As(&dxgi_factory4);
    if (FAILED(hr))
      return nullptr;
    dxgi_factory4->EnumAdapterByLuid(adapter_luid_,
                                     IID_PPV_ARGS(adapter.GetAddressOf()));
  }
  return adapter;
}

Microsoft::WRL::ComPtr<ID3D11Device> D3D11TextureHelper::GetDevice() {
  EnsureInitialized();
  return render_state_.d3d11_device_;
}

bool D3D11TextureHelper::EnsureInitialized() {
  if (render_state_.d3d11_device_ &&
      SUCCEEDED(render_state_.d3d11_device_->GetDeviceRemovedReason()))
    return true;  // Already initialized.

  // If we were previously initialized, but lost the device, throw away old
  // state.  This will be initialized lazily as needed.
  render_state_ = {};

  D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_1};
  UINT flags = 0;
  D3D_FEATURE_LEVEL feature_level_out = D3D_FEATURE_LEVEL_11_1;

  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter = GetAdapter();
  if (!adapter) {
    return false;
  }

  Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device;
  HRESULT hr = D3D11CreateDevice(
      adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, NULL, flags, feature_levels,
      arraysize(feature_levels), D3D11_SDK_VERSION, d3d11_device.GetAddressOf(),
      &feature_level_out, render_state_.d3d11_device_context_.GetAddressOf());
  if (SUCCEEDED(hr)) {
    hr = d3d11_device.As(&render_state_.d3d11_device_);
    if (FAILED(hr)) {
      render_state_.d3d11_device_context_ = nullptr;
    }
  }
  return SUCCEEDED(hr);
}

bool D3D11TextureHelper::SetAdapterIndex(int32_t index) {
  adapter_index_ = index;
  return (index >= 0);
}

bool D3D11TextureHelper::SetAdapterLUID(const LUID& luid) {
  adapter_luid_ = luid;
  adapter_index_ = -1;
  return true;
}

}  // namespace device
