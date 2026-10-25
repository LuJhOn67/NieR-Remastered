#include <fstream>

#include "dxvk_instance.h"
#include "dxvk_nier_dlss.h"
#include "dxvk_nier_ngx.h"

#include "../util/config/config.h"
#include "../util/util_env.h"
#include "../util/util_string.h"

#include <nvsdk_ngx_vk.h>

namespace dxvk {

  DxvkNgxProvider DxvkNgxProvider::s_instance;


  static std::string exeDirectory() {
    std::string exe = env::getExePath();
    return exe.substr(0, exe.find_last_of("\\/") + 1u);
  }


  /**
   * \brief NGX discovery info shared by the extension queries
   *
   * NGX logs go to nier_trace\ next to the executable, and the DLSS
   * DLL (nvngx_dlss.dll) is looked up next to the executable.
   */
  struct NierNgxDiscovery {
    std::wstring                    dataPath = str::tows((exeDirectory() + "nier_trace").c_str());
    std::wstring                    dllPath  = str::tows(exeDirectory().c_str());
    const wchar_t*                  paths[1] = { dllPath.c_str() };
    NVSDK_NGX_FeatureCommonInfo     common   = { };
    NVSDK_NGX_FeatureDiscoveryInfo  info     = { };

    NierNgxDiscovery() {
      ::CreateDirectoryA((exeDirectory() + "nier_trace").c_str(), nullptr);

      common.PathListInfo.Path = paths;
      common.PathListInfo.Length = 1u;

      info.SDKVersion = NVSDK_NGX_Version_API;
      info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
      info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
      info.Identifier.v.ProjectDesc.ProjectId = NierNgxProjectId;
      info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
      info.Identifier.v.ProjectDesc.EngineVersion = "1.0";
      info.ApplicationDataPath = dataPath.c_str();
      info.FeatureInfo = &common;
    }
  };


  std::string_view DxvkNgxProvider::getName() {
    return "NVIDIA NGX (NieR mod)";
  }


  DxvkExtensionList DxvkNgxProvider::getInstanceExtensions() {
    std::lock_guard lock(m_mutex);
    return m_insExtensions;
  }


  DxvkExtensionList DxvkNgxProvider::getDeviceExtensions(uint32_t adapterId) {
    std::lock_guard lock(m_mutex);

    if (adapterId < m_devExtensions.size())
      return m_devExtensions[adapterId];

    return DxvkExtensionList();
  }


  void DxvkNgxProvider::initInstanceExtensions() {
    std::lock_guard lock(m_mutex);

    if (m_initializedInsExt)
      return;

    m_initializedInsExt = true;
    m_enabled = Config::getUserConfig().getOption<bool>("dxvk.nierDlss", false);

    if (!m_enabled)
      return;

    NierNgxDiscovery discovery;
    uint32_t count = 0u;
    VkExtensionProperties* props = nullptr;

    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
      &discovery.info, &count, &props);

    if (NVSDK_NGX_FAILED(result)) {
      writeReport(str::format("instance extension query failed: 0x", std::hex, uint32_t(result)));
      m_enabled = false;
      return;
    }

    m_insExtensions.assign(props, props + count);

    for (const auto& ext : m_insExtensions)
      writeReport(str::format("instance extension: ", ext.extensionName));
  }


  void DxvkNgxProvider::initDeviceExtensions(const DxvkInstance* instance) {
    std::lock_guard lock(m_mutex);

    if (m_initializedDevExt || !m_enabled)
      return;

    m_initializedDevExt = true;

    // The instance accessors are not const, but nothing is modified here
    auto dxvkInstance = const_cast<DxvkInstance*>(instance);
    NierNgxDiscovery discovery;

    for (uint32_t i = 0u; i < dxvkInstance->adapterCount(); i++) {
      auto adapter = dxvkInstance->enumAdapters(i);

      uint32_t count = 0u;
      VkExtensionProperties* props = nullptr;

      NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
        dxvkInstance->handle(), adapter->handle(), &discovery.info, &count, &props);

      DxvkExtensionList list;

      if (NVSDK_NGX_SUCCEED(result))
        list.assign(props, props + count);

      writeReport(str::format("adapter ", i, ": ",
        NVSDK_NGX_SUCCEED(result) ? "" : "query failed, ", list.size(), " device extensions"));

      for (const auto& ext : list)
        writeReport(str::format("  device extension: ", ext.extensionName));

      m_devExtensions.push_back(std::move(list));
    }
  }


  void DxvkNgxProvider::writeReport(const std::string& line) const {
    std::ofstream file(exeDirectory() + "nier_trace\\nier_ngx.txt", std::ios::app);
    file << line << "\n";
  }

}
