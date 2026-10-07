#pragma once

#include "../helpers/memory/Memory.hpp"
#include "../helpers/signal/Signal.hpp"
#include "../helpers/math/Math.hpp"
#include "../desktop/DesktopTypes.hpp"
#include "../devices/IKeyboard.hpp"
#include "../devices/IPointer.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <hyprutils/os/FileDescriptor.hpp>

class IWaylandProtocol;
class CXCursorManager;
class CInputMethodRelay;
class CSeatManager;
class CWLSeatProtocol;
class CWLSurfaceResource;
class CVirtualKeyboardV1Resource;
class CVirtualPointerV1Resource;
class CEventLoopTimer;
namespace Layout::Supplementary {
    class CDragStateController;
}
struct wl_global;
struct wl_client;
struct wl_event_source;
struct SSeatInputClient;
namespace Desktop::View {
    class CWLSurface;
}
namespace Pointer {
    class CPointerManager;
}
namespace Render {
    class CRenderContext;
}

// A desktop seat owns its focus and cursor. The default seat keeps the existing
// input policy; secondary seats never enter that policy through a global swap.
class CSeatDesktop {
  public:
    CSeatDesktop(const std::string& name, PHLMONITOR monitor);
    ~CSeatDesktop();

    CSeatManager*                                manager() const;
    CInputMethodRelay*                           relay() const;
    CWLSeatProtocol*                             protocol() const;
    Pointer::CPointerManager*                    pointer() const;
    PHLMONITOR                                   monitor() const;
    PHLWORKSPACE                                 workspace() const;
    PHLWINDOW                                    window() const;
    bool                                         active() const;
    bool                                         inputAllowed() const;
    bool                                         viewAvailable() const;
    bool                                         paused() const;
    bool                                         continuesOnHumanLock() const;
    void                                         setHumanLockPolicy(bool allow);
    void                                         setCaptureGrant(const std::string& grant);
    bool                                         captureGranted(const std::string& grant) const;
    uint64_t                                     controlGeneration() const;
    uint64_t                                     viewEpoch() const;
    void                                         setPaused(bool paused);
    void                                         retire();
    const std::string&                           socketName() const;
    bool                                         ownsClient(const wl_client* client) const;
    bool                                         allowsInputSource(wl_client* client);
    void                                         forgetInputClient(wl_client* client);
    Layout::Supplementary::CDragStateController* dragController() const;
    void                                         setCursorShape(const std::string& name);

    void                                         attachKeyboard(SP<IKeyboard> keyboard);
    void                                         attachPointer(SP<IPointer> pointer);
    void                                         focusWindow(PHLWINDOW window, SP<CWLSurfaceResource> surface = nullptr);
    void                                         refocus(uint32_t timeMs = 0, bool keyboard = false);
    std::string                                  switchWorkspace(const std::string& name);
    std::vector<uint32_t>                        pressedKeys() const;
    const std::vector<SP<IKeyboard>>&            keyboards() const;

  private:
    static int                                           acceptClient(int fd, uint32_t mask, void* data);
    void                                                 updateCapabilities();
    void                                                 keyboardKey(const IKeyboard::SKeyEvent& event, SP<IKeyboard> keyboard);
    void                                                 keyboardModifiers(SP<IKeyboard> keyboard);
    void                                                 move(const IPointer::SMotionEvent& event);
    void                                                 warp(const IPointer::SMotionAbsoluteEvent& event);
    void                                                 button(const IPointer::SButtonEvent& event, IPointer* device);
    void                                                 axis(const IPointer::SAxisEvent& event);

    UP<CWLSeatProtocol>                                  m_protocol;
    UP<CSeatManager>                                     m_manager;
    UP<CInputMethodRelay>                                m_relay;
    UP<Pointer::CPointerManager>                         m_pointer;
    SP<Desktop::View::CWLSurface>                        m_cursorSurface;
    UP<CXCursorManager>                                  m_cursorTheme;
    UP<Layout::Supplementary::CDragStateController>      m_dragController;
    PHLMONITORREF                                        m_monitor;
    PHLMONITORREF                                        m_homeOutput;
    PHLWORKSPACE                                         m_workspace;
    PHLWINDOWREF                                         m_window;
    bool                                                 m_active              = true;
    bool                                                 m_paused              = false;
    bool                                                 m_managedControl      = false;
    bool                                                 m_continueOnHumanLock = false;
    uint64_t                                             m_controlGeneration   = 1;
    uint64_t                                             m_viewEpoch           = 1;
    std::unordered_map<IHID*, uint64_t>                  m_deviceGenerations;
    std::unordered_map<wl_client*, UP<SSeatInputClient>> m_inputClients;
    Hyprutils::OS::CFileDescriptor                       m_socketFd;
    wl_event_source*                                     m_socketSource = nullptr;
    std::string                                          m_socketName;
    std::string                                          m_captureGrant;
    std::string                                          m_socketPath;
    std::vector<SP<IKeyboard>>                           m_keyboards;
    std::vector<SP<IPointer>>                            m_pointers;
    std::vector<uint32_t>                                m_buttons;
    std::unordered_map<IPointer*, std::vector<uint32_t>> m_deviceButtons;
    CHyprSignalListener                                  m_windowUnmap;
    CHyprSignalListener                                  m_windowDestroy;
    std::vector<CHyprSignalListener>                     m_listeners;
};

class CSeatDesktopRegistry {
  public:
    CSeatDesktopRegistry();
    ~CSeatDesktopRegistry();
    std::string                          create(const std::string& name, PHLMONITOR monitor);
    std::string                          remove(const std::string& name);
    CSeatDesktop*                        forName(const std::string& name) const;
    CSeatDesktop*                        forManager(CSeatManager* manager) const;
    CSeatDesktop*                        forClient(const wl_client* client) const;
    bool                                 allowsGlobal(const wl_client* client, const wl_global* global) const;
    void                                 refreshWorkspace(PHLWORKSPACE workspace);
    bool                                 isPrivateOutput(PHLMONITOR monitor) const;
    std::string                          registerPrivateOutput(PHLMONITOR monitor);
    std::string                          createPrivateOutput(const std::string& name);
    bool                                 isSeatGlobal(const wl_global* global) const;
    bool                                 usesWorkspace(PHLWORKSPACE workspace) const;
    bool                                 focusesWindow(PHLWINDOW window) const;
    const std::vector<UP<CSeatDesktop>>& seats() const;
    std::string                          windowIdentity(PHLWINDOW window);

  private:
    // Retain retired seats until their socket clients disconnect: Wayland
    // children may outlive wl_seat and removal of a registry global.
    void                                                            collectRetired();
    std::vector<UP<CSeatDesktop>>                                   m_seats;
    UP<IWaylandProtocol>                                            m_sharedPrimarySeat;
    std::vector<PHLMONITORREF>                                      m_privateOutputs;
    std::unordered_set<std::string>                                 m_pendingPrivateOutputs;
    CHyprSignalListener                                             m_privateOutputAdded;
    SP<CEventLoopTimer>                                             m_frameTimer;
    uint64_t                                                        m_nextWindowIdentity = 0;
    std::unordered_map<void*, std::pair<PHLWINDOWREF, std::string>> m_windowIdentities;
};

inline UP<CSeatDesktopRegistry> g_pSeatDesktopRegistry;
