#pragma once

#include <string>
#include <vector>

#include "dxvk_image.h"

namespace dxvk {

  class DxvkPipelineLayout;

  /// Arbitrary but fixed project ID identifying this mod to NGX
  inline constexpr const char* NierNgxProjectId = "4b5e2f8a-9c1d-4e7b-a3f6-2d8c0b9e1a57";

  /**
   * \brief Per-frame DLAA inputs gathered by the D3D11 layer (NieR DLSS mod)
   *
   * The first members are the push constants of dxvk_nier_mvec.comp.
   */
  struct DxvkNierDlaaParams {
    float     viewToPrevClip[16]  = { };    ///< Current view -> previous clip, rows
    float     projScale[2]        = { };    ///< Projection [0][0], [1][1]
    float     depthBiasScale[2]   = { };    ///< View depth = raw * [1] + [0]
    float     jitter[2]           = { };    ///< Pixels
    uint32_t  size[2]             = { };
    // Not passed to the shader
    uint32_t  depthInverted       = 0u;
    uint32_t  reset               = 0u;
    uint32_t  preset              = 0u;     ///< NVSDK_NGX_DLSS_Hint_Render_Preset value
    float     frameTimeMs         = 16.7f;
  };

  /**
   * \brief DLSS state owned by the context (NieR DLSS mod)
   *
   * NGX handles are stored as opaque pointers so that this header
   * does not depend on the NGX SDK.
   */
  struct DxvkNierDlss {
    ~DxvkNierDlss();

    Rc<vk::DeviceFn>          vkd;
    bool                      initTried     = false;
    bool                      initOk        = false;
    void*                     ngxParams     = nullptr;
    void*                     feature       = nullptr;
    uint32_t                  featureWidth  = 0u;
    uint32_t                  featureHeight = 0u;
    uint32_t                  featureFlags  = 0u;
    uint32_t                  featurePreset = 0u;

    Rc<DxvkImage>             mvImage;
    Rc<DxvkImage>             outImage;

    const DxvkPipelineLayout* mvecLayout    = nullptr;
    VkPipeline                mvecPipeline  = VK_NULL_HANDLE;

    /// Plain VkImageViews for NGX. With descriptor heaps, DXVK views for
    /// shader resources have no VkImageView handle, but NGX needs one.
    struct RawView {
      VkImage             image;
      VkFormat            format;
      VkImageAspectFlags  aspects;
      VkImageUsageFlags   usage;
      VkImageView         view;
      uint64_t            lastUsed;
    };

    std::vector<RawView>      rawViews;
    uint64_t                  frame         = 0u;

    VkImageView getRawView(
      const Rc<DxvkImage>&    image,
            VkImageAspectFlags aspects,
            VkImageUsageFlags usage);

    void releaseUnusedRawViews();

    uint64_t                  evaluations   = 0u;
    uint64_t                  failures      = 0u;
    std::string               lastError;
  };

}
