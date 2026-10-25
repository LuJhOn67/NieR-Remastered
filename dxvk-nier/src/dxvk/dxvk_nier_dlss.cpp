#include <cstddef>
#include <cstring>
#include <fstream>
#include <intrin.h>

#include "dxvk_context.h"
#include "dxvk_device.h"
#include "dxvk_nier_dlss.h"
#include "dxvk_nier_settings.h"

#include "../util/util_env.h"
#include "../util/util_string.h"

#include <dxvk_nier_mvec.h>

#ifdef DXVK_NIER_NGX
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#endif

namespace dxvk {

  static std::string nierExeDirectory() {
    std::string exe = env::getExePath();
    return exe.substr(0, exe.find_last_of("\\/") + 1u);
  }


  static void dlssReport(const std::string& line) {
    ::CreateDirectoryA((nierExeDirectory() + "nier_trace").c_str(), nullptr);
    std::ofstream file(nierExeDirectory() + "nier_trace\\nier_dlss.txt", std::ios::app);
    file << line << "\n";
  }


  VkImageView DxvkNierDlss::getRawView(
    const Rc<DxvkImage>&    image,
          VkImageAspectFlags aspects,
          VkImageUsageFlags usage) {
    VkImage handle = image->handle();
    VkFormat format = image->info().format;

    for (auto& v : rawViews) {
      if (v.image == handle && v.format == format && v.aspects == aspects && v.usage == usage) {
        v.lastUsed = frame;
        return v.view;
      }
    }

    VkImageViewUsageCreateInfo usageInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
    usageInfo.usage = usage;

    VkImageViewCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &usageInfo };
    info.image = handle;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = { aspects, 0u, 1u, 0u, 1u };

    VkImageView view = VK_NULL_HANDLE;

    if (vkd->vkCreateImageView(vkd->device(), &info, nullptr, &view) != VK_SUCCESS)
      return VK_NULL_HANDLE;

    rawViews.push_back({ handle, format, aspects, usage, view, frame });
    return view;
  }


  void DxvkNierDlss::releaseUnusedRawViews() {
    // Views of images that were recreated stop being used; destroy them once
    // they cannot be referenced by in-flight command buffers any more
    for (auto i = rawViews.begin(); i != rawViews.end(); ) {
      if (frame - i->lastUsed > 16u) {
        vkd->vkDestroyImageView(vkd->device(), i->view, nullptr);
        i = rawViews.erase(i);
      } else {
        i++;
      }
    }
  }


  DxvkNierDlss::~DxvkNierDlss() {
    for (const auto& v : rawViews)
      vkd->vkDestroyImageView(vkd->device(), v.view, nullptr);

    if (mvecPipeline)
      vkd->vkDestroyPipeline(vkd->device(), mvecPipeline, nullptr);

#ifdef DXVK_NIER_NGX
    if (feature)
      NVSDK_NGX_VULKAN_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(feature));

    if (ngxParams)
      NVSDK_NGX_VULKAN_DestroyParameters(static_cast<NVSDK_NGX_Parameter*>(ngxParams));

    if (initOk)
      NVSDK_NGX_VULKAN_Shutdown1(vkd->device());
#endif
  }


