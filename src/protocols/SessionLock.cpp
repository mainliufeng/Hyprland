#include "SessionLock.hpp"
#include "../managers/SeatManager.hpp"
#include "FractionalScale.hpp"
#include "LockNotify.hpp"
#include "core/Compositor.hpp"
#include "core/Output.hpp"
#include "../output/Monitor.hpp"
#include "../render/Renderer.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../managers/SeatDesktop.hpp"
#include "../managers/SessionLockManager.hpp"

class CLockSurfaceRole : public ISurfaceRole {
  public:
    eSurfaceRole role() override;
};
eSurfaceRole CLockSurfaceRole::role() {
    return SURFACE_ROLE_SESSION_LOCK;
}

CSessionLockSurface::CSessionLockSurface(TLockSurfaceResource resource_, SP<CWLSurfaceResource> surface_, PHLMONITOR pMonitor_, WP<CSessionLock> owner_) :
    m_resource(resource_), m_sessionLock(owner_), m_surface(surface_), m_monitor(pMonitor_) {
    if UNLIKELY (!good())
        return;
    std::visit(
        [this](auto& r) {
            r->setDestroy([this](auto*) {
                m_events.destroy.emit();
                PROTO::sessionLock->destroyResource(this);
            });
            r->setOnDestroy([this](auto*) {
                m_events.destroy.emit();
                PROTO::sessionLock->destroyResource(this);
            });
            r->setAckConfigure([this](auto* resource, uint32_t serial) {
                if (inert())
                    return;
                const auto found = std::ranges::find_if(m_configures, [serial](const auto& c) { return c.first == serial; });
                if (found == m_configures.end()) {
                    resource->error(3, "Unknown or already acknowledged configure serial");
                    return;
                }
                m_ackSize       = found->second;
                m_ackdConfigure = true;
                m_configures.erase(m_configures.begin(), std::next(found));
            });
        },
        m_resource);

    m_listeners.surfaceCommit = m_surface->m_events.commit.listen([this] {
        if (inert())
            return;
        if (!m_surface->m_current.texture) {
            LOG(Log::ERR, "SessionLock attached a null buffer");
            std::visit([](auto& r) { r->error(1, "Null buffer attached"); }, m_resource);
            return;
        }

        if (!m_ackdConfigure) {
            LOG(Log::ERR, "SessionLock committed without an ack");
            std::visit([](auto& r) { r->error(0, "Commit before valid configure ack"); }, m_resource);
            return;
        }

        if (m_surface->m_current.size.x != m_ackSize.first || m_surface->m_current.size.y != m_ackSize.second) {
            std::visit([](auto& r) { r->error(2, "Buffer dimensions differ from acknowledged configure"); }, m_resource);
            return;
        }

        if (m_committed)
            m_events.commit.emit();
        else {
            m_surface->map();
            m_events.map.emit();
        }
        m_committed = true;
    });

    m_listeners.surfaceDestroy = m_surface->m_events.destroy.listen([this] {
        LOG(Log::WARN, "SessionLockSurface object remains but surface is being destroyed???");
        m_surface->unmap();
        m_listeners.surfaceCommit.reset();
        m_listeners.surfaceDestroy.reset();
        if (Desktop::focusState()->surface() == m_surface)
            Desktop::focusState()->surface().reset();

        m_surface.reset();
    });

    if (m_monitor) {
        PROTO::fractional->sendScale(surface_, m_monitor->m_scale);

        if (m_surface)
            m_surface->enter(m_monitor.lock());

        m_listeners.monitorMode = m_monitor->m_events.modeChanged.listen([this] { sendConfigure(); });
    }

    sendConfigure();
}

CSessionLockSurface::~CSessionLockSurface() {
    if (m_surface && m_surface->m_mapped)
        m_surface->unmap();
    m_listeners.surfaceCommit.reset();
    m_listeners.surfaceDestroy.reset();
    m_events.destroy.emit(); // just in case.
}

void CSessionLockSurface::sendConfigure() {
    if (!m_monitor) {
        LOG(Log::ERR, "sendConfigure: monitor is gone");
        return;
    }

    const auto SERIAL = g_pSeatManager->nextSerial(g_pSeatManager->seatResourceForClient(std::visit([](const auto& r) { return r->client(); }, m_resource)));
    m_configures.emplace_back(SERIAL, std::pair<int, int>{m_monitor->m_size.x, m_monitor->m_size.y});
    std::visit([&](auto& r) { r->sendConfigure(SERIAL, m_monitor->m_size.x, m_monitor->m_size.y); }, m_resource);
}

bool CSessionLockSurface::good() {
    return std::visit([](const auto& r) { return r->resource() != nullptr; }, m_resource);
}

bool CSessionLockSurface::inert() {
    return m_sessionLock.expired() || m_sessionLock->inert();
}

PHLMONITOR CSessionLockSurface::monitor() {
    return m_monitor.lock();
}

