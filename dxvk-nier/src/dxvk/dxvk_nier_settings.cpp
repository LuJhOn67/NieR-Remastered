#include <algorithm>
#include <array>
#include <fstream>

#include "dxvk_nier_settings.h"

#include "../util/config/config.h"
#include "../util/util_env.h"
#include "../util/util_win32_compat.h"

namespace dxvk {

  // NVSDK_NGX_DLSS_Hint_Render_Preset values offered in the menu
  static const std::array<uint32_t, 5> NierPresets = { 0u, 10u, 11u, 12u, 13u };


  NierModSettings& NierModSettings::get() {
    static NierModSettings settings;
    return settings;
  }


  NierModSettings::NierModSettings() {
    m_dlssLoaded = Config::getUserConfig().getOption<bool>("dxvk.nierDlss", false);
    m_dlssPreset = uint32_t(Config::getUserConfig().getOption<int32_t>("dxvk.nierDlssPreset", 12));
    load();

    m_frameGenStartup = m_frameGen.load();
  }


  std::string NierModSettings::iniPath() const {
    std::string path = env::getExePath();
    return path.substr(0, path.find_last_of("\\/") + 1u) + "nier_mod.ini";
  }


  void NierModSettings::load() {
    std::ifstream file(iniPath());
    std::string line;

    while (std::getline(file, line)) {
      auto eq = line.find('=');

      if (eq == std::string::npos)
        continue;

      std::string key = line.substr(0, eq);
      int32_t value = std::atoi(line.c_str() + eq + 1u);

      if (key == "dlss")
        m_dlssEnabled = value != 0;
      else if (key == "dlssPreset")
        m_dlssPreset = uint32_t(value);
      else if (key == "frameGen")
        m_frameGen = std::min(uint32_t(value), 1u);
    }
  }


  void NierModSettings::save() const {
    std::ofstream file(iniPath(), std::ios::trunc);
    file << "# NieR mod settings, changed with the in-game menu (Page Up)\n"
         << "dlss=" << (m_dlssEnabled.load() ? 1 : 0) << "\n"
         << "dlssPreset=" << m_dlssPreset.load() << "\n"
         << "# 0 = off, 1 = NVIDIA Smooth Motion (RTX 50, applies after a restart)\n"
         << "frameGen=" << m_frameGen.load() << "\n";
  }


  uint32_t NierModSettings::rowCount() const {
    uint32_t count = 0u;

    // DLSS on/off and preset, only if DLSS is usable
    if (m_dlssLoaded && dlssStatus() != NierDlssStatus::Unsupported)
      count += 2u;

    if (frameGenSupported())
      count += 1u;

    return count;
  }


  NierMenuRow NierModSettings::rowKind(uint32_t row) const {
    if (!m_dlssLoaded || dlssStatus() == NierDlssStatus::Unsupported)
      row += 2u;

    return NierMenuRow(std::min(row, uint32_t(NierMenuRow::FrameGen)));
  }


  void NierModSettings::applyFrameGenEnvironment() const {
    // Loads the driver's present layer with Smooth Motion for this process only
    if (m_frameGenStartup == 1u)
      ::SetEnvironmentVariableA("NVPRESENT_ENABLE_SMOOTH_MOTION", "1");
  }


  const char* NierModSettings::presetName(uint32_t preset) {
    switch (preset) {
      case 0u:  return "Default";
      case 10u: return "J";
      case 11u: return "K";
      case 12u: return "L";
      case 13u: return "M";
      default:  return "?";
    }
  }


  void NierModSettings::changeValue(int32_t delta) {
    switch (rowKind(m_row.load())) {
      case NierMenuRow::DlssEnabled:
        m_dlssEnabled = !m_dlssEnabled.load();
        break;

      case NierMenuRow::FrameGen:
        m_frameGen = m_frameGen.load() ? 0u : 1u;
        break;

      case NierMenuRow::DlssPreset: {
        uint32_t index = 0u;

        for (uint32_t i = 0u; i < NierPresets.size(); i++) {
          if (NierPresets[i] == m_dlssPreset.load())
            index = i;
        }

        index = (index + NierPresets.size() + delta) % NierPresets.size();
        m_dlssPreset = NierPresets[index];
      } break;
    }

    save();
  }


  bool NierModSettings::keyPressed(int key, uint32_t bit) {
    bool down = (::GetAsyncKeyState(key) & 0x8000) != 0;
    bool wasDown = (m_keysDown & bit) != 0;

    if (down)
      m_keysDown |= bit;
    else
      m_keysDown &= ~bit;

    return down && !wasDown;
  }


  void NierModSettings::pollInput() {
    // Ignore keys while another window has focus
    DWORD pid = 0;
    ::GetWindowThreadProcessId(::GetForegroundWindow(), &pid);

    if (pid != ::GetCurrentProcessId()) {
      m_keysDown = ~0u;
      return;
    }

    if (keyPressed(VK_PRIOR, 1u << 0))
      m_menuOpen = !m_menuOpen.load();

    bool up    = keyPressed(VK_UP,     1u << 1);
    bool down  = keyPressed(VK_DOWN,   1u << 2);
    bool left  = keyPressed(VK_LEFT,   1u << 3);
    bool right = keyPressed(VK_RIGHT,  1u << 4);
    bool enter = keyPressed(VK_RETURN, 1u << 5);

    uint32_t rows = rowCount();

    if (!m_menuOpen.load() || !rows)
      return;

    if (up)
      m_row = (m_row.load() + rows - 1u) % rows;

    if (down)
      m_row = (m_row.load() + 1u) % rows;

    if (m_row.load() >= rows)
      m_row = 0u;

    if (left)
      changeValue(-1);

    if (right || enter)
      changeValue(1);
  }

}
