#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace dxvk {

  /**
   * \brief DLSS state as shown in the mod menu
   */
  enum class NierDlssStatus : uint32_t {
    Waiting,      ///< Not initialised yet (no 3D scene so far)
    Running,
    NeedAaOff,    ///< The game renders with MSAA
    Failed,
    Unsupported,  ///< NGX reports no DLSS on this GPU/driver
  };


  /**
   * \brief Rows of the in-game menu
   */
  enum class NierMenuRow : uint32_t {
    DlssEnabled,
    DlssPreset,
    FrameGen,
  };


  /**
   * \brief Runtime settings of the NieR mod (in-game menu)
   *
   * Toggled with Page Up. Values are saved to nier_mod.ini next to the exe.
   * DLSS rows only exist if \c dxvk.nierDlss loaded NGX support at startup.
   */
  class NierModSettings {

  public:

    static NierModSettings& get();

    /// Polls the menu keys, call once per presented frame
    void pollInput();

    bool menuOpen() const { return m_menuOpen.load(); }

    uint32_t selectedRow() const { return m_row.load(); }

    /// Whether DLSS support was loaded at startup (dxvk.nierDlss)
    bool dlssLoaded() const { return m_dlssLoaded; }

    bool dlssEnabled() const { return m_dlssEnabled.load(); }

    uint32_t dlssPreset() const { return m_dlssPreset.load(); }

    NierDlssStatus dlssStatus() const { return NierDlssStatus(m_dlssStatus.load()); }

    void setDlssStatus(NierDlssStatus status) { m_dlssStatus.store(uint32_t(status)); }

    /// Frame generation: 0 = off, 1 = NVIDIA Smooth Motion (driver, 2x)
    uint32_t frameGen() const { return m_frameGen.load(); }

    /// Frame generation mode the game was started with (needs a restart to change)
    uint32_t frameGenAtStartup() const { return m_frameGenStartup; }

    /// Smooth Motion needs an RTX 50 series GPU
    bool frameGenSupported() const { return m_frameGenSupported.load(); }

    void setFrameGenSupported(bool supported) { m_frameGenSupported.store(supported); }

    /// Menu rows that are currently shown and can be selected
    uint32_t rowCount() const;

    /// Kind of the given visible row
    NierMenuRow rowKind(uint32_t row) const;

    /// Sets the environment that enables frame generation; call before
    /// the Vulkan instance is created
    void applyFrameGenEnvironment() const;

    static const char* presetName(uint32_t preset);

  private:

    NierModSettings();

    void load();

    void save() const;

    void changeValue(int32_t delta);

    bool keyPressed(int key, uint32_t bit);

    std::string iniPath() const;

    bool                  m_dlssLoaded  = false;

    std::atomic<bool>     m_menuOpen    = { false };
    std::atomic<uint32_t> m_row         = { 0u };
    std::atomic<bool>     m_dlssEnabled = { true };
    std::atomic<uint32_t> m_dlssPreset  = { 12u };
    std::atomic<uint32_t> m_dlssStatus  = { uint32_t(NierDlssStatus::Waiting) };
    std::atomic<uint32_t> m_frameGen    = { 0u };
    uint32_t              m_frameGenStartup = 0u;
    std::atomic<bool>     m_frameGenSupported = { false };

    uint32_t              m_keysDown    = 0u;

  };

}
