#include <algorithm>
#include <cstring>
#include <fstream>

#include "d3d11_nier_textures.h"

#include "../util/config/config.h"
#include "../util/log/log.h"
#include "../util/util_env.h"
#include "../util/util_string.h"

namespace dxvk {

  // DDS header fields (legacy header, no DX10 extension)
  constexpr size_t DdsHeaderSize  = 128u;
  constexpr size_t DdsHeightOff   = 12u;
  constexpr size_t DdsWidthOff    = 16u;
  constexpr size_t DdsMipsOff     = 28u;
  constexpr size_t DdsFourccOff   = 84u;
  constexpr size_t DdsDx10Size    = 20u;   // DDS_HEADER_DXT10 after the legacy header (fourcc "DX10")


  static uint32_t bcBlockBytes(DXGI_FORMAT format) {
    switch (format) {
      case DXGI_FORMAT_BC1_TYPELESS:
      case DXGI_FORMAT_BC1_UNORM:
      case DXGI_FORMAT_BC1_UNORM_SRGB:
        return 8u;

      case DXGI_FORMAT_BC2_TYPELESS:
      case DXGI_FORMAT_BC2_UNORM:
      case DXGI_FORMAT_BC2_UNORM_SRGB:
      case DXGI_FORMAT_BC3_TYPELESS:
      case DXGI_FORMAT_BC3_UNORM:
      case DXGI_FORMAT_BC3_UNORM_SRGB:
        return 16u;

      default:
        return 0u;
    }
  }


  static const char* bcFourcc(DXGI_FORMAT format) {
    switch (format) {
      case DXGI_FORMAT_BC1_TYPELESS:
      case DXGI_FORMAT_BC1_UNORM:
      case DXGI_FORMAT_BC1_UNORM_SRGB:
        return "DXT1";

      case DXGI_FORMAT_BC2_TYPELESS:
      case DXGI_FORMAT_BC2_UNORM:
      case DXGI_FORMAT_BC2_UNORM_SRGB:
        return "DXT3";

      default:
        return "DXT5";
    }
  }


  D3D11NierTextures& D3D11NierTextures::get() {
    static D3D11NierTextures textures;
    return textures;
  }


  D3D11NierTextures::D3D11NierTextures() {
    if (!Config::getUserConfig().getOption<bool>("d3d11.nierTextures", true))
      return;

    // Largest replacement size to upload (0 = no limit); bigger files start at a smaller mip (saves VRAM)
    m_maxSize = uint32_t(std::max(0, Config::getUserConfig().getOption<int32_t>("d3d11.nierTexturesMaxSize", 0)));

    std::string exe = env::getExePath();
    m_dir = exe.substr(0, exe.find_last_of("\\/") + 1u) + "nier_textures\\";

    WIN32_FIND_DATAA find = { };
    HANDLE handle = ::FindFirstFileA((m_dir + "*.dds").c_str(), &find);

    if (handle == INVALID_HANDLE_VALUE)
      return;

    do {
      // File names are the 16 hex digit hash of the original first mip
      std::string name = find.cFileName;

      if (name.size() == 20u)
        m_files.emplace(std::strtoull(name.substr(0, 16).c_str(), nullptr, 16), name);
    } while (::FindNextFileA(handle, &find));

    ::FindClose(handle);

    m_enabled = !m_files.empty();
    report(str::format("texture replacement: ", m_files.size(), " textures in ", m_dir));
  }


  uint64_t D3D11NierTextures::hash(const void* data, size_t size, uint64_t h) {
    auto bytes = reinterpret_cast<const uint8_t*>(data);

    for (size_t i = 0; i < size; i++) {
      h ^= bytes[i];
      h *= 0x100000001b3ull;
    }

    return h;
  }


