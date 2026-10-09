#pragma once

#include <vector>
#include <cstdint>
#include <variant>
#include "WaylandProtocol.hpp"
#include "ext-session-lock-v1.hpp"
#include "hyprland-lock-scope-v1.hpp"
#include "../helpers/signal/Signal.hpp"

using TLockResource        = std::variant<SP<CExtSessionLockV1>, SP<CHyprlandLockScopeV1>>;
using TLockSurfaceResource = std::variant<SP<CExtSessionLockSurfaceV1>, SP<CHyprlandLockScopeSurfaceV1>>;

class CSeatDesktop;
class CSessionLock;
class CWLSurfaceResource;

class CSessionLockSurface {
  public:
    CSessionLockSurface(TLockSurfaceResource resource_, SP<CWLSurfaceResource> surface_, PHLMONITOR pMonitor_, WP<CSessionLock> owner_);
    ~CSessionLockSurface();

    bool                   good();
    bool                   inert();
    PHLMONITOR             monitor();
    SP<CWLSurfaceResource> surface();

    struct {
        CSignalT<> map;
        CSignalT<> destroy;
        CSignalT<> commit;
    } m_events;

  private:
    TLockSurfaceResource                                  m_resource;
    WP<CSessionLock>                                      m_sessionLock;
    WP<CWLSurfaceResource>                                m_surface;
    PHLMONITORREF                                         m_monitor;

    std::vector<std::pair<uint32_t, std::pair<int, int>>> m_configures;
    std::pair<int, int>                                   m_ackSize;
    bool                                                  m_ackdConfigure = false;
    bool                                                  m_committed     = false;

    void                                                  sendConfigure();

    struct {
        CHyprSignalListener monitorMode;
        CHyprSignalListener surfaceCommit;
        CHyprSignalListener surfaceDestroy;
    } m_listeners;
};

class CSessionLock {
  public:
    CSessionLock(TLockResource resource_);
    ~CSessionLock();

    bool good();
    void sendLocked();
    void sendDenied();
    bool scoped() const;
    bool allowsSeatInput(const CSeatDesktop* seat) const;
    bool excludesOutput(PHLMONITOR monitor) const;
    bool inert() const {
        return m_inert;
    }
    wl_resource* resource() const;

    struct {
        CSignalT<SP<CSessionLockSurface>> newLockSurface;
        CSignalT<>                        unlockAndDestroy;
        CSignalT<>                        destroyed; // fires regardless of whether there was a unlockAndDestroy or not.
    } m_events;

  private:
    TLockResource m_resource;

    struct SSeatExemption {
        std::string name, identity;
        uint64_t    generation;
    };
    std::vector<SSeatExemption> m_allowedSeats;
    std::vector<PHLMONITORREF>  m_excludedOutputs;
    bool                        m_activated = false;
    bool                        m_inert     = false;

    friend class CSessionLockProtocol;
};

class CSessionLockProtocol : public IWaylandProtocol {
  public:
    CSessionLockProtocol(const wl_interface* iface, const int& ver, const std::string& name);

    virtual void bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id);

    void         bindScopeManager(wl_client* client, uint32_t ver, uint32_t id);
    bool         isLocked();
    void         forceUnlock();
    void         forceLock();
    void         abandonForFullLock();

    struct {
        CSignalT<SP<CSessionLock>> newLock;
    } m_events;

  private:
    void onManagerResourceDestroy(wl_resource* res);
    void destroyResource(CSessionLock* lock);
    void destroyResource(CSessionLockSurface* surf);
    void onLock(CExtSessionLockManagerV1* pMgr, uint32_t id);
    void activate(CSessionLock* lock);
    void onGetLockSurface(CSessionLock* lock, uint32_t id, wl_resource* surface, wl_resource* output);

    bool m_locked = false;

    //
    std::vector<UP<CExtSessionLockManagerV1>>    m_managers;
    std::vector<UP<CHyprlandLockScopeManagerV1>> m_scopeManagers;
    std::vector<UP<CHyprlandLockGuardV1>>        m_guards;
    std::vector<SP<CSessionLock>>                m_locks;
    std::vector<SP<CSessionLockSurface>>         m_lockSurfaces;

    friend class CSessionLock;
    friend class CSessionLockSurface;
};

class CLockScopeProtocol : public IWaylandProtocol {
  public:
    CLockScopeProtocol(const wl_interface* iface, int ver, const std::string& name);
    void bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) override;
};

namespace PROTO {
    inline UP<CSessionLockProtocol> sessionLock;
    inline UP<CLockScopeProtocol>   lockScope;
};
