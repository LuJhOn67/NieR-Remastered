#include <algorithm>
#include <cstring>
#include <fstream>

#include "dxvk_context.h"
#include "dxvk_device.h"
#include "dxvk_hash.h"
#include "dxvk_nier_rt.h"

#include <dxvk_nier_rtao.h>

#include "../util/util_env.h"

namespace dxvk {

  /// New BLAS builds per frame; the rest are deferred to later frames to avoid hitches
  constexpr uint32_t MaxBlasBuildsPerFrame = 96u;
  /// BLAS not referenced for this many frames are released (~30 s at 60 fps)
  constexpr uint64_t BlasEvictFrames = 1800u;
  constexpr uint64_t EvictInterval = 300u;
  constexpr uint32_t StatsInterval = 600u;

  constexpr VkPipelineStageFlags2 AsBuildStage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  constexpr VkAccessFlags2 AsAccess = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR
                                    | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;


  static Rc<DxvkBuffer> createRtBuffer(
          DxvkDevice*           device,
          VkDeviceSize          size,
          VkBufferUsageFlags    usage,
          VkMemoryPropertyFlags memoryType,
    const char*                 name) {
    DxvkBufferCreateInfo info;
    info.size   = size;
    info.usage  = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
                | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    info.access = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
                | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR
                | VK_ACCESS_SHADER_READ_BIT;
    info.debugName = name;

    // Acceleration structures and build inputs are referenced by GPU address
    auto buffer = device->createBuffer(info, memoryType);
    buffer->enableStableAddress();
    return buffer;
  }


  DxvkRtAccel::DxvkRtAccel(
          DxvkDevice*                     device,
          Rc<DxvkBuffer>                  storage,
          VkAccelerationStructureTypeKHR  type)
  : m_vkd(device->vkd()), m_storage(std::move(storage)) {
    auto slice = m_storage->getSliceInfo();

    VkAccelerationStructureCreateInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    info.buffer = slice.buffer;
    info.offset = slice.offset;
    info.size   = slice.size;
    info.type   = type;

    if (m_vkd->vkCreateAccelerationStructureKHR(m_vkd->device(), &info, nullptr, &m_handle) != VK_SUCCESS)
      throw DxvkError("NieR RT: Failed to create acceleration structure");

    VkAccelerationStructureDeviceAddressInfoKHR addressInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
    addressInfo.accelerationStructure = m_handle;
    m_address = m_vkd->vkGetAccelerationStructureDeviceAddressKHR(m_vkd->device(), &addressInfo);
  }


  DxvkRtAccel::~DxvkRtAccel() {
    m_vkd->vkDestroyAccelerationStructureKHR(m_vkd->device(), m_handle, nullptr);
  }


  DxvkRtScene::~DxvkRtScene() {
    if (rtaoPipeline)
      vkd->vkDestroyPipeline(vkd->device(), rtaoPipeline, nullptr);
  }


  size_t DxvkRtBlasKeyHash::operator () (const DxvkRtBlasKey& k) const {
    DxvkHashState hash;
    hash.add(reinterpret_cast<uintptr_t>(k.vertexBuffer));
    hash.add(reinterpret_cast<uintptr_t>(k.indexBuffer));
    hash.add(k.vertexOffset);
    hash.add(k.indexOffset);
    hash.add(k.vertexStride);
    hash.add(k.firstIndex);
    hash.add(k.indexCount);
    hash.add(uint32_t(k.baseVertex));
    return hash;
  }


  static void writeSceneStats(const DxvkRtScene& scene) {
    std::string path = env::getExePath();
    path = path.substr(0, path.find_last_of("\\/") + 1u) + "nier_trace";
    ::CreateDirectoryA(path.c_str(), nullptr);

    uint32_t f = std::max(scene.stats.frames, 1u);
    std::ofstream file(path + "\\nier_rt_scene.txt", std::ios::app);
    file << "frame " << scene.frame
         << " | BLAS cached " << scene.blas.size()
         << " (" << (scene.blasBytes >> 20) << " MiB)"
         << ", built " << scene.stats.built
         << ", evicted " << scene.stats.evicted
         << " | TLAS instances/frame " << scene.stats.instances / f
         << ", deferred/frame " << scene.stats.skipped / f
         << ", TLAS " << (scene.tlas != nullptr ? (scene.tlas->size() >> 10) : 0u) << " KiB"
         << " | RTAO runs " << scene.rtaoRuns << "\n";
  }