  uint64_t D3D11NierTextures::textureKey(const char* mip0, size_t size, uint32_t width, uint32_t height) {
    // Must match texture_key() in remaster/inject.py
    constexpr size_t Sample = 4096u;
    size_t n = std::min(size, Sample);
    size_t mid = size / 2u - std::min(size / 2u, Sample / 2u);

    uint64_t h = hash(mip0, n);
    h = hash(mip0 + mid, std::min(size - mid, Sample), h);
    h = hash(mip0 + size - n, n, h);

    uint32_t dims[2] = { width, height };
    return hash(dims, sizeof(dims), h);
  }


  bool D3D11NierTextures::find(
    const D3D11_TEXTURE2D_DESC1&        desc,
    const D3D11_SUBRESOURCE_DATA*       init,
          Replacement&                  out) {
    if (!m_enabled)
      return false;

    uint32_t blockBytes = bcBlockBytes(desc.Format);

    if (!init || !init[0].pSysMem) {
      if (blockBytes)
        note(Event::CreateNoData);
      return false;
    }

    // Only plain, immutable-style albedo textures are candidates
    if (!blockBytes || desc.ArraySize != 1u || desc.SampleDesc.Count != 1u
     || desc.CPUAccessFlags || desc.MiscFlags
     || desc.BindFlags != D3D11_BIND_SHADER_RESOURCE
     || (desc.Usage != D3D11_USAGE_DEFAULT && desc.Usage != D3D11_USAGE_IMMUTABLE))
      return false;

    // Key: the tightly packed first mip, sampled at start, middle and end, plus its size
    uint32_t blocksW = std::max(1u, (desc.Width + 3u) / 4u);
    uint32_t blocksH = std::max(1u, (desc.Height + 3u) / 4u);
    size_t rowBytes = size_t(blocksW) * blockBytes;
    size_t size = rowBytes * blocksH;

    if (init[0].SysMemPitch < rowBytes)
      return false;

    auto src = reinterpret_cast<const char*>(init[0].pSysMem);
    std::vector<char> packed;

    if (init[0].SysMemPitch != rowBytes) {
      packed.resize(size);

      for (uint32_t y = 0; y < blocksH; y++)
        std::memcpy(&packed[y * rowBytes], src + size_t(y) * init[0].SysMemPitch, rowBytes);

      src = packed.data();
    }

    uint64_t h = textureKey(src, size, desc.Width, desc.Height);

    auto entry = m_files.find(h);

    if (entry == m_files.end()) {
      uint32_t misses = ++m_misses;

      if (misses <= 32u || !(misses % 1000u)) {
        report(str::format("no replacement #", misses, ": ", std::hex, h, std::dec,
          " ", desc.Width, "x", desc.Height, " fmt ", uint32_t(desc.Format), " mips ", desc.MipLevels));
      }

      return false;
    }

    if (!load(m_dir + entry->second, desc, out))
      return false;

    uint32_t replaced = ++m_replaced;

    if (replaced <= 8u || !(replaced % 500u)) {
      report(str::format("replaced #", replaced, ": ", entry->second, " ", desc.Width, "x", desc.Height,
        " -> ", out.desc.Width, "x", out.desc.Height));
    }

    return true;
  }


  bool D3D11NierTextures::load(const std::string& path, const D3D11_TEXTURE2D_DESC1& desc, Replacement& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);

    if (!file)
      return false;

    out.file.resize(size_t(file.tellg()));
    file.seekg(0);
    file.read(out.file.data(), out.file.size());

    if (out.file.size() < DdsHeaderSize || std::memcmp(out.file.data(), "DDS ", 4))
      return false;

    uint32_t height, width, mips;
    std::memcpy(&height, &out.file[DdsHeightOff], 4);
    std::memcpy(&width,  &out.file[DdsWidthOff],  4);
    std::memcpy(&mips,   &out.file[DdsMipsOff],   4);
    mips = std::max(mips, 1u);