#ifdef DXVK_NIER_NGX

  // Diagnostics: NGX gets its Vulkan entry points through these wrappers so
  // that image view handle queries with a null view can be traced to a caller
  static PFN_vkGetInstanceProcAddr    g_realGipa = nullptr;
  static PFN_vkGetDeviceProcAddr      g_realGdpa = nullptr;
  static PFN_vkGetImageViewHandleNVX  g_realGetImageViewHandle = nullptr;
  static uint32_t                     g_viewHandleCalls = 0u;
  static uint32_t                     g_viewHandleNulls = 0u;

  static std::string callerModule(void* address) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = { };

    if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(address), &module))
      ::GetModuleFileNameA(module, name, MAX_PATH);

    return str::format(name, " +0x", std::hex,
      reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module));
  }

  static uint32_t VKAPI_CALL nierGetImageViewHandle(VkDevice device, const VkImageViewHandleInfoNVX* pInfo) {
    g_viewHandleCalls += 1u;

    if (!pInfo || !pInfo->imageView) {
      if (++g_viewHandleNulls <= 8u) {
        dlssReport(str::format("NULL image view handle query #", g_viewHandleCalls,
          " descriptorType ", pInfo ? uint32_t(pInfo->descriptorType) : ~0u,
          " sampler ", pInfo ? uint64_t(pInfo->sampler) : 0ull,
          " from ", callerModule(_ReturnAddress())));
      }

      // Forwarding a null view crashes the driver
      return 0u;
    }

    if (g_viewHandleCalls <= 16u) {
      dlssReport(str::format("image view handle query #", g_viewHandleCalls,
        " view 0x", std::hex, uint64_t(pInfo->imageView), std::dec,
        " descriptorType ", uint32_t(pInfo->descriptorType),
        " from ", callerModule(_ReturnAddress())));
    }

    return g_realGetImageViewHandle(device, pInfo);
  }

  static PFN_vkVoidFunction VKAPI_CALL nierGetDeviceProcAddr(VkDevice device, const char* name) {
    PFN_vkVoidFunction fn = g_realGdpa(device, name);

    if (fn && !std::strcmp(name, "vkGetImageViewHandleNVX")) {
      g_realGetImageViewHandle = reinterpret_cast<PFN_vkGetImageViewHandleNVX>(fn);
      return reinterpret_cast<PFN_vkVoidFunction>(&nierGetImageViewHandle);
    }

    return fn;
  }

  static PFN_vkVoidFunction VKAPI_CALL nierGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!std::strcmp(name, "vkGetDeviceProcAddr"))
      return reinterpret_cast<PFN_vkVoidFunction>(&nierGetDeviceProcAddr);

    return g_realGipa(instance, name);
  }


  static Rc<DxvkImage> createNierImage(
          DxvkDevice*         device,
          VkFormat            format,
          uint32_t            width,
          uint32_t            height,
          VkImageUsageFlags   usage,
    const char*               name) {
    DxvkImageCreateInfo info = { };
    info.type = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    info.extent = { width, height, 1u };
    info.numLayers = 1u;
    info.mipLevels = 1u;
    info.usage = usage;
    info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.layout = VK_IMAGE_LAYOUT_GENERAL;
    info.debugName = name;
    return device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  }


  static Rc<DxvkImageView> createNierView(
    const Rc<DxvkImage>&      image,
          VkImageUsageFlags   usage,
          VkImageAspectFlags  aspects) {
    DxvkImageViewKey key = { };
    key.viewType = VK_IMAGE_VIEW_TYPE_2D;
    key.usage = usage;
    key.format = image->info().format;
    key.aspects = aspects;
    key.layout = usage == VK_IMAGE_USAGE_STORAGE_BIT
      ? VK_IMAGE_LAYOUT_GENERAL
      : image->pickLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    key.mipIndex = 0u;
    key.mipCount = 1u;
    key.layerIndex = 0u;
    key.layerCount = 1u;
    return image->createView(key);
  }


  static NVSDK_NGX_Resource_VK ngxResource(
          DxvkNierDlss&       s,
    const Rc<DxvkImage>&      image,
          VkImageAspectFlags  aspects,
          bool                readWrite) {
    VkImageSubresourceRange range = { aspects, 0u, 1u, 0u, 1u };
    VkImageView view = s.getRawView(image, aspects,
      readWrite ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT);

    return NVSDK_NGX_Create_ImageView_Resource_VK(view, image->handle(), range,
      image->info().format, image->info().extent.width, image->info().extent.height, readWrite);
  }

