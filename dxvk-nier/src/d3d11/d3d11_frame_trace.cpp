#include <fstream>
#include <sstream>

#include "d3d11_frame_trace.h"

#include "../util/util_env.h"

namespace dxvk {

  // Auto-capture policy: only frames that look like gameplay (lots of draws),
  // not before the player could be in the world, and spread over the session.
  constexpr uint32_t AutoMinDraws     = 800u;
  constexpr uint32_t AutoMaxCaptures  = 6u;
  constexpr auto     AutoFirstDelay   = std::chrono::seconds(120);
  constexpr auto     AutoInterval     = std::chrono::seconds(300);


  D3D11FrameTracer& D3D11FrameTracer::get() {
    static D3D11FrameTracer s_tracer;
    return s_tracer;
  }


  D3D11FrameTracer::D3D11FrameTracer() {
    m_enabled = env::getEnvVar("NIER_TRACE") != "0";
  }


  void D3D11FrameTracer::onPresent() {
    uint32_t draws = m_drawCount.exchange(0u, std::memory_order_relaxed);
    m_frameId += 1u;

    if (!m_enabled)
      return;

    if (recording()) {
      std::lock_guard lock(m_mutex);
      m_recording.store(false, std::memory_order_relaxed);
      m_lastFrameDraws = draws;
      write();
      return;
    }

    // Edge-detect on the "is down" bit; the "pressed since last call"
    // bit is unreliable and kept re-triggering captures
    auto now = Clock::now();
    bool keyDown = (::GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    bool key = keyDown && !m_keyDown;
    m_keyDown = keyDown;
    bool autoCapture = draws >= AutoMinDraws
      && m_autoCount < AutoMaxCaptures
      && now - m_start >= AutoFirstDelay
      && (!m_autoCount || now - m_lastAuto >= AutoInterval);

    if (!key && !autoCapture)
      return;

    std::lock_guard lock(m_mutex);

    if (autoCapture && !key) {
      m_autoCount += 1u;
      m_lastAuto = now;
    }

    m_reason = key ? "F9" : "auto";
    m_buffer.clear();
    m_passes.clear();
    m_recording.store(true, std::memory_order_relaxed);
  }


  void D3D11FrameTracer::onDraw(
    const void*               ctx,
          bool                deferred,
    const D3D11ContextState&  state,
    const char*               kind,
          uint32_t            count,
          uint32_t            instances) {
    std::lock_guard lock(m_mutex);

    if (!recording())
      return;

    beginPass(ctx, deferred, state);

    std::ostringstream s;
    s << "  " << kind << " n=" << count;

    if (instances != 1u)
      s << " inst=" << instances;

    s << " vs=" << shader(state.vs.ptr() ? state.vs->GetCommonShader() : nullptr)
      << " ps=" << shader(state.ps.ptr() ? state.ps->GetCommonShader() : nullptr);

    const auto& srvs = state.srv[D3D11ShaderType::ePixel].views;

    for (uint32_t i = 0u; i < 16u; i++) {
      if (srvs[i].ptr())
        s << " t" << i << "=" << view(srvs[i].ptr());
    }

    m_buffer += s.str();
    m_buffer += '\n';
  }


  void D3D11FrameTracer::onDispatch(
    const void*               ctx,
          bool                deferred,
    const D3D11ContextState&  state,
          uint32_t            x,
          uint32_t            y,
          uint32_t            z) {
    std::lock_guard lock(m_mutex);

    if (!recording())
      return;

    std::ostringstream s;
    s << ctxTag(ctx, deferred) << "DISPATCH " << x << "x" << y << "x" << z
      << " cs=" << shader(state.cs.ptr() ? state.cs->GetCommonShader() : nullptr);

    const auto& srvs = state.srv[D3D11ShaderType::eCompute].views;

    for (uint32_t i = 0u; i < 16u; i++) {
      if (srvs[i].ptr())
        s << " t" << i << "=" << view(srvs[i].ptr());
    }

    const auto& uavs = state.uav.views;

    for (uint32_t i = 0u; i < 8u; i++) {
      if (uavs[i].ptr())
        s << " u" << i << "=" << view(uavs[i].ptr());
    }

    m_buffer += s.str();
    m_buffer += '\n';
    m_passes.erase(ctx);
  }


  void D3D11FrameTracer::onClear(
    const void*               ctx,
    const char*               kind,
          ID3D11View*         target) {
    std::lock_guard lock(m_mutex);

    if (!recording())
      return;

    m_buffer += ctxTag(ctx, false) + kind + " " + view(target) + "\n";
  }


  void D3D11FrameTracer::onCopy(
    const void*               ctx,
    const char*               kind,
          ID3D11Resource*     dst,
          ID3D11Resource*     src) {
    std::lock_guard lock(m_mutex);

    if (!recording())
      return;

    m_buffer += ctxTag(ctx, false) + kind + " " + resource(src) + " -> " + resource(dst) + "\n";
  }


  void D3D11FrameTracer::onExecuteCommandList(
    const void*               ctx,
    const void*               list) {
    std::lock_guard lock(m_mutex);

    if (!recording())
      return;

    m_buffer += ctxTag(ctx, false) + "EXECUTE list#" + std::to_string(id(list)) + "\n";
    m_passes.erase(ctx);
  }


  uint32_t D3D11FrameTracer::id(const void* object) {
    auto entry = m_ids.emplace(object, uint32_t(m_ids.size() + 1u));
    return entry.first->second;
  }


  std::string D3D11FrameTracer::ctxTag(const void* ctx, bool deferred) {
    return deferred ? "[def#" + std::to_string(id(ctx)) + "] " : std::string();
  }


  std::string D3D11FrameTracer::resource(ID3D11Resource* res) {
    if (!res)
      return "null";

    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);

    std::ostringstream s;
    s << "R" << id(res);

    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
      D3D11_TEXTURE2D_DESC desc = { };
      static_cast<ID3D11Texture2D*>(res)->GetDesc(&desc);
      s << "(" << desc.Width << "x" << desc.Height;

      if (desc.ArraySize > 1u)
        s << "[" << desc.ArraySize << "]";
      if (desc.SampleDesc.Count > 1u)
        s << " ms" << desc.SampleDesc.Count;

      s << " f" << uint32_t(desc.Format) << " m" << desc.MipLevels << ")";
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
      D3D11_TEXTURE3D_DESC desc = { };
      static_cast<ID3D11Texture3D*>(res)->GetDesc(&desc);
      s << "(" << desc.Width << "x" << desc.Height << "x" << desc.Depth << " f" << uint32_t(desc.Format) << ")";
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
      D3D11_BUFFER_DESC desc = { };
      static_cast<ID3D11Buffer*>(res)->GetDesc(&desc);
      s << "(buf " << desc.ByteWidth << ")";
    }

