#pragma once

#include "../helpers/memory/Memory.hpp"
#include "../helpers/signal/Signal.hpp"
#include <cstddef>
#include <cstdint>
#include <unordered_map>

class CSeatDesktop;
struct wl_client;
struct wl_display;

// Preferred seats belong to connections, not to client PIDs or socket paths.
// A retired seat remains referenced until all of its clients disconnect.
class CSeatClientRegistry {
  public:
    explicit CSeatClientRegistry(wl_display* display = nullptr);
    ~CSeatClientRegistry();
    CSeatClientRegistry(const CSeatClientRegistry&)            = delete;
    CSeatClientRegistry& operator=(const CSeatClientRegistry&) = delete;
    CSeatClientRegistry(CSeatClientRegistry&&)                 = delete;
    CSeatClientRegistry& operator=(CSeatClientRegistry&&)      = delete;

    void                 track(wl_client* client);
    uint64_t             idFor(const wl_client* client) const;
    uint64_t             lastId() const;
    void                 add(wl_client* client, CSeatDesktop* seat);
    void                 addManagement(wl_client* client);
    bool                 isManagementClient(const wl_client* client) const;
    CSeatDesktop*        seatFor(const wl_client* client) const;
    bool                 hasClients(const CSeatDesktop* seat) const;

    struct {
        CSignalT<uint64_t> removed;
    } m_events;

  private:
    struct SClientRegistration;
    struct SDisplayRegistration;
    void                                                          remove(const wl_client* client);

    UP<SDisplayRegistration>                                      m_displayRegistration;
    std::unordered_map<const wl_client*, UP<SClientRegistration>> m_clients;
    std::unordered_map<const CSeatDesktop*, size_t>               m_seatClients;
    uint64_t                                                      m_lastId = 0;
};