  void DxvkContext::nierRtao(
    const Rc<DxvkImageView>&            depthView,
    const Rc<DxvkImage>&                aoImage,
          DxvkNierRtaoParams            params,
          std::vector<DxvkRtInstanceDesc>&& draws) {
    // Build with this frame's pre-pass geometry so the TLAS matches the depth buffer
    nierBuildRtScene(std::move(draws));

    if (!m_nierRt || m_nierRt->tlas == nullptr || !m_nierRt->tlasInstances
     || !m_device->features().khrRayQuery.rayQuery)
      return;

    auto& scene = *m_nierRt;

    // The SSAO target is a plain render target; it needs storage usage
    VkFormat aoFormat = aoImage->info().format;
    auto formatFeatures = m_device->adapter()->getFormatFeatures(aoFormat);

    if (!(formatFeatures.optimal & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT))
      return;

    this->endCurrentPass(true);

    DxvkImageUsageInfo usage = { };
    usage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    usage.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    usage.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    if (!ensureImageCompatibility(aoImage.ptr(), usage))
      return;

    this->invalidateState();

    if (!scene.rtaoPipeline) {
      std::array<DxvkDescriptorSetLayoutBinding, 2> bindings = {{
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT },
      }};

      scene.rtaoLayout = m_device->createBuiltInPipelineLayout(DxvkPipelineLayoutFlag::UsesSamplerHeap,
        VK_SHADER_STAGE_COMPUTE_BIT, sizeof(DxvkNierRtaoParams), bindings.size(), bindings.data());

      util::DxvkBuiltInShaderStage shader(dxvk_nier_rtao, nullptr);
      scene.rtaoPipeline = m_device->createBuiltInComputePipeline(scene.rtaoLayout, shader);
    }

    DxvkImageViewKey key = { };
    key.viewType = VK_IMAGE_VIEW_TYPE_2D;
    key.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    key.layout = VK_IMAGE_LAYOUT_GENERAL;
    key.format = aoFormat;
    key.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    key.mipIndex = 0u;
    key.mipCount = 1u;
    key.layerIndex = 0u;
    key.layerCount = 1u;

    Rc<DxvkImageView> aoView = aoImage->createView(key);

    std::array<DxvkDescriptorWrite, 2> descriptors = { };
    descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptors[0].descriptor = depthView->getDescriptor();
    descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    descriptors[1].descriptor = aoView->getDescriptor();

    small_vector<DxvkResourceAccess, 2u> accesses;
    accesses.emplace_back(*depthView, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      VK_ACCESS_2_SHADER_READ_BIT, false);
    accesses.emplace_back(*aoView, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, false);

    params.tlas = scene.tlas->address();
    params.frame = uint32_t(scene.frame);

