#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "d3d11_include.h"

namespace dxvk {

  /**
   * \brief Texture replacement (NieR remaster)
   *
   * The game reserves fixed memory for every world file, so bigger loose
   * files fail to load. Instead, textures are swapped when the game creates
   * them: the first mip of the initial data is hashed (see textureKey) and, if
   * nier_textures\<hash>.dds exists next to the exe, that DDS is uploaded
   * instead. remaster/inject.py builds the folder.
   */
  class D3D11NierTextures {

  public:

    /// Data of a replacement texture, kept alive until the texture is initialised
    struct Replacement {
      D3D11_TEXTURE2D_DESC1               desc = { };
      std::vector<char>                   file;
      std::vector<D3D11_SUBRESOURCE_DATA> data;
    };

    static D3D11NierTextures& get();

    /**
     * \brief Looks up a replacement for a texture being created
     *
     * \param [in] desc Texture description from the game
     * \param [in] init Initial data from the game
     * \param [out] out Replacement description and data
     * \returns \c true if the texture is replaced
     */
    bool find(
      const D3D11_TEXTURE2D_DESC1&        desc,
      const D3D11_SUBRESOURCE_DATA*       init,
            Replacement&                  out);

    static uint64_t hash(const void* data, size_t size, uint64_t h = 0xcbf29ce484222325ull);

    /// Diagnostics for textures that flicker: what the game does to textures after creating them
    enum class Event : uint32_t {
      UpdateReplaced,     ///< UpdateSubresource into a replaced texture
      UpdateOther,        ///< UpdateSubresource into any other texture
      CopyFromReplaced,   ///< Copy with a replaced texture as source
      CopyToReplaced,     ///< Copy with a replaced texture as destination
      MinLod,             ///< SetResourceMinLOD (not implemented by DXVK)
      CreateNoData,       ///< BC texture created without initial data
      DestroyReplaced,    ///< Replaced texture destroyed
      Count
    };

    void note(Event event) {
      m_events[uint32_t(event)].fetch_add(1u, std::memory_order_relaxed);
    }

    /// Writes the event counts every 10 seconds, call once per frame
    void onPresent();

    /// Key of a texture: its packed first mip sampled at start, middle and end, plus its size
    static uint64_t textureKey(const char* mip0, size_t size, uint32_t width, uint32_t height);

  private:

    D3D11NierTextures();

    bool load(const std::string& path, const D3D11_TEXTURE2D_DESC1& desc, Replacement& out);

    void report(const std::string& line);

    bool                                    m_enabled = false;
    uint32_t                                m_maxSize = 0u;
    std::string                             m_dir;
    std::unordered_map<uint64_t, std::string> m_files;

    std::mutex                              m_reportMutex;
    std::atomic<uint32_t>                   m_replaced  = { 0u };
    std::atomic<uint32_t>                   m_misses    = { 0u };

    std::array<std::atomic<uint32_t>, uint32_t(Event::Count)> m_events = { };
    uint64_t                                m_lastReport = 0u;

  };

}