SP<CWLSurfaceResource> CSessionLockSurface::surface() {
    return m_surface.lock();
}

CSessionLock::CSessionLock(TLockResource resource_) : m_resource(resource_) {
    if UNLIKELY (!good())
        return;

    std::visit(
        [this](auto& resource) {
            resource->setDestroy([this](auto* r) {
                if (!m_inert && g_pSessionLockManager->clientLocked()) {
                    r->error(0, "Cannot destroy an active lock");
                    return;
                }
                PROTO::sessionLock->destroyResource(this);
            });
            resource->setOnDestroy([this](auto*) { PROTO::sessionLock->destroyResource(this); });
            resource->setGetLockSurface([this](auto*, uint32_t id, wl_resource* surf, wl_resource* output) {
                if (!m_inert)
                    PROTO::sessionLock->onGetLockSurface(this, id, surf, output);
            });
            resource->setUnlockAndDestroy([this](auto* r) {
                if (m_inert) {
                    PROTO::sessionLock->destroyResource(this);
                    return;
                }
                if (!g_pSessionLockManager->clientLocked()) {
                    r->error(1, "Lock is not secure");
                    return;
                }
                PROTO::sessionLock->m_locked = false;
                PROTO::lockNotify->onUnlocked();
                m_events.unlockAndDestroy.emit();
                g_pHyprRenderer->setCursorFromName("left_ptr");
                m_inert = true;
                PROTO::sessionLock->destroyResource(this);
            });
        },
        m_resource);
}

CSessionLock::~CSessionLock() {
    m_events.destroyed.emit();
}

void CSessionLock::sendLocked() {
    std::visit(
        [](auto& r) {
            if constexpr (requires { r->sendSecure(); })
                r->sendSecure();
            else
                r->sendLocked();
        },
        m_resource);
    PROTO::lockNotify->onLocked();
}

bool CSessionLock::good() {
    return std::visit([](const auto& r) { return r->resource() != nullptr; }, m_resource);
}

void CSessionLock::sendDenied() {
    m_inert = true;
    std::visit([](auto& r) { r->sendFinished(); }, m_resource);
}

CSessionLockProtocol::CSessionLockProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    ;
}

void CSessionLockProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_managers.emplace_back(makeUnique<CExtSessionLockManagerV1>(client, ver, id)).get();
    RESOURCE->setOnDestroy([this](CExtSessionLockManagerV1* p) { this->onManagerResourceDestroy(p->resource()); });

    RESOURCE->setDestroy([this](CExtSessionLockManagerV1* pMgr) { this->onManagerResourceDestroy(pMgr->resource()); });
    RESOURCE->setLock([this](CExtSessionLockManagerV1* pMgr, uint32_t id) { this->onLock(pMgr, id); });
}

void CSessionLockProtocol::onManagerResourceDestroy(wl_resource* res) {
    std::erase_if(m_managers, [&](const auto& other) { return other->resource() == res; });
}

void CSessionLockProtocol::destroyResource(CSessionLock* lock) {
    std::erase_if(m_locks, [&](const auto& other) { return other.get() == lock; });
}

void CSessionLockProtocol::destroyResource(CSessionLockSurface* surf) {
    std::erase_if(m_lockSurfaces, [&](const auto& other) { return other.get() == surf; });
}

void CSessionLockProtocol::onLock(CExtSessionLockManagerV1* pMgr, uint32_t id) {

    LOG(Log::DEBUG, "New sessionLock with id {}", id);

    const auto CLIENT   = pMgr->client();
    const auto RESOURCE = m_locks.emplace_back(makeShared<CSessionLock>(makeShared<CExtSessionLockV1>(CLIENT, pMgr->version(), id)));

    if UNLIKELY (!RESOURCE->good()) {
        pMgr->noMemory();
        m_locks.pop_back();
        return;
    }

    m_events.newLock.emit(RESOURCE);

    m_locked = true;
}

