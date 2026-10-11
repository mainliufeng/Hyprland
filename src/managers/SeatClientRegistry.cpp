#include "SeatClientRegistry.hpp"
#include <wayland-server-core.h>
#include <limits>
#include <stdexcept>

struct CSeatClientRegistry::SClientRegistration {
    SClientRegistration(CSeatClientRegistry* registry_, wl_client* client_, uint64_t id_) : registry(registry_), client(client_), id(id_) {
        wl_list_init(&destroy.link);
        destroy.notify = [](wl_listener* listener, void*) {
            SClientRegistration* registration = wl_container_of(listener, registration, destroy);
            registration->registry->remove(registration->client);
        };
        wl_client_add_destroy_listener(client, &destroy);
    }

    ~SClientRegistration() {
        wl_list_remove(&destroy.link);
    }

    wl_listener          destroy    = {};
    CSeatClientRegistry* registry   = nullptr;
    wl_client*           client     = nullptr;
    CSeatDesktop*        seat       = nullptr;
    bool                 management = false;
    const uint64_t       id         = 0;
};

struct CSeatClientRegistry::SDisplayRegistration {
    SDisplayRegistration(CSeatClientRegistry* registry_, wl_display* display) : registry(registry_) {
        wl_list_init(&created.link);
        wl_list_init(&destroy.link);
        created.notify = [](wl_listener* listener, void* client) {
            SDisplayRegistration* registration = wl_container_of(listener, registration, created);
            registration->registry->track(sc<wl_client*>(client));
        };
        destroy.notify = [](wl_listener* listener, void*) {
            SDisplayRegistration* registration = wl_container_of(listener, registration, destroy);
            registration->detach();
        };
        wl_display_add_client_created_listener(display, &created);
        wl_display_add_destroy_listener(display, &destroy);
    }

    ~SDisplayRegistration() {
        detach();
    }

    void detach() {
        wl_list_remove(&created.link);
        wl_list_init(&created.link);
        wl_list_remove(&destroy.link);
        wl_list_init(&destroy.link);
    }

    wl_listener          created  = {};
    wl_listener          destroy  = {};
    CSeatClientRegistry* registry = nullptr;
};

CSeatClientRegistry::CSeatClientRegistry(wl_display* display) {
    if (!display)
        return;
    m_displayRegistration = makeUnique<SDisplayRegistration>(this, display);
    wl_client* client     = nullptr;
    wl_client_for_each(client, wl_display_get_client_list(display)) track(client);
}

CSeatClientRegistry::~CSeatClientRegistry() {
    m_displayRegistration.reset();
}

void CSeatClientRegistry::track(wl_client* client) {
    if (!client || m_clients.contains(client))
        return;
    if (m_lastId == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("Wayland connection identity exhausted");
    const auto id = m_lastId + 1;
    m_clients.emplace(client, makeUnique<SClientRegistration>(this, client, id));
    m_lastId = id;
}

uint64_t CSeatClientRegistry::idFor(const wl_client* client) const {
    const auto found = m_clients.find(client);
    return found == m_clients.end() ? 0 : found->second->id;
}

uint64_t CSeatClientRegistry::lastId() const {
    return m_lastId;
}

void CSeatClientRegistry::add(wl_client* client, CSeatDesktop* seat) {
    // Reusing a name must not route an old connection to a replacement seat.
    if (!client || !seat)
        return;
    track(client);
    auto& registration = m_clients.at(client);
    if (registration->seat || registration->management)
        return;
    registration->seat = seat;
    ++m_seatClients[seat];
}

void CSeatClientRegistry::addManagement(wl_client* client) {
    if (!client)
        return;
    track(client);
    auto& registration = m_clients.at(client);
    if (registration->seat)
        return;
    registration->management = true;
}

bool CSeatClientRegistry::isManagementClient(const wl_client* client) const {
    const auto found = m_clients.find(client);
    return found != m_clients.end() && found->second->management;
}

CSeatDesktop* CSeatClientRegistry::seatFor(const wl_client* client) const {
    const auto found = m_clients.find(client);
    return found == m_clients.end() ? nullptr : found->second->seat;
}

bool CSeatClientRegistry::hasClients(const CSeatDesktop* seat) const {
    return m_seatClients.contains(seat);
}

void CSeatClientRegistry::remove(const wl_client* client) {
    const auto found = m_clients.find(client);
    if (found == m_clients.end())
        return;
    m_events.removed.emit(found->second->id);
    // Observers may register other clients and rehash the lookup table.
    const auto seat = m_clients.at(client)->seat;
    if (seat) {
        const auto count = m_seatClients.find(seat);
        if (--count->second == 0)
            m_seatClients.erase(count);
    }
    m_clients.erase(client);
}