    // Same compression (or BC7 from a DX10 header, e.g. Special K style packs) and aspect ratio as the
    // original, otherwise keep the original
    DXGI_FORMAT format = desc.Format;
    size_t offset = DdsHeaderSize;

    if (!std::memcmp(&out.file[DdsFourccOff], "DX10", 4)) {
      if (out.file.size() < DdsHeaderSize + DdsDx10Size)
        return false;

      uint32_t dxgi;
      std::memcpy(&dxgi, &out.file[DdsHeaderSize], 4);
      offset += DdsDx10Size;

      if (dxgi == DXGI_FORMAT_BC7_TYPELESS || dxgi == DXGI_FORMAT_BC7_UNORM || dxgi == DXGI_FORMAT_BC7_UNORM_SRGB)
        format = DXGI_FORMAT_BC7_TYPELESS;   // views get the game's sRGB choice, see CreateShaderResourceView1
      else if (dxgi < DXGI_FORMAT_BC1_TYPELESS || dxgi > DXGI_FORMAT_BC3_UNORM_SRGB
            || std::strcmp(bcFourcc(DXGI_FORMAT(dxgi)), bcFourcc(desc.Format)))
        return false;   // only BC1-3 of the original's kind, or BC7
    } else if (std::memcmp(&out.file[DdsFourccOff], bcFourcc(desc.Format), 4)) {
      return false;
    }

    if (uint64_t(width) * desc.Height != uint64_t(height) * desc.Width)
      return false;

    uint32_t blockBytes = format == DXGI_FORMAT_BC7_TYPELESS ? 16u : bcBlockBytes(desc.Format);

    // Skip the top mips of files above the size limit
    while (m_maxSize && mips > 1u && std::max(width, height) > m_maxSize) {
      uint32_t pitch = std::max(1u, (width + 3u) / 4u) * blockBytes;
      offset += size_t(pitch) * std::max(1u, (height + 3u) / 4u);
      width = std::max(1u, width >> 1);
      height = std::max(1u, height >> 1);
      mips -= 1u;
    }

    out.data.resize(mips);

    for (uint32_t i = 0; i < mips; i++) {
      uint32_t w = std::max(1u, width >> i);
      uint32_t h = std::max(1u, height >> i);
      uint32_t pitch = std::max(1u, (w + 3u) / 4u) * blockBytes;
      uint32_t size = pitch * std::max(1u, (h + 3u) / 4u);

      if (offset + size > out.file.size())
        return false;

      out.data[i].pSysMem = &out.file[offset];
      out.data[i].SysMemPitch = pitch;
      out.data[i].SysMemSlicePitch = size;
      offset += size;
    }

    out.desc = desc;
    out.desc.Width = width;
    out.desc.Height = height;
    out.desc.MipLevels = mips;
    out.desc.Format = format;
    return true;
  }


  void D3D11NierTextures::onPresent() {
    if (!m_enabled)
      return;

    uint64_t now = ::GetTickCount64();

    if (!m_lastReport)
      m_lastReport = now;

    if (now - m_lastReport < 10000u)
      return;

    m_lastReport = now;

    static const char* names[] = { "update replaced", "update other", "copy from replaced",
      "copy to replaced", "SetResourceMinLOD", "BC created without data", "replaced destroyed" };

    std::string line = "10s:";

    for (uint32_t i = 0; i < uint32_t(Event::Count); i++)
      line += str::format(" | ", names[i], " ", m_events[i].exchange(0u));

    line += str::format(" | replaced total ", m_replaced.load());
    report(line);
  }


  void D3D11NierTextures::report(const std::string& line) {
    std::lock_guard lock(m_reportMutex);

    std::string exe = env::getExePath();
    std::string dir = exe.substr(0, exe.find_last_of("\\/") + 1u) + "nier_trace";
    ::CreateDirectoryA(dir.c_str(), nullptr);

    std::ofstream file(dir + "\\nier_textures.txt", std::ios::app);
    file << line << "\n";
  }

}
