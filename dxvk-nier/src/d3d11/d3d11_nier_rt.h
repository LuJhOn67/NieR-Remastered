#pragma once

#include <array>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "d3d11_context_state.h"

#include "../dxvk/dxvk_nier_dlss.h"
#include "../dxvk/dxvk_nier_rt.h"

namespace dxvk {

  /**
   * \brief Geometry captured from one depth pre-pass draw (NieR RT mod)
   *
   * Object-space positions, indices and the object-to-world matrix
   * from VS constant buffer slot 11, as consumed by the backend.
   */
  using D3D11RtDraw = DxvkRtInstanceDesc;


  /**
   * \brief Captures static scene geometry for ray tracing (NieR RT mod)
   *
   * NieR renders a depth pre-pass (R32F linear depth + D24S8) in which
   * static meshes use object-space float3 positions and a per-draw world
   * matrix in VS cb11. This class records those draws per frame. Skinned
   * and otherwise unsupported draws are counted and skipped for now.
   *
   * Enabled with \c d3d11.nierRt = True. Statistics are appended to
   * \c nier_trace\\nier_rt_stats.txt next to the executable.
   */
  class D3D11RtCapture {
    using Clock = std::chrono::steady_clock;
  public:

    static D3D11RtCapture& get();

    void setEnabled(bool enabled) {
      m_enabled = enabled;
    }

    bool enabled() const {
      return m_enabled;
    }

    void onDraw(
      const D3D11ContextState&  state,
            bool                indexed,
            uint32_t            count,
            uint32_t            first,
            int32_t             base,
            uint32_t            instances);

    void onPresent();

    /**
     * \brief Takes the draws captured since the last call
     */
    std::vector<D3D11RtDraw> takeDraws();

    /**
     * \brief Sets RTAO options
     *
     * \param [in] mode 0 = off, 1 = replace SSAO, 2 = min with SSAO
     */
    void setRtao(uint32_t mode, float radius, float strength, bool noRays) {
      m_rtaoMode = mode;
      m_rtaoRadius = radius;
      m_rtaoStrength = strength;
      m_rtaoNoRays = noRays;
    }

    /**
     * \brief Checks a just-recorded immediate context draw for RTAO
     *
     * Remembers the inverse view matrix from the shadow mask pass, and
     * returns \c true with filled-in parameters if the draw was the
     * game's raw half-resolution SSAO pass.
     */
    bool checkRtao(
      const D3D11ContextState&  state,
            DxvkNierRtaoParams& params);

    /**
     * \brief Sets DLAA options
     *
     * \param [in] enabled Whether to run DLAA
     * \param [in] preset NVSDK_NGX_DLSS_Hint_Render_Preset value (0 = default)
     */
    void setDlaa(bool enabled, uint32_t preset) {
      m_dlaa = enabled;
      m_dlssPreset = preset;
    }

    bool dlaaEnabled() const {
      return m_dlaa;
    }

    /**
     * \brief Checks an immediate context draw before it is recorded
     *
     * At the first depth pre-pass draw of a frame, captures the camera
     * and jitters the projection. Returns \c true with the DLAA job
     * filled in right before the game's first bloom downsample draw,
     * which reads the finished HDR scene colour.
     */
    bool preDraw(
      const D3D11ContextState&  state,
            Rc<DxvkImage>&      color,
            Rc<DxvkImage>&      depth,
            Rc<DxvkImage>&      linearDepth,
            DxvkNierDlaaParams& params);

  private:

    enum class PsRole : uint8_t {
      None,
      ShadowMask,
      Ssao,
      Bloom,
    };

    // DLAA state, touched by the immediate context only
    bool                  m_dlaa          = false;
    uint32_t              m_dlssPreset    = 0u;
    bool                  m_haveCamera    = false;
    bool                  m_havePrev      = false;
    bool                  m_dlaaDone      = false;
    bool                  m_dlaaReset     = false;
    bool                  m_haveDepthScale = false;
    std::array<float, 16> m_view          = { };
    std::array<float, 16> m_proj          = { };
    std::array<float, 16> m_prevView      = { };
    std::array<float, 16> m_prevProj      = { };
    std::array<float, 2>  m_depthScale    = { };
    std::array<float, 2>  m_jitter        = { };
    uint32_t              m_jitterIndex   = 0u;
    uint32_t              m_renderWidth   = 0u;
    uint32_t              m_renderHeight  = 0u;
    Rc<DxvkImage>         m_depthImage;
    Rc<DxvkImage>         m_linearDepthImage;
    void*                 m_patchPtr      = nullptr;
    std::array<float, 8>  m_patchOriginal = { };
    std::array<float, 8>  m_patchApplied  = { };
    Clock::time_point     m_lastPresent   = Clock::now();
    float                 m_frameMs       = 16.7f;
    uint64_t              m_dlaaJobs      = 0u;
    uint64_t              m_dlaaNoCamera  = 0u;
    uint32_t              m_dlaaFrames    = 0u;

    void captureCamera(const D3D11ContextState& state);

    void dlaaFrameEnd();

    struct Stats {
      uint32_t frames         = 0u;
      uint64_t prepassDraws   = 0u;
      uint64_t captured       = 0u;
      uint64_t skinned        = 0u;
      uint64_t instanced      = 0u;
      uint64_t notIndexed     = 0u;
      uint64_t badPosition    = 0u;
      uint64_t noMatrix       = 0u;
      uint64_t badMatrix      = 0u;
      uint64_t cbNotMapped    = 0u;
      uint64_t uniqueMeshes   = 0u;
    };

    D3D11RtCapture() = default;

    bool                  m_enabled = false;
    std::mutex            m_mutex;
    std::vector<D3D11RtDraw> m_draws;

    Stats                 m_stats;
    std::unordered_set<uint64_t> m_meshKeys;
    Clock::time_point     m_lastReport = Clock::now();

    uint32_t              m_rtaoMode      = 1u;
    bool                  m_rtaoKeyDown   = false;
    bool                  m_rtaoNoRays    = false;
    float                 m_rtaoRadius    = 1.5f;
    float                 m_rtaoStrength  = 1.0f;

    std::unordered_map<const D3D11CommonShader*, PsRole> m_psRoles;
    std::array<float, 12> m_invView = { };
    bool                  m_hasInvView = false;
    uint64_t              m_rtaoMissingConstants = 0u;

    bool isDepthPrepass(const D3D11ContextState& state) const;

    PsRole getRole(const D3D11CommonShader* shader);

    static bool isSaneTransform(const std::array<float, 12>& m);

    void writeStats();

  };

}
