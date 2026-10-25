#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

#include "d3d11_context_state.h"

namespace dxvk {

  /**
   * \brief Frame structure tracer (NieR RT mod)
   *
   * Records the D3D11 command stream of selected frames - render target
   * changes, draws with their shaders and pixel shader inputs, dispatches,
   * clears and copies - into text files next to the executable, so that
   * the game's render passes can be mapped without an external capture.
   *
   * F9 captures the next frame. In-game frames (many draws) are also
   * captured automatically a few times per session. NIER_TRACE=0 or
   * \c d3d11.nierTrace = False disables the tracer.
   */
  class D3D11FrameTracer {
    using Clock = std::chrono::steady_clock;
  public:

    static D3D11FrameTracer& get();

    void setEnabled(bool enabled) {
      m_enabled = enabled;
    }

    bool recording() const {
      return m_recording.load(std::memory_order_relaxed);
    }

    void countDraw() {
      m_drawCount.fetch_add(1u, std::memory_order_relaxed);
    }

    void onPresent();

    void onDraw(
      const void*               ctx,
            bool                deferred,
      const D3D11ContextState&  state,
      const char*               kind,
            uint32_t            count,
            uint32_t            instances);

    void onDispatch(
      const void*               ctx,
            bool                deferred,
      const D3D11ContextState&  state,
            uint32_t            x,
            uint32_t            y,
            uint32_t            z);

    void onClear(
      const void*               ctx,
      const char*               kind,
            ID3D11View*         view);

    void onCopy(
      const void*               ctx,
      const char*               kind,
            ID3D11Resource*     dst,
            ID3D11Resource*     src);

    void onExecuteCommandList(
      const void*               ctx,
      const void*               list);

  private:

    D3D11FrameTracer();

    bool                  m_enabled = true;
    bool                  m_keyDown = false;
    std::atomic<bool>     m_recording = { false };
    std::atomic<uint32_t> m_drawCount = { 0u };

    std::mutex            m_mutex;
    std::string           m_buffer;
    std::string           m_reason;
    uint32_t              m_lastFrameDraws = 0u;

    uint64_t              m_frameId = 0u;
    uint32_t              m_captureCount = 0u;
    uint32_t              m_autoCount = 0u;
    Clock::time_point     m_start = Clock::now();
    Clock::time_point     m_lastAuto = { };

    std::unordered_map<const void*, uint32_t>     m_ids;
    std::unordered_map<const void*, std::string>  m_passes;

    uint32_t id(const void* object);

    std::string ctxTag(const void* ctx, bool deferred);

    std::string resource(ID3D11Resource* res);

    std::string view(ID3D11View* view);

    std::string shader(const D3D11CommonShader* shader);

    void beginPass(const void* ctx, bool deferred, const D3D11ContextState& state);

    void write();

  };

}
