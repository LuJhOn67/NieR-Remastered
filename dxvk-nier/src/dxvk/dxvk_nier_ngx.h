#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "dxvk_extension_provider.h"

namespace dxvk {

  class DxvkInstance;

  /**
   * \brief NVIDIA NGX extension provider (NieR mod)
   *
   * Asks NGX which Vulkan instance and device extensions DLSS needs and
   * adds them to the ones DXVK enables. Inactive unless \c dxvk.nierDlss
   * is set, and on adapters where NGX reports no DLSS support.
   */
  class DxvkNgxProvider : public DxvkExtensionProvider {

  public:

    std::string_view getName();

    DxvkExtensionList getInstanceExtensions();

    DxvkExtensionList getDeviceExtensions(
            uint32_t      adapterId);

    void initInstanceExtensions();

    void initDeviceExtensions(
      const DxvkInstance* instance);

    /**
     * \brief Whether DLSS was requested and NGX is usable
     */
    bool enabled() const {
      return m_enabled;
    }

    static DxvkNgxProvider s_instance;

  private:

    dxvk::mutex                     m_mutex;
    bool                            m_enabled = false;
    bool                            m_initializedInsExt = false;
    bool                            m_initializedDevExt = false;

    DxvkExtensionList               m_insExtensions;
    std::vector<DxvkExtensionList>  m_devExtensions;

    void writeReport(const std::string& line) const;

  };

}
