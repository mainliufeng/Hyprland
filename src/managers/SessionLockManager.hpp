#pragma once

#include "../defines.hpp"
#include "../helpers/time/Timer.hpp"
#include "../helpers/signal/Signal.hpp"
#include "./eventLoop/EventLoopTimer.hpp"
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

class CSeatDesktop;
class CSessionLockSurface;
class CSessionLock;
class CWLSurfaceResource;

namespace Desktop::View {
    class CSessionLock;
}

struct SSessionLockSurface {
    SSessionLockSurface(SP<CSessionLockSurface> surface_);

    WP<CSessionLockSurface>         surface;
    WP<CWLSurfaceResource>          pWlrSurface;
    SP<Desktop::View::CSessionLock> view;
    uint64_t                        iMonitorID = -1;

    bool                            mapped = false;

    struct {
        CHyprSignalListener map;
        CHyprSignalListener destroy;
        CHyprSignalListener commit;
    } listeners;
};

struct SSessionLock {
    WP<CSessionLock>                     lock;
    CTimer                               lockTimer;
    SP<CEventLoopTimer>                  sendLockedTimer;

    std::vector<SP<SSessionLockSurface>> vSessionLockSurfaces;

    struct {
        CHyprSignalListener newSurface;
        CHyprSignalListener unlock;
        CHyprSignalListener destroy;
    } listeners;

    bool                         hasSentLocked = false;
    bool                         hasSentDenied = false;
    std::unordered_set<uint64_t> lockedMonitors;
};

class CSessionLockManager {
  public:
    CSessionLockManager();
    ~CSessionLockManager() = default;

    WP<SSessionLockSurface> getSessionLockSurfaceForMonitor(uint64_t);

    bool                    isSessionLocked();
    bool                    humanScope() const;
    uint64_t                lockEpoch() const;
    bool                    protectsOutput(PHLMONITOR monitor) const;
    bool                    agentMayContinue(const CSeatDesktop* seat) const;
    bool                    clientLocked();
    bool                    outputsSecure() const;
    bool                    confirmedOff(PHLMONITOR monitor) const;
    void                    onOutputCommit(PHLMONITOR monitor, bool enabled);
    std::string             protectionStateJSON() const;
    bool                    hasOwner() const {
        return m_sessionLock != nullptr;
    }
    bool     clientDenied();
    bool     isSurfaceSessionLock(SP<CWLSurfaceResource>);
    bool     anySessionLockSurfacesPresent();

    void     forceUnlock();
    void     forceLock();
    void     abandonToFullLock();

    void     removeSessionLockSurface(SSessionLockSurface*);

    void     onLockscreenRenderedOnMonitor(uint64_t id);
    uint64_t takeRenderedLock(uint64_t id);
    void     onLockscreenPresented(uint64_t id, uint64_t epoch);

    bool     shallConsiderLockMissing();

    struct {
        CSignalT<> lock;
        CSignalT<> unlock;
    } m_events;

  private:
    UP<SSessionLock>                       m_sessionLock;
    bool                                   m_humanScope = false;
    uint64_t                               m_lockEpoch  = 0;
    uint64_t                               m_lockId     = 0;
    std::unordered_map<uint64_t, uint64_t> m_presentedLocks;
    std::vector<PHLMONITORREF>             m_offOutputs;
    std::vector<CHyprSignalListener>       m_outputListeners;
    std::unordered_map<uint64_t, uint64_t> m_renderedLocks;

    struct {
        CHyprSignalListener newLock;
    } m_listeners;

    void invalidateOutputs();
    void watchOutput(PHLMONITOR monitor);
    void onNewSessionLock(SP<CSessionLock> pWlrLock);
    void removeSendLockedTimer();
    void clearSessionLock();
};

inline UP<CSessionLockManager> g_pSessionLockManager;
