#include "WaylandSocket.hpp"
#include <wayland-server-core.h>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct CWaylandSocket::SDisplayRegistration {
    SDisplayRegistration(CWaylandSocket* socket, wl_display* display) : owner(socket) {
        wl_list_init(&destroy.link);
        destroy.notify = [](wl_listener* listener, void*) {
            SDisplayRegistration* registration = wl_container_of(listener, registration, destroy);
            registration->owner->stop();
            registration->owner->m_display = nullptr;
            registration->detach();
        };
        wl_display_add_destroy_listener(display, &destroy);
    }

    ~SDisplayRegistration() {
        detach();
    }

    void detach() {
        wl_list_remove(&destroy.link);
        wl_list_init(&destroy.link);
    }

    wl_listener     destroy = {};
    CWaylandSocket* owner   = nullptr;
};

CWaylandSocket::CWaylandSocket(wl_display* display, const std::string& path, std::function<void(wl_client*)> onClient) :
    m_display(display), m_path(path), m_onClient(std::move(onClient)) {
    if (!m_display || !m_onClient || m_path.empty())
        throw std::invalid_argument("Wayland socket requires a display, path and client callback");
    sockaddr_un address = {.sun_family = AF_UNIX, .sun_path = {}};
    if (m_path.size() >= sizeof(address.sun_path))
        throw std::invalid_argument("Wayland socket path exceeds AF_UNIX limit");
    std::ranges::copy(m_path, address.sun_path);
    m_fd = Hyprutils::OS::CFileDescriptor{socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (!m_fd.isValid() || bind(m_fd.get(), rc<sockaddr*>(&address), sizeof(address)) < 0)
        throw std::system_error(errno, std::generic_category(), "creating Wayland socket");
    m_bound                             = true;
    bool                          ready = false;
    Hyprutils::Utils::CScopeGuard cleanup([this, &ready] {
        if (!ready)
            stop();
    });
    std::filesystem::permissions(m_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    if (listen(m_fd.get(), 16) < 0)
        throw std::system_error(errno, std::generic_category(), "listening on Wayland socket");
    m_source = wl_event_loop_add_fd(wl_display_get_event_loop(m_display), m_fd.get(), WL_EVENT_READABLE, acceptClient, this);
    if (!m_source)
        throw std::runtime_error("registering Wayland socket failed");
    m_displayRegistration = makeUnique<SDisplayRegistration>(this, m_display);
    ready                 = true;
}

CWaylandSocket::~CWaylandSocket() {
    m_displayRegistration.reset();
    stop();
}

void CWaylandSocket::stop() {
    if (m_source) {
        wl_event_source_remove(m_source);
        m_source = nullptr;
    }
    m_fd.reset();
    if (!m_bound)
        return;
    std::error_code error;
    std::filesystem::remove(m_path, error);
    m_bound = false;
}

int CWaylandSocket::acceptClient(int fd, uint32_t mask, void* data) {
    auto socket = sc<CWaylandSocket*>(data);
    if (!socket->m_display || !(mask & WL_EVENT_READABLE))
        return 0;
    while (true) {
        Hyprutils::OS::CFileDescriptor connection{accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)};
        if (!connection.isValid()) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        ucred     credentials = {};
        socklen_t length      = sizeof(credentials);
        if (getsockopt(connection.get(), SOL_SOCKET, SO_PEERCRED, &credentials, &length) < 0 || length != sizeof(credentials) || credentials.uid != geteuid())
            continue;
        const auto client = wl_client_create(socket->m_display, connection.get());
        if (!client)
            continue;
        connection.take();
        try {
            socket->m_onClient(client);
        } catch (...) { wl_client_destroy(client); }
    }
}