    m_cmd->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
      VK_PIPELINE_BIND_POINT_COMPUTE, scene.rtaoPipeline);

    m_cmd->bindResources(DxvkCmdBuffer::ExecBuffer,
      scene.rtaoLayout, descriptors.size(), descriptors.data(),
      sizeof(params), &params);

    syncResources(DxvkCmdBuffer::ExecBuffer, accesses.size(), accesses.data());

    m_cmd->cmdDispatch(DxvkCmdBuffer::ExecBuffer,
      (params.size[0] + 7u) / 8u, (params.size[1] + 7u) / 8u, 1u);

    m_cmd->track(scene.tlas);
    scene.rtaoRuns += 1u;
  }


  void DxvkContext::nierBuildRtScene(std::vector<DxvkRtInstanceDesc>&& draws) {
    if (!m_device->features().khrAccelerationStructure.accelerationStructure)
      return;

    if (!m_nierRt) {
      m_nierRt = std::make_unique<DxvkRtScene>();
      m_nierRt->vkd = m_device->vkd();
    }

    auto& scene = *m_nierRt;
    scene.frame += 1u;
    scene.stats.frames += 1u;

    auto vk = m_device->vkd();
    VkDeviceSize scratchAlign = std::max<VkDeviceSize>(1u,
      m_device->properties().khrAccelerationStructure.minAccelerationStructureScratchOffsetAlignment);

    struct PendingBuild {
      VkAccelerationStructureGeometryKHR        geometry;
      VkAccelerationStructureBuildGeometryInfoKHR info;
      VkAccelerationStructureBuildRangeInfoKHR  range;
      VkDeviceSize                              scratchSize;
    };

    std::vector<PendingBuild> builds;
    builds.reserve(MaxBlasBuildsPerFrame);

    std::vector<DxvkResourceAccess> accesses;
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    instances.reserve(draws.size());

    for (auto& d : draws) {
      DxvkRtBlasKey key = { d.vertexBuffer.ptr(), d.indexBuffer.ptr(), d.vertexOffset, d.indexOffset,
        d.vertexStride, d.firstIndex, d.indexCount, d.baseVertex };

      auto entry = scene.blas.find(key);

      if (entry == scene.blas.end()) {
        if (builds.size() >= MaxBlasBuildsPerFrame || d.baseVertex < 0 || !d.vertexStride) {
          scene.stats.skipped += 1u;
          continue;
        }

        auto vbSlice = d.vertexBuffer->getSliceInfo();
        auto ibSlice = d.indexBuffer->getSliceInfo();

        VkDeviceSize indexSize = d.indexType == VK_INDEX_TYPE_UINT32 ? 4u : 2u;
        VkDeviceSize vertexStart = d.vertexOffset + VkDeviceSize(d.baseVertex) * d.vertexStride;
        VkDeviceSize indexStart = d.indexOffset + VkDeviceSize(d.firstIndex) * indexSize;
        VkDeviceSize indexBytes = VkDeviceSize(d.indexCount) * indexSize;
        uint32_t primitiveCount = d.indexCount / 3u;

        if (vertexStart >= vbSlice.size || indexStart + indexBytes > ibSlice.size || !primitiveCount) {
          scene.stats.skipped += 1u;
          continue;
        }

        // We do not know the highest referenced index without reading the
        // index buffer, so allow everything up to the end of the buffer
        uint32_t maxVertex = uint32_t((vbSlice.size - vertexStart) / d.vertexStride) - 1u;

        auto& b = builds.emplace_back();
        b.geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        b.geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        b.geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

        auto& tri = b.geometry.geometry.triangles;
        tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.vertexFormat = d.vertexFormat;
        tri.vertexData.deviceAddress = vbSlice.gpuAddress + vertexStart;
        tri.vertexStride = d.vertexStride;
        tri.maxVertex = maxVertex;
        tri.indexType = d.indexType;
        tri.indexData.deviceAddress = ibSlice.gpuAddress + indexStart;

        b.info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        b.info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        b.info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        b.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        b.info.geometryCount = 1u;
        b.info.pGeometries = &b.geometry;

        b.range = { };
        b.range.primitiveCount = primitiveCount;

        VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        vk->vkGetAccelerationStructureBuildSizesKHR(vk->device(),
          VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &b.info, &primitiveCount, &sizes);

        auto storage = createRtBuffer(m_device.ptr(), sizes.accelerationStructureSize,
          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "NieR BLAS");

        Rc<DxvkRtAccel> accel = new DxvkRtAccel(m_device.ptr(), std::move(storage),
          VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);

        b.info.dstAccelerationStructure = accel->handle();
        b.scratchSize = align(sizes.buildScratchSize, scratchAlign);

        accesses.emplace_back(*d.vertexBuffer, vertexStart, vbSlice.size - vertexStart,
          AsBuildStage, VK_ACCESS_2_SHADER_READ_BIT);
        accesses.emplace_back(*d.indexBuffer, indexStart, indexBytes,
          AsBuildStage, VK_ACCESS_2_SHADER_READ_BIT);

        m_cmd->track(d.vertexBuffer, DxvkAccess::Read);
        m_cmd->track(d.indexBuffer, DxvkAccess::Read);

        scene.blasBytes += accel->size();
        scene.stats.built += 1u;

        DxvkRtBlasEntry newEntry;
        newEntry.accel = std::move(accel);
        newEntry.vertexBuffer = d.vertexBuffer;
        newEntry.indexBuffer = d.indexBuffer;
        entry = scene.blas.emplace(key, std::move(newEntry)).first;
      }

      entry->second.lastUsed = scene.frame;
      m_cmd->track(entry->second.accel);

      VkAccelerationStructureInstanceKHR instance = { };
      std::memcpy(instance.transform.matrix, d.transform.data(), sizeof(instance.transform.matrix));
      instance.mask = 0xffu;
      instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
      instance.accelerationStructureReference = entry->second.accel->address();
      instances.push_back(instance);
    }

    scene.stats.instances += instances.size();

    if (!builds.empty() || !instances.empty())
      this->endCurrentPass(true);

    // Bottom-level builds for meshes seen for the first time
    if (!builds.empty()) {
      syncResources(DxvkCmdBuffer::ExecBuffer, accesses.size(), accesses.data());

      VkDeviceSize scratchTotal = 0u;

      for (const auto& b : builds)
        scratchTotal += b.scratchSize;

      // The start address gets aligned below, so keep one alignment unit of slack
      scratchTotal += scratchAlign;

      if (scene.blasScratch == nullptr || scene.blasScratch->info().size < scratchTotal) {
        scene.blasScratch = createRtBuffer(m_device.ptr(), std::max<VkDeviceSize>(scratchTotal, 16u << 20),
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "NieR BLAS scratch");
      }

      // Previous builds may still use the scratch memory
      accessMemory(DxvkCmdBuffer::ExecBuffer, AsBuildStage, AsAccess, AsBuildStage, AsAccess);
      flushBarriers();

      VkDeviceAddress scratchAddress = align(scene.blasScratch->getSliceInfo().gpuAddress, scratchAlign);

      std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
      std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;

      for (auto& b : builds) {
        b.info.pGeometries = &b.geometry;
        b.info.scratchData.deviceAddress = scratchAddress;
        scratchAddress += b.scratchSize;

        infos.push_back(b.info);
        ranges.push_back(&b.range);
      }

      m_cmd->cmdBuildAccelerationStructures(DxvkCmdBuffer::ExecBuffer,
        uint32_t(infos.size()), infos.data(), ranges.data());
      m_cmd->track(scene.blasScratch, DxvkAccess::Write);
    }

    // Top-level structure over every instance drawn this frame
    if (!instances.empty()) {
      VkDeviceSize instanceBytes = instances.size() * sizeof(VkAccelerationStructureInstanceKHR);

      auto instanceBuffer = createRtBuffer(m_device.ptr(), instanceBytes,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "NieR TLAS instances");

      auto instanceSlice = instanceBuffer->getSliceInfo();
      std::memcpy(instanceSlice.mapPtr, instances.data(), instanceBytes);

      VkAccelerationStructureGeometryKHR geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
      geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
      geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
      geometry.geometry.instances.arrayOfPointers = VK_FALSE;
      geometry.geometry.instances.data.deviceAddress = instanceSlice.gpuAddress;

      VkAccelerationStructureBuildGeometryInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
      info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
      info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
      info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
      info.geometryCount = 1u;
      info.pGeometries = &geometry;

      uint32_t instanceCount = uint32_t(instances.size());

      // Size the TLAS with headroom so it is not reallocated every frame
      if (scene.tlas == nullptr || scene.tlasCapacity < instanceCount) {
        uint32_t capacity = std::max(instanceCount + instanceCount / 2u, 1024u);

        VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        vk->vkGetAccelerationStructureBuildSizesKHR(vk->device(),
          VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &capacity, &sizes);

        auto storage = createRtBuffer(m_device.ptr(), sizes.accelerationStructureSize,
          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "NieR TLAS");

        scene.tlas = new DxvkRtAccel(m_device.ptr(), std::move(storage),
          VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
        scene.tlasCapacity = capacity;

        scene.tlasScratch = createRtBuffer(m_device.ptr(), sizes.buildScratchSize + scratchAlign,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "NieR TLAS scratch");
      }

      info.dstAccelerationStructure = scene.tlas->handle();
      info.scratchData.deviceAddress = align(scene.tlasScratch->getSliceInfo().gpuAddress, scratchAlign);

      VkAccelerationStructureBuildRangeInfoKHR range = { };
      range.primitiveCount = instanceCount;
      const VkAccelerationStructureBuildRangeInfoKHR* rangePtr = &range;

      // BLAS builds above, and last frame's ray queries, must finish first
      accessMemory(DxvkCmdBuffer::ExecBuffer,
        AsBuildStage | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, AsAccess,
        AsBuildStage, AsAccess);
      flushBarriers();

      m_cmd->cmdBuildAccelerationStructures(DxvkCmdBuffer::ExecBuffer, 1u, &info, &rangePtr);

      // Make the TLAS visible to ray queries in compute shaders
      accessMemory(DxvkCmdBuffer::ExecBuffer,
        AsBuildStage, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
      flushBarriers();

      m_cmd->track(std::move(instanceBuffer), DxvkAccess::Read);
      m_cmd->track(scene.tlas);
      m_cmd->track(scene.tlasScratch, DxvkAccess::Write);
      scene.tlasInstances = instanceCount;
    } else {
      scene.tlasInstances = 0u;
    }

    // Release meshes that have not been drawn for a while; command lists
    // that used them still hold their own references
    if (!(scene.frame % EvictInterval)) {
      for (auto i = scene.blas.begin(); i != scene.blas.end(); ) {
        if (scene.frame - i->second.lastUsed > BlasEvictFrames) {
          scene.blasBytes -= i->second.accel->size();
          scene.stats.evicted += 1u;
          i = scene.blas.erase(i);
        } else {
          i++;
        }
      }
    }

    if (scene.stats.frames >= StatsInterval) {
      writeSceneStats(scene);
      scene.stats = { };
    }
  }

}
