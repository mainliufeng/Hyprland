#pragma once

#include "memory/Memory.hpp"
#include <hyprutils/os/FileDescriptor.hpp>
#include <cstdint>
#include <functional>
#include <string>

struct wl_client;
struct wl_display;
struct wl_event_source;

// A socket connection is classified before its registry requests are dispatched.
// This listener only accepts clients belonging to the compositor's effective UID.
class CWaylandSocket {
  public:
    CWaylandSocket(wl_display* display, const std::string& path, std::function<void(wl_client*)> onClient);
    ~CWaylandSocket();
    CWaylandSocket(const CWaylandSocket&)            = delete;
    CWaylandSocket& operator=(const CWaylandSocket&) = delete;
    CWaylandSocket(CWaylandSocket&&)                 = delete;
    CWaylandSocket& operator=(CWaylandSocket&&)      = delete;

    void            stop();

  private:
    struct SDisplayRegistration;
    static int                      acceptClient(int fd, uint32_t mask, void* data);

    wl_display*                     m_display = nullptr;
    wl_event_source*                m_source  = nullptr;
    Hyprutils::OS::CFileDescriptor  m_fd;
    std::string                     m_path;
    std::function<void(wl_client*)> m_onClient;
    UP<SDisplayRegistration>        m_displayRegistration;
    bool                            m_bound = false;
};