    return s.str();
  }


  std::string D3D11FrameTracer::view(ID3D11View* target) {
    if (!target)
      return "null";

    Com<ID3D11Resource> res;
    target->GetResource(&res);
    return resource(res.ptr());
  }


  std::string D3D11FrameTracer::shader(const D3D11CommonShader* shader) {
    return shader ? shader->GetName() : std::string("-");
  }


  void D3D11FrameTracer::beginPass(const void* ctx, bool deferred, const D3D11ContextState& state) {
    std::ostringstream s;

    for (uint32_t i = 0u; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
      if (state.om.rtvs[i].ptr())
        s << " rt" << i << "=" << view(state.om.rtvs[i].ptr());
    }

    if (state.om.dsv.ptr())
      s << " ds=" << view(state.om.dsv.ptr());

    std::string targets = s.str();
    auto& current = m_passes[ctx];

    if (current != targets) {
      current = targets;
      m_buffer += ctxTag(ctx, deferred) + "PASS" + targets + "\n";
    }
  }


  void D3D11FrameTracer::write() {
    std::string dir = env::getExePath();
    dir = dir.substr(0, dir.find_last_of("\\/") + 1u) + "nier_trace";
    ::CreateDirectoryA(dir.c_str(), nullptr);

    m_captureCount += 1u;

    std::ostringstream name;
    name << dir << "\\frame_" << m_captureCount << "_" << m_frameId << ".txt";

    std::ofstream file(name.str(), std::ios::binary);
    file << "# NieR frame trace: frame " << m_frameId << ", reason " << m_reason
         << ", draws " << m_lastFrameDraws
         << ", seconds " << std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - m_start).count()
         << "\n" << m_buffer;

    m_buffer.clear();
    m_buffer.shrink_to_fit();
    m_passes.clear();

    Logger::info(str::format("NieR frame trace written: ", name.str()));
  }

}
