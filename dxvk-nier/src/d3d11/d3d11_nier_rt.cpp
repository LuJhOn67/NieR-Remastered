#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "d3d11_nier_rt.h"

#include "../dxvk/dxvk_nier_settings.h"

#include "../util/util_env.h"

namespace dxvk {

  constexpr uint32_t WorldMatrixSlot = 11u;
  constexpr auto     StatsInterval   = std::chrono::seconds(10);


  D3D11RtCapture& D3D11RtCapture::get() {
    static D3D11RtCapture s_capture;
    return s_capture;
  }


  void D3D11RtCapture::onDraw(
    const D3D11ContextState&  state,
          bool                indexed,
          uint32_t            count,
          uint32_t            first,
          int32_t             base,
          uint32_t            instances) {
    if (!isDepthPrepass(state))
      return;

    std::lock_guard lock(m_mutex);
    m_stats.prepassDraws += 1u;

    if (!indexed || !state.ia.indexBuffer.buffer.ptr()) {
      m_stats.notIndexed += 1u;
      return;
    }

    if (instances != 1u) {
      m_stats.instanced += 1u;
      return;
    }

    auto layout = state.ia.inputLayout.ptr();

    if (!layout) {
      m_stats.badPosition += 1u;
      return;
    }

    // Find the position attribute (location 0) and reject skinned
    // vertex formats, which carry integer blend indices
    DxvkVertexAttribute position = { };
    bool hasPosition = false;
    bool skinned = false;

    for (uint32_t i = 0u; i < layout->GetAttributeCount(); i++) {
      DxvkVertexAttribute attr = layout->GetInput(i).attribute();

      if (attr.location == 0u) {
        position = attr;
        hasPosition = true;
      }

      if (attr.format == VK_FORMAT_R8G8B8A8_UINT
       || attr.format == VK_FORMAT_R16G16B16A16_UINT
       || attr.format == VK_FORMAT_R32G32B32A32_UINT)
        skinned = true;
    }

    if (skinned) {
      m_stats.skinned += 1u;
      return;
    }

    VkFormat vertexFormat = VK_FORMAT_UNDEFINED;

    if (hasPosition) {
      if (position.format == VK_FORMAT_R32G32B32_SFLOAT
       || position.format == VK_FORMAT_R32G32B32A32_SFLOAT)
        vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
      else if (position.format == VK_FORMAT_R16G16B16A16_SFLOAT)
        vertexFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    }

    const auto& vb = state.ia.vertexBuffers[position.binding];

    if (vertexFormat == VK_FORMAT_UNDEFINED || !vb.buffer.ptr()) {
      m_stats.badPosition += 1u;
      return;
    }

    const auto& cb = state.cbv[D3D11ShaderType::eVertex].buffers[WorldMatrixSlot];

    if (!cb.buffer.ptr() || cb.constantCount < 3u) {
      m_stats.noMatrix += 1u;
      return;
    }

    // Dynamic constant buffers are host-visible; the current slice holds
    // the matrix the game just wrote for this draw
    auto cbSlice = cb.buffer->GetBuffer()->getSliceInfo(VkDeviceSize(cb.constantOffset) * 16u, 48u);

    if (!cbSlice.mapPtr) {
      m_stats.cbNotMapped += 1u;
      return;
    }

    // Some vertex shaders may use cb11 for something other than a world
    // matrix. A non-finite or degenerate transform would poison the TLAS,
    // so only accept sane affine transforms.
    std::array<float, 12> transform;
    std::memcpy(transform.data(), cbSlice.mapPtr, sizeof(transform));

    if (!isSaneTransform(transform)) {
      m_stats.badMatrix += 1u;
      return;
    }

    if (vb.stride < 12u || vb.stride > 256u || count < 3u || count % 3u) {
      m_stats.badPosition += 1u;
      return;
    }

    D3D11RtDraw draw;
    draw.vertexBuffer = vb.buffer->GetBuffer();
    draw.vertexOffset = VkDeviceSize(vb.offset) + position.offset;
    draw.vertexStride = vb.stride;
    draw.vertexFormat = vertexFormat;
    draw.indexBuffer  = state.ia.indexBuffer.buffer->GetBuffer();
    draw.indexOffset  = state.ia.indexBuffer.offset;
    draw.indexType    = state.ia.indexBuffer.format == DXGI_FORMAT_R32_UINT
      ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    draw.indexCount   = count;
    draw.firstIndex   = first;
    draw.baseVertex   = base;
    draw.transform    = transform;

    uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(draw.vertexBuffer.ptr()))
      ^ (uint64_t(reinterpret_cast<uintptr_t>(draw.indexBuffer.ptr())) << 1u)
      ^ (uint64_t(draw.firstIndex) << 32u) ^ (uint64_t(draw.indexCount) << 8u)
      ^ uint64_t(int64_t(draw.baseVertex) * 0x9e3779b97f4a7c15ull) ^ draw.vertexOffset;