#endif


  void DxvkContext::nierAfterExternalCommands() {
    // Same as beginCurrentCommands, without switching command buffers:
    // splitting the command list around NGX submitted a command buffer twice
    m_flags.clr(
      DxvkContextFlag::GpRenderPassActive,
      DxvkContextFlag::GpXfbActive,
      DxvkContextFlag::GpIndependentSets,
      DxvkContextFlag::CpComputePassActive);

    m_flags.set(
      DxvkContextFlag::GpDirtyRenderTargets,
      DxvkContextFlag::GpDirtyPipeline,
      DxvkContextFlag::GpDirtyPipelineState,
      DxvkContextFlag::GpDirtyVertexBuffers,
      DxvkContextFlag::GpDirtyIndexBuffer,
      DxvkContextFlag::GpDirtyXfbBuffers,
      DxvkContextFlag::GpDirtyBlendConstants,
      DxvkContextFlag::GpDirtyStencilTest,
      DxvkContextFlag::GpDirtyStencilRef,
      DxvkContextFlag::GpDirtyMultisampleState,
      DxvkContextFlag::GpDirtyRasterizerState,
      DxvkContextFlag::GpDirtySampleLocations,
      DxvkContextFlag::GpDirtyViewport,
      DxvkContextFlag::GpDirtyDepthBias,
      DxvkContextFlag::GpDirtyDepthBounds,
      DxvkContextFlag::GpDirtyDepthClip,
      DxvkContextFlag::GpDirtyDepthTest,
      DxvkContextFlag::CpDirtyPipelineState,
      DxvkContextFlag::DirtyDrawBuffer);

    m_descriptorState.dirtyStages(
      VK_SHADER_STAGE_ALL_GRAPHICS |
      VK_SHADER_STAGE_COMPUTE_BIT);

    m_state.gp.pipeline = nullptr;
    m_state.cp.pipeline = nullptr;
  }


  void DxvkContext::nierDlaa(
    const Rc<DxvkImage>&              color,
    const Rc<DxvkImage>&              depth,
    const Rc<DxvkImage>&              linearDepth,
          DxvkNierDlaaParams          params) {
#ifdef DXVK_NIER_NGX
    if (!m_nierDlss) {
      m_nierDlss = std::make_unique<DxvkNierDlss>();
      m_nierDlss->vkd = m_device->vkd();
    }

    auto& s = *m_nierDlss;
    auto vk = m_device->vkd();

    if (color->info().sampleCount != VK_SAMPLE_COUNT_1_BIT
     || depth->info().sampleCount != VK_SAMPLE_COUNT_1_BIT) {
      if (s.lastError != "msaa") {
        s.lastError = "msaa";
        dlssReport("DLAA skipped: the game renders with MSAA, set Anti-Aliasing to Off in the game's graphics settings");
      }
      NierModSettings::get().setDlssStatus(NierDlssStatus::NeedAaOff);
      return;
    }

    uint32_t width = color->info().extent.width;
    uint32_t height = color->info().extent.height;

    this->endCurrentPass(true);

    // One-time NGX initialisation on the game's Vulkan device
    if (!s.initTried) {
      s.initTried = true;

      std::wstring dataPath = str::tows((nierExeDirectory() + "nier_trace").c_str());
      std::wstring dllPath = str::tows(nierExeDirectory().c_str());
      const wchar_t* paths[1] = { dllPath.c_str() };

      NVSDK_NGX_FeatureCommonInfo common = { };
      common.PathListInfo.Path = paths;
      common.PathListInfo.Length = 1u;

      g_realGipa = m_device->instance()->vki()->getLoaderProc();
      g_realGdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        g_realGipa(m_device->instance()->handle(), "vkGetDeviceProcAddr"));

      NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(NierNgxProjectId,
        NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dataPath.c_str(),
        m_device->instance()->handle(), m_device->adapter()->handle(), vk->device(),
        &nierGetInstanceProcAddr, &nierGetDeviceProcAddr, &common, NVSDK_NGX_Version_API);

      if (NVSDK_NGX_FAILED(result)) {
        dlssReport(str::format("NGX init failed: 0x", std::hex, uint32_t(result)));
        NierModSettings::get().setDlssStatus(NierDlssStatus::Failed);
        return;
      }

      NVSDK_NGX_Parameter* ngxParams = nullptr;
      result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&ngxParams);

      int available = 0;

      if (NVSDK_NGX_SUCCEED(result))
        NVSDK_NGX_Parameter_GetI(ngxParams, NVSDK_NGX_Parameter_SuperSampling_Available, &available);

      if (!available) {
        dlssReport("NGX: DLSS is not available on this GPU/driver");
        NierModSettings::get().setDlssStatus(NierDlssStatus::Unsupported);
        NVSDK_NGX_VULKAN_Shutdown1(vk->device());
        return;
      }

      s.ngxParams = ngxParams;
      s.initOk = true;
      dlssReport("NGX initialised, DLSS available");
    }

    if (!s.initOk)
      return;

    // Our own images: motion vectors and the DLSS output
    if (s.outImage == nullptr || s.outImage->info().extent.width != width
     || s.outImage->info().extent.height != height || s.outImage->info().format != color->info().format) {
      auto features = m_device->adapter()->getFormatFeatures(color->info().format);

      if (!(features.optimal & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT)) {
        dlssReport(str::format("DLAA skipped: no storage support for colour format ", uint32_t(color->info().format)));
        NierModSettings::get().setDlssStatus(NierDlssStatus::Failed);
        s.initOk = false;
        return;
      }

      // NGX clears its output, so it needs transfer dst usage
      s.mvImage = createNierImage(m_device.ptr(), VK_FORMAT_R16G16_SFLOAT, width, height,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        "NieR DLSS motion vectors");
      s.outImage = createNierImage(m_device.ptr(), color->info().format, width, height,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        "NieR DLSS output");

      // Move both out of the undefined layout before NGX sees them
      this->initImage(s.mvImage, VK_IMAGE_LAYOUT_UNDEFINED);
      this->initImage(s.outImage, VK_IMAGE_LAYOUT_UNDEFINED);
    }

    // The game never samples its depth buffer (it reads its own linear depth),
    // so it may lack sampled usage; views created without it are invalid
    DxvkImageUsageInfo sampledUsage = { };
    sampledUsage.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    sampledUsage.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    sampledUsage.access = VK_ACCESS_SHADER_READ_BIT;

    if (!ensureImageCompatibility(depth.ptr(), sampledUsage)
     || !ensureImageCompatibility(linearDepth.ptr(), sampledUsage)
     || !ensureImageCompatibility(color.ptr(), sampledUsage)) {
      if (s.lastError != "usage") {
        s.lastError = "usage";
        dlssReport("DLAA skipped: could not add sampled usage to the game's depth/colour images");
        NierModSettings::get().setDlssStatus(NierDlssStatus::Failed);
      }
      return;
    }

    // Camera motion vectors
    this->invalidateState();

    if (!s.mvecPipeline) {
      std::array<DxvkDescriptorSetLayoutBinding, 2> bindings = {{
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT },
      }};

      s.mvecLayout = m_device->createBuiltInPipelineLayout(DxvkPipelineLayoutFlag::UsesSamplerHeap,
        VK_SHADER_STAGE_COMPUTE_BIT, offsetof(DxvkNierDlaaParams, depthInverted), bindings.size(), bindings.data());

      util::DxvkBuiltInShaderStage shader(dxvk_nier_mvec, nullptr);
      s.mvecPipeline = m_device->createBuiltInComputePipeline(s.mvecLayout, shader);
    }

    auto linearDepthView = createNierView(linearDepth, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    auto mvStorageView = createNierView(s.mvImage, VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    {
      std::array<DxvkDescriptorWrite, 2> descriptors = { };
      descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      descriptors[0].descriptor = linearDepthView->getDescriptor();
      descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      descriptors[1].descriptor = mvStorageView->getDescriptor();

      small_vector<DxvkResourceAccess, 2u> accesses;
      accesses.emplace_back(*linearDepthView, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT, false);
      accesses.emplace_back(*mvStorageView, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_WRITE_BIT, true);

      params.size[0] = width;
      params.size[1] = height;

      m_cmd->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE, s.mvecPipeline);
      m_cmd->bindResources(DxvkCmdBuffer::ExecBuffer, s.mvecLayout,
        descriptors.size(), descriptors.data(),
        offsetof(DxvkNierDlaaParams, depthInverted), &params);

      syncResources(DxvkCmdBuffer::ExecBuffer, accesses.size(), accesses.data());

      m_cmd->cmdDispatch(DxvkCmdBuffer::ExecBuffer, (width + 7u) / 8u, (height + 7u) / 8u, 1u);
    }

    auto ngxParams = static_cast<NVSDK_NGX_Parameter*>(s.ngxParams);

    // (Re)create the DLAA feature when size, flags or preset change
    uint32_t flags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR
                   | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
                   | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

    if (params.depthInverted)
      flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

    if (!s.feature || s.featureWidth != width || s.featureHeight != height
     || s.featureFlags != flags || s.featurePreset != params.preset) {
      if (s.feature) {
        m_device->waitForIdle();
        NVSDK_NGX_VULKAN_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(s.feature));
        s.feature = nullptr;
      }

      NVSDK_NGX_Parameter_SetUI(ngxParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, params.preset);

      NVSDK_NGX_DLSS_Create_Params createParams = { };
      createParams.Feature.InWidth = width;
      createParams.Feature.InHeight = height;
      createParams.Feature.InTargetWidth = width;
      createParams.Feature.InTargetHeight = height;
      createParams.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
      createParams.InFeatureCreateFlags = int(flags);

      // NGX records into the current command buffer; flush our barriers first
      flushBarriers();

      NVSDK_NGX_Handle* handle = nullptr;
      NVSDK_NGX_Result result = NGX_VULKAN_CREATE_DLSS_EXT1(vk->device(),
        m_cmd->getExternalCmdBuffer(DxvkCmdBuffer::ExecBuffer), 1u, 1u, &handle, ngxParams, &createParams);

      // NGX bound its own state
      this->nierAfterExternalCommands();

      if (NVSDK_NGX_FAILED(result)) {
        dlssReport(str::format("DLAA feature creation failed: 0x", std::hex, uint32_t(result)));
        NierModSettings::get().setDlssStatus(NierDlssStatus::Failed);
        s.initOk = false;
        return;
      }

      s.feature = handle;
      s.featureWidth = width;
      s.featureHeight = height;
      s.featureFlags = flags;
      s.featurePreset = params.preset;
      params.reset = 1u;

      dlssReport(str::format("DLAA feature created ", width, "x", height, ", flags 0x", std::hex, flags,
        std::dec, ", preset ", params.preset));
    }

    // Evaluate DLAA into our output image
    auto colorView = createNierView(color, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    auto depthView = createNierView(depth, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    auto mvView = createNierView(s.mvImage, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    auto outView = createNierView(s.outImage, VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    small_vector<DxvkResourceAccess, 4u> accesses;
    accesses.emplace_back(*colorView, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, false);
    accesses.emplace_back(*depthView, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, false);
    accesses.emplace_back(*mvView, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, false);
    accesses.emplace_back(*outView, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, true);

    syncResources(DxvkCmdBuffer::ExecBuffer, accesses.size(), accesses.data());
    flushBarriers();

    s.frame += 1u;

    NVSDK_NGX_Resource_VK colorRes = ngxResource(s, color, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK depthRes = ngxResource(s, depth, VK_IMAGE_ASPECT_DEPTH_BIT, false);
    NVSDK_NGX_Resource_VK mvRes = ngxResource(s, s.mvImage, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK outRes = ngxResource(s, s.outImage, VK_IMAGE_ASPECT_COLOR_BIT, true);

    if (!colorRes.Resource.ImageViewInfo.ImageView || !depthRes.Resource.ImageViewInfo.ImageView
     || !mvRes.Resource.ImageViewInfo.ImageView || !outRes.Resource.ImageViewInfo.ImageView) {
      dlssReport("DLAA skipped: could not create image views for NGX");
      NierModSettings::get().setDlssStatus(NierDlssStatus::Failed);
      return;
    }

    s.releaseUnusedRawViews();

    if (!s.evaluations && !s.failures) {
      dlssReport(str::format("first evaluate: color view 0x", std::hex,
        uint64_t(colorRes.Resource.ImageViewInfo.ImageView), " img 0x", uint64_t(color->handle()),
        " fmt ", std::dec, uint32_t(color->info().format),
        " | depth view 0x", std::hex, uint64_t(depthRes.Resource.ImageViewInfo.ImageView),
        " fmt ", std::dec, uint32_t(depth->info().format),
        " | mv view 0x", std::hex, uint64_t(mvRes.Resource.ImageViewInfo.ImageView),
        " | out view 0x", uint64_t(outRes.Resource.ImageViewInfo.ImageView), std::dec,
        " | handle queries so far ", g_viewHandleCalls, ", null ", g_viewHandleNulls));
    }

    NVSDK_NGX_VK_DLSS_Eval_Params evalParams = { };
    evalParams.Feature.pInColor = &colorRes;
    evalParams.Feature.pInOutput = &outRes;
    evalParams.pInDepth = &depthRes;
    evalParams.pInMotionVectors = &mvRes;
    evalParams.InJitterOffsetX = params.jitter[0];
    evalParams.InJitterOffsetY = params.jitter[1];
    evalParams.InRenderSubrectDimensions = { width, height };
    evalParams.InReset = int(params.reset);
    evalParams.InMVScaleX = 1.0f;
    evalParams.InMVScaleY = 1.0f;
    evalParams.InFrameTimeDeltaInMsec = params.frameTimeMs;

    NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT(
      m_cmd->getExternalCmdBuffer(DxvkCmdBuffer::ExecBuffer),
      static_cast<NVSDK_NGX_Handle*>(s.feature), ngxParams, &evalParams);

    m_cmd->track(color, DxvkAccess::Read);
    m_cmd->track(depth, DxvkAccess::Read);
    m_cmd->track(s.mvImage, DxvkAccess::Read);
    m_cmd->track(s.outImage, DxvkAccess::Write);

    this->nierAfterExternalCommands();

    if (!s.evaluations && !s.failures) {
      dlssReport(str::format("first evaluate returned 0x", std::hex, uint32_t(result), std::dec,
        ", handle queries ", g_viewHandleCalls, ", null ", g_viewHandleNulls));
    }

    if (g_viewHandleNulls) {
      // NGX asked for a handle of a view it does not have; stop before it gets worse
      s.initOk = false;
      dlssReport("DLAA disabled: NGX queried a null image view (see above)");
      NierModSettings::get().setDlssStatus(NierDlssStatus::Failed);
      return;
    }

    if (NVSDK_NGX_FAILED(result)) {
      s.failures += 1u;

      if (s.failures <= 3u)
        dlssReport(str::format("DLAA evaluate failed: 0x", std::hex, uint32_t(result)));
      NierModSettings::get().setDlssStatus(NierDlssStatus::Failed);
      return;
    }

    NierModSettings::get().setDlssStatus(NierDlssStatus::Running);

    // Replace the game's scene colour with the anti-aliased result
    VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
    this->copyImage(color, layers, VkOffset3D { 0, 0, 0 },
      s.outImage, layers, VkOffset3D { 0, 0, 0 }, VkExtent3D { width, height, 1u });

    if (!(s.evaluations++ % 3600u))
      dlssReport(str::format("DLAA running, ", s.evaluations, " frames, jitter ",
        params.jitter[0], " ", params.jitter[1]));
#endif
  }

}
