#pragma once

#include <array>
#include <unordered_map>
#include <vector>

#include "dxvk_buffer.h"

namespace dxvk {

  class DxvkDevice;
  class DxvkPipelineLayout;

  /**
   * \brief One captured draw to place in the ray tracing scene (NieR RT mod)
   *
   * Object-space triangle geometry plus its object-to-world transform.
   */
  struct DxvkRtInstanceDesc {
    Rc<DxvkBuffer>  vertexBuffer;
    VkDeviceSize    vertexOffset    = 0u;   ///< Byte offset of the position attribute of vertex 0
    uint32_t        vertexStride    = 0u;
    VkFormat        vertexFormat    = VK_FORMAT_UNDEFINED;
    Rc<DxvkBuffer>  indexBuffer;
    VkDeviceSize    indexOffset     = 0u;
    VkIndexType     indexType       = VK_INDEX_TYPE_UINT16;
    uint32_t        indexCount      = 0u;
    uint32_t        firstIndex      = 0u;
    int32_t         baseVertex      = 0;
    std::array<float, 12> transform = { };  ///< Row-major 3x4, same layout as VkTransformMatrixKHR
  };


  /**
   * \brief Acceleration structure with its backing storage
   *
   * Reference counted so that command lists can keep it alive
   * until the GPU is done with it; destroyed with the last reference.
   */
  class DxvkRtAccel : public RcObject {

  public:

    DxvkRtAccel(
            DxvkDevice*                     device,
            Rc<DxvkBuffer>                  storage,
            VkAccelerationStructureTypeKHR  type);

    ~DxvkRtAccel();

    VkAccelerationStructureKHR handle() const {
      return m_handle;
    }

    VkDeviceAddress address() const {
      return m_address;
    }

    VkDeviceSize size() const {
      return m_storage->info().size;
    }

  private:

    Rc<vk::DeviceFn>            m_vkd;
    Rc<DxvkBuffer>              m_storage;
    VkAccelerationStructureKHR  m_handle  = VK_NULL_HANDLE;
    VkDeviceAddress             m_address = 0u;

  };


  /**
   * \brief Identifies one mesh (geometry range) for BLAS caching
   */
  struct DxvkRtBlasKey {
    const DxvkBuffer* vertexBuffer;
    const DxvkBuffer* indexBuffer;
    VkDeviceSize      vertexOffset;
    VkDeviceSize      indexOffset;
    uint32_t          vertexStride;
    uint32_t          firstIndex;
    uint32_t          indexCount;
    int32_t           baseVertex;

    bool operator == (const DxvkRtBlasKey& other) const {
      return vertexBuffer == other.vertexBuffer && indexBuffer == other.indexBuffer
          && vertexOffset == other.vertexOffset && indexOffset == other.indexOffset
          && vertexStride == other.vertexStride && firstIndex == other.firstIndex
          && indexCount == other.indexCount && baseVertex == other.baseVertex;
    }
  };

  struct DxvkRtBlasKeyHash {
    size_t operator () (const DxvkRtBlasKey& k) const;
  };


  /**
   * \brief Cached bottom-level acceleration structure
   *
   * Keeps the source buffers alive so their addresses cannot be
   * reused by another mesh while the cache entry exists.
   */
  struct DxvkRtBlasEntry {
    Rc<DxvkRtAccel> accel;
    Rc<DxvkBuffer>  vertexBuffer;
    Rc<DxvkBuffer>  indexBuffer;
    uint64_t        lastUsed = 0u;
  };


  /**
   * \brief Push constants of the RTAO shader (dxvk_nier_rtao.comp)
   *
   * Camera data comes from the game's SSAO pass constants.
   */
  struct DxvkNierRtaoParams {
    uint64_t  tlas            = 0u;       ///< Filled in by the backend
    float     invView[12]     = { };      ///< View to world, 3 rows
    float     projScale[2]    = { };      ///< Projection [0][0], [1][1]
    float     depthBiasScale[2] = { };    ///< View depth = raw * [1] + [0]
    uint32_t  size[2]         = { };
    float     skyThreshold    = 1.0f;
    float     radius          = 1.5f;
    float     strength        = 1.0f;
    uint32_t  frame           = 0u;       ///< Filled in by the backend
    uint32_t  mode            = 0u;       ///< 0 = replace SSAO, 1 = min with SSAO
  };


  /**
   * \brief Ray tracing scene state, owned by the context (NieR RT mod)
   */
  struct DxvkRtScene {
    ~DxvkRtScene();

    Rc<vk::DeviceFn>  vkd;
    const DxvkPipelineLayout* rtaoLayout = nullptr;
    VkPipeline        rtaoPipeline    = VK_NULL_HANDLE;
    uint64_t          rtaoRuns        = 0u;

    std::unordered_map<DxvkRtBlasKey, DxvkRtBlasEntry, DxvkRtBlasKeyHash> blas;

    Rc<DxvkRtAccel>   tlas;
    uint32_t          tlasCapacity    = 0u;
    uint32_t          tlasInstances   = 0u;

    Rc<DxvkBuffer>    blasScratch;
    Rc<DxvkBuffer>    tlasScratch;

    uint64_t          frame           = 0u;
    VkDeviceSize      blasBytes       = 0u;

    struct {
      uint64_t built        = 0u;
      uint64_t evicted      = 0u;
      uint64_t instances    = 0u;
      uint64_t skipped      = 0u;
      uint32_t frames       = 0u;
    } stats;
  };

}