void CSessionLockProtocol::onGetLockSurface(CSessionLock* lock, uint32_t id, wl_resource* surface, wl_resource* output) {
    LOG(Log::DEBUG, "New sessionLockSurface with id {}", id);

    auto PSURFACE  = CWLSurfaceResource::fromResource(surface);
    auto OUTPUTRES = CWLOutputResource::fromResource(output);
    if (!OUTPUTRES) {
        LOG(Log::ERR, "onGetLockSurface: invalid output resource");
        return;
    }
    auto PMONITOR = OUTPUTRES->m_monitor.lock();
    if (!PMONITOR) {
        LOG(Log::ERR, "onGetLockSurface: monitor is gone for output resource");
        return;
    }

    SP<CSessionLock> sessionLock;
    for (const auto& l : m_locks) {
        if (l.get() == lock) {
            sessionLock = l;
            break;
        }
    }
    if (!sessionLock || (lock->humanScope() && g_pSeatDesktopRegistry->isPrivateOutput(PMONITOR))) {
        wl_resource_post_error(lock->resource(), 3, "Output is not protected by this lock");
        return;
    }
    if (!PSURFACE || (PSURFACE->m_role && PSURFACE->m_role->role() != SURFACE_ROLE_UNASSIGNED) || PSURFACE->m_current.texture) {
        wl_resource_post_error(lock->resource(), 2, "Surface already has a role or buffer");
        return;
    }
    for (const auto& existing : m_lockSurfaces) {
        if (!existing->inert() && existing->monitor() == PMONITOR) {
            wl_resource_post_error(lock->resource(), 3, "Duplicate lock output");
            return;
        }
    }
    if (PSURFACE->client() != wl_resource_get_client(lock->resource())) {
        wl_resource_post_error(lock->resource(), 2, "Foreign surface");
        return;
    }
    PSURFACE->m_role = makeShared<CLockSurfaceRole>();
    TLockSurfaceResource resource;
    if (lock->humanScope())
        resource = makeShared<CCorniceHumanLockSurfaceV1>(wl_resource_get_client(lock->resource()), 1, id);
    else
        resource = makeShared<CExtSessionLockSurfaceV1>(wl_resource_get_client(lock->resource()), 1, id);
    const auto RESOURCE = m_lockSurfaces.emplace_back(makeShared<CSessionLockSurface>(resource, PSURFACE, PMONITOR, sessionLock));
    if UNLIKELY (!RESOURCE->good()) {
        wl_resource_post_no_memory(lock->resource());
        m_lockSurfaces.pop_back();
        return;
    }
    sessionLock->m_events.newLockSurface.emit(RESOURCE);
}

bool CSessionLockProtocol::isLocked() {
    return m_locked;
}

void CSessionLockProtocol::forceUnlock() {
    m_locked = false;

    for (const auto& l : m_locks) {
        l->sendDenied();
    }

    PROTO::lockNotify->onUnlocked();
}

void CSessionLockProtocol::abandonForFullLock() {
    m_locked = true;
    for (const auto& lock : m_locks)
        if (!lock->inert())
            lock->sendDenied();
    PROTO::lockNotify->onLocked();
}

void CSessionLockProtocol::forceLock() {
    m_locked = true;

    for (const auto& l : m_locks) {
        l->sendLocked();
    }

    PROTO::lockNotify->onLocked();
}

bool CSessionLock::humanScope() const {
    return std::holds_alternative<SP<CCorniceHumanLockV1>>(m_resource);
}
wl_resource* CSessionLock::resource() const {
    return std::visit([](const auto& r) { return r->resource(); }, m_resource);
}

CHumanLockProtocol::CHumanLockProtocol(const wl_interface* iface, int ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {}
void CHumanLockProtocol::bindManager(wl_client* client, void*, uint32_t ver, uint32_t id) {
    PROTO::sessionLock->bindHumanManager(client, ver, id);
}
void CSessionLockProtocol::bindHumanManager(wl_client* client, uint32_t ver, uint32_t id) {
    if (g_pSeatDesktopRegistry->forClient(client)) {
        wl_client_post_implementation_error(client, "Agent connections cannot manage human locks");
        return;
    }
    const auto resource  = m_humanManagers.emplace_back(makeUnique<CCorniceHumanLockManagerV1>(client, ver, id)).get();
    auto       destroyed = [this](auto* r) { std::erase_if(m_humanManagers, [&](const auto& other) { return other.get() == r; }); };
    resource->setDestroy(destroyed);
    resource->setOnDestroy(destroyed);
    resource->setGetGuard([this](auto* manager, uint32_t id) {
        const auto guard   = m_guards.emplace_back(makeUnique<CCorniceSessionGuardV1>(manager->client(), 1, id)).get();
        auto       destroy = [this](auto* resource) {
            if (g_pSessionLockManager->isSessionLocked() && g_pSessionLockManager->humanScope())
                g_pSessionLockManager->abandonToFullLock();
            std::erase_if(m_guards, [&](const auto& item) { return item.get() == resource; });
        };
        guard->setDestroy(destroy);
        guard->setOnDestroy(destroy);
    });
    resource->setGetOutputRole([](auto* r, wl_resource* output) {
        const auto res = CWLOutputResource::fromResource(output);
        if (!res) {
            wl_client_post_implementation_error(r->client(), "Invalid output");
            return;
        }
        r->sendOutputRole(output, g_pSeatDesktopRegistry->isPrivateOutput(res->m_monitor.lock()));
    });
    resource->setLock([this](auto* r, uint32_t id) {
        const auto lock = m_locks.emplace_back(makeShared<CSessionLock>(makeShared<CCorniceHumanLockV1>(r->client(), 1, id)));
        m_events.newLock.emit(lock);
        m_locked = true;
    });
}