    if (m_meshKeys.insert(key).second)
      m_stats.uniqueMeshes += 1u;

    m_stats.captured += 1u;
    m_draws.push_back(std::move(draw));
  }


  void D3D11RtCapture::onPresent() {
    // The in-game menu can switch DLAA and its preset at any time
    auto& settings = NierModSettings::get();
    bool dlaa = settings.dlssLoaded() && settings.dlssEnabled();

    if (dlaa != m_dlaa) {
      m_dlaa = dlaa;
      m_dlaaReset = true;
    }

    m_dlssPreset = settings.dlssPreset();

    if (m_dlaa)
      dlaaFrameEnd();

    if (!m_enabled)
      return;

    std::lock_guard lock(m_mutex);
    m_stats.frames += 1u;

    // The swap chain hands the draws to the backend right after this; only
    // guard against unbounded growth if presentation stops consuming them
    if (m_draws.size() > 100000u)
      m_draws.clear();

    if (Clock::now() - m_lastReport >= StatsInterval) {
      writeStats();
      m_stats = Stats();
      m_lastReport = Clock::now();
    }
  }


  std::vector<D3D11RtDraw> D3D11RtCapture::takeDraws() {
    std::lock_guard lock(m_mutex);
    return std::exchange(m_draws, { });
  }


  // Row-major 4x4 helpers for the DLAA camera math
  using Mat4 = std::array<float, 16>;

  static Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r = { };

    for (uint32_t i = 0u; i < 4u; i++) {
      for (uint32_t j = 0u; j < 4u; j++) {
        for (uint32_t k = 0u; k < 4u; k++)
          r[i * 4u + j] += a[i * 4u + k] * b[k * 4u + j];
      }
    }

    return r;
  }


  static bool invert(const Mat4& m, Mat4& out) {
    // Gauss-Jordan elimination with partial pivoting
    std::array<double, 32> a;

    for (uint32_t i = 0u; i < 4u; i++) {
      for (uint32_t j = 0u; j < 4u; j++) {
        a[i * 8u + j] = m[i * 4u + j];
        a[i * 8u + 4u + j] = i == j ? 1.0 : 0.0;
      }
    }

    for (uint32_t c = 0u; c < 4u; c++) {
      uint32_t pivot = c;

      for (uint32_t r = c + 1u; r < 4u; r++) {
        if (std::abs(a[r * 8u + c]) > std::abs(a[pivot * 8u + c]))
          pivot = r;
      }

      if (std::abs(a[pivot * 8u + c]) < 1.0e-12)
        return false;

      for (uint32_t j = 0u; j < 8u; j++)
        std::swap(a[c * 8u + j], a[pivot * 8u + j]);

      double inv = 1.0 / a[c * 8u + c];

      for (uint32_t j = 0u; j < 8u; j++)
        a[c * 8u + j] *= inv;

      for (uint32_t r = 0u; r < 4u; r++) {
        if (r == c)
          continue;

        double f = a[r * 8u + c];

        for (uint32_t j = 0u; j < 8u; j++)
          a[r * 8u + j] -= f * a[c * 8u + j];
      }
    }

    for (uint32_t i = 0u; i < 4u; i++) {
      for (uint32_t j = 0u; j < 4u; j++)
        out[i * 4u + j] = float(a[i * 8u + 4u + j]);
    }

    return true;
  }


  static float halton(uint32_t index, uint32_t base) {
    float f = 1.0f, r = 0.0f;

    while (index) {
      f /= float(base);
      r += f * float(index % base);
      index /= base;
    }

    return r;
  }


  bool D3D11RtCapture::preDraw(
    const D3D11ContextState&  state,
          Rc<DxvkImage>&      color,
          Rc<DxvkImage>&      depth,
          Rc<DxvkImage>&      linearDepth,
          DxvkNierDlaaParams& params) {
    if (!m_haveCamera && isDepthPrepass(state))
      captureCamera(state);

    if (!state.ps.ptr())
      return false;

    PsRole role = getRole(state.ps->GetCommonShader());

    if (role == PsRole::Ssao) {
      std::array<float, 4> cb13;
      const auto& cb = state.cbv[D3D11ShaderType::ePixel].buffers[13];

      if (cb.buffer.ptr() && cb.constantCount >= 1u) {
        auto slice = cb.buffer->GetBuffer()->getSliceInfo(VkDeviceSize(cb.constantOffset) * 16u, 16u);

        if (slice.mapPtr) {
          std::memcpy(cb13.data(), slice.mapPtr, sizeof(cb13));
          m_depthScale = { cb13[0], cb13[1] };
          m_haveDepthScale = true;
        }
      }

      return false;
    }

    if (role != PsRole::Bloom || m_dlaaDone)
      return false;

    m_dlaaDone = true;

    if (!m_haveCamera || !m_havePrev || !m_haveDepthScale) {
      m_dlaaNoCamera += 1u;
      return false;
    }

    // Only the first, full-resolution downsample reads the scene colour
    auto srv = state.srv[D3D11ShaderType::ePixel].views[0].ptr();

    if (!srv)
      return false;

    auto image = Rc<DxvkImage>(srv->GetImageView()->image());

    if (image->info().extent.width != m_renderWidth || image->info().extent.height != m_renderHeight)
      return false;

    Mat4 invView;

    if (!invert(m_view, invView))
      return false;

    Mat4 viewToPrevClip = mul(mul(m_prevProj, m_prevView), invView);
    std::memcpy(params.viewToPrevClip, viewToPrevClip.data(), sizeof(params.viewToPrevClip));

    params.projScale[0] = m_proj[0];
    params.projScale[1] = m_proj[5];
    params.depthBiasScale[0] = m_depthScale[0];
    params.depthBiasScale[1] = m_depthScale[1];
    params.jitter[0] = m_jitter[0];
    params.jitter[1] = m_jitter[1];

    // Reversed depth if a near point maps to a larger NDC depth than a far one
    auto ndcZ = [this] (float viewZ) {
      float z = m_proj[8] * 0.0f + m_proj[9] * 0.0f + m_proj[10] * viewZ + m_proj[11];
      float w = m_proj[12] * 0.0f + m_proj[13] * 0.0f + m_proj[14] * viewZ + m_proj[15];
      return z / w;
    };

    params.depthInverted = ndcZ(-1.0f) > ndcZ(-1000.0f) ? 1u : 0u;
    params.preset = m_dlssPreset;
    params.reset = std::exchange(m_dlaaReset, false) ? 1u : 0u;
    params.frameTimeMs = m_frameMs;

    color = std::move(image);
    depth = m_depthImage;
    linearDepth = m_linearDepthImage;
    m_dlaaJobs += 1u;
    return true;
  }


  void D3D11RtCapture::captureCamera(const D3D11ContextState& state) {
    const auto& cb = state.cbv[D3D11ShaderType::eVertex].buffers[0];

    if (!cb.buffer.ptr() || cb.constantCount < 8u)
      return;

    auto slice = cb.buffer->GetBuffer()->getSliceInfo(VkDeviceSize(cb.constantOffset) * 16u, 128u);

    if (!slice.mapPtr)
      return;

    auto rows = static_cast<float*>(slice.mapPtr);

    // A slice the game did not rewrite still holds last frame's jittered
    // projection rows; restore them so jitter does not accumulate
    if (slice.mapPtr == m_patchPtr && !std::memcmp(rows + 16, m_patchApplied.data(), sizeof(m_patchApplied)))
      std::memcpy(rows + 16, m_patchOriginal.data(), sizeof(m_patchOriginal));

    std::memcpy(m_view.data(), rows, sizeof(m_view));
    std::memcpy(m_proj.data(), rows + 16, sizeof(m_proj));

    Com<ID3D11Resource> resource;
    state.om.rtvs[0]->GetResource(&resource);

    D3D11_TEXTURE2D_DESC desc = { };
    static_cast<ID3D11Texture2D*>(resource.ptr())->GetDesc(&desc);

    m_renderWidth = desc.Width;
    m_renderHeight = desc.Height;

    // DLAA cannot run on multisampled targets; jittering would only shake the image
    if (desc.SampleDesc.Count > 1u) {
      m_patchPtr = nullptr;
      return;
    }

    // Jitter the projection: clip.xy += jitter_ndc * clip.w (row 7 produces w)
    float jx = 2.0f * m_jitter[0] / float(desc.Width);
    float jy = -2.0f * m_jitter[1] / float(desc.Height);

    std::memcpy(m_patchOriginal.data(), rows + 16, sizeof(m_patchOriginal));

    for (uint32_t i = 0u; i < 4u; i++) {
      rows[16u + i] += jx * rows[28u + i];
      rows[20u + i] += jy * rows[28u + i];
    }

    std::memcpy(m_patchApplied.data(), rows + 16, sizeof(m_patchApplied));
    m_patchPtr = slice.mapPtr;

    m_depthImage = Rc<DxvkImage>(state.om.dsv->GetImageView()->image());
    m_linearDepthImage = Rc<DxvkImage>(state.om.rtvs[0]->GetImageView()->image());
    m_haveCamera = true;
  }


  void D3D11RtCapture::dlaaFrameEnd() {
    auto now = Clock::now();
    m_frameMs = std::chrono::duration<float, std::milli>(now - m_lastPresent).count();
    m_lastPresent = now;

    m_havePrev = m_haveCamera;

    if (m_haveCamera) {
      m_prevView = m_view;
      m_prevProj = m_proj;
    }

    m_haveCamera = false;
    m_dlaaDone = false;

    // 8-phase Halton(2,3) sequence, centred on the pixel
    m_jitterIndex = (m_jitterIndex + 1u) % 8u;
    m_jitter = { halton(m_jitterIndex + 1u, 2u) - 0.5f, halton(m_jitterIndex + 1u, 3u) - 0.5f };

    if (++m_dlaaFrames >= 600u) {
      std::string path = env::getExePath();
      path = path.substr(0, path.find_last_of("\\/") + 1u) + "nier_trace";
      ::CreateDirectoryA(path.c_str(), nullptr);

      std::ofstream file(path + "\\nier_dlaa_d3d11.txt", std::ios::app);
      file << "frames " << m_dlaaFrames << " | DLAA jobs " << m_dlaaJobs
           << ", skipped (no camera/depth scale) " << m_dlaaNoCamera
           << " | jittered camera slice " << (m_patchPtr ? "yes" : "no (MSAA on or not mapped)") << "\n";

      m_dlaaFrames = 0u;
      m_dlaaJobs = 0u;
      m_dlaaNoCamera = 0u;
    }
  }


  bool D3D11RtCapture::isSaneTransform(const std::array<float, 12>& m) {
    for (float v : m) {
      if (!std::isfinite(v) || std::abs(v) > 1.0e6f)
        return false;
    }

    // Rows are (x, y, z, translation); the 3x3 part must not collapse space
    float det = m[0] * (m[5] * m[10] - m[6] * m[9])
              - m[1] * (m[4] * m[10] - m[6] * m[8])
              + m[2] * (m[4] * m[9]  - m[5] * m[8]);

    return std::abs(det) > 1.0e-9f;
  }


  bool D3D11RtCapture::checkRtao(
    const D3D11ContextState&  state,
          DxvkNierRtaoParams& params) {
    if (!state.ps.ptr())
      return false;

    PsRole role = getRole(state.ps->GetCommonShader());

    if (role == PsRole::None)
      return false;

    // F10 cycles off -> replace -> min, for in-game comparisons. Polled once
    // per frame here since the SSAO pass runs once per frame.
    if (role == PsRole::Ssao) {
      bool keyDown = (::GetAsyncKeyState(VK_F10) & 0x8000) != 0;

      if (keyDown && !m_rtaoKeyDown)
        m_rtaoMode = (m_rtaoMode + 1u) % 3u;

      m_rtaoKeyDown = keyDown;
    }

    if (!m_rtaoMode)
      return false;

    // Reads rows of a pixel shader constant buffer from its current mapped slice
    auto readRows = [&state] (uint32_t slot, uint32_t first, uint32_t count, float* dst) {
      const auto& cb = state.cbv[D3D11ShaderType::ePixel].buffers[slot];

      if (!cb.buffer.ptr() || cb.constantCount < first + count)
        return false;

      auto slice = cb.buffer->GetBuffer()->getSliceInfo(
        VkDeviceSize(cb.constantOffset + first) * 16u, VkDeviceSize(count) * 16u);

      if (!slice.mapPtr)
        return false;

      std::memcpy(dst, slice.mapPtr, count * 16u);
      return true;
    };

    // The shadow mask shader is known to read view->world from cb0 rows 12-14
    if (role == PsRole::ShadowMask) {
      m_hasInvView = readRows(0u, 12u, 3u, m_invView.data());
      return false;
    }

    std::array<float, 4> row4, row5, cb13, cb8;

    if (!readRows(0u, 4u, 1u, row4.data()) || !readRows(0u, 5u, 1u, row5.data())
     || !readRows(13u, 0u, 1u, cb13.data()) || !readRows(8u, 3u, 1u, cb8.data())) {
      m_rtaoMissingConstants += 1u;
      return false;
    }

    if (m_hasInvView)
      std::memcpy(params.invView, m_invView.data(), sizeof(params.invView));
    else if (!readRows(0u, 12u, 3u, params.invView)) {
      m_rtaoMissingConstants += 1u;
      return false;
    }

    if (!state.om.rtvs[0].ptr())
      return false;

    Com<ID3D11Resource> resource;
    state.om.rtvs[0]->GetResource(&resource);

    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dim);

    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
      return false;

    D3D11_TEXTURE2D_DESC desc = { };
    static_cast<ID3D11Texture2D*>(resource.ptr())->GetDesc(&desc);

    params.projScale[0] = row4[0];
    params.projScale[1] = row5[1];
    params.depthBiasScale[0] = cb13[0];
    params.depthBiasScale[1] = cb13[1];
    params.skyThreshold = cb8[0];
    params.size[0] = desc.Width;
    params.size[1] = desc.Height;
    params.radius = m_rtaoRadius;
    params.strength = m_rtaoStrength;
    params.mode = (m_rtaoMode - 1u) | (m_rtaoNoRays ? 16u : 0u);
    return true;
  }


  D3D11RtCapture::PsRole D3D11RtCapture::getRole(const D3D11CommonShader* shader) {
    auto entry = m_psRoles.find(shader);

    if (entry != m_psRoles.end())
      return entry->second;

    // Shader names are DXBC hashes, see traces/gameplay-1/FINDINGS.md
    std::string name = shader->GetName();
    PsRole role = PsRole::None;

    if (name == "fs.c2570c6a8bdef9c634335a674677d3d3")
      role = PsRole::Ssao;
    else if (name == "fs.096e7389bcb3fd0ef4ef37da4cc1e426")
      role = PsRole::ShadowMask;
    else if (name == "fs.2283dedc5b5c4968dee5cc089267b9db")
      role = PsRole::Bloom;

    m_psRoles.emplace(shader, role);
    return role;
  }


  bool D3D11RtCapture::isDepthPrepass(const D3D11ContextState& state) const {
    if (!state.om.rtvs[0].ptr() || !state.om.dsv.ptr())
      return false;

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = { };
    state.om.rtvs[0]->GetDesc(&rtvDesc);

    if (rtvDesc.Format != DXGI_FORMAT_R32_FLOAT)
      return false;

    // The shadow pass also renders to R32F, but with a typeless 32-bit depth buffer
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = { };
    state.om.dsv->GetDesc(&dsvDesc);
    return dsvDesc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT;
  }


  void D3D11RtCapture::writeStats() {
    std::string path = env::getExePath();
    path = path.substr(0, path.find_last_of("\\/") + 1u) + "nier_trace";
    ::CreateDirectoryA(path.c_str(), nullptr);

    std::ofstream file(path + "\\nier_rt_stats.txt", std::ios::app);
    uint32_t f = std::max(m_stats.frames, 1u);

    file << "frames " << m_stats.frames
         << " | per frame: prepass " << m_stats.prepassDraws / f
         << ", captured " << m_stats.captured / f
         << ", skinned " << m_stats.skinned / f
         << ", instanced " << m_stats.instanced / f
         << ", not indexed " << m_stats.notIndexed / f
         << ", bad position " << m_stats.badPosition / f
         << ", no matrix " << m_stats.noMatrix / f
         << ", bad matrix " << m_stats.badMatrix / f
         << ", cb not mapped " << m_stats.cbNotMapped / f
         << " | new unique meshes " << m_stats.uniqueMeshes
         << " (session " << m_meshKeys.size() << ")"
         << " | RTAO inv view " << (m_hasInvView ? "yes" : "no")
         << ", missing constants " << m_rtaoMissingConstants << "\n";
  }

}
