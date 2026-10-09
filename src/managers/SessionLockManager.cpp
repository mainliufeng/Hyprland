#include "SessionLockManager.hpp"
#include "../Compositor.hpp"
#include "../config/ConfigValue.hpp"
#include "../protocols/FractionalScale.hpp"
#include "../protocols/SessionLock.hpp"
#include "../render/Renderer.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../desktop/view/SessionLock.hpp"
#include "../managers/SeatManager.hpp"
#include "SeatDesktop.hpp"
#include "screenshare/ScreenshareManager.hpp"
#include "../managers/input/InputManager.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include "../state/MonitorState.hpp"
#include "../layout/LayoutManager.hpp"
#include "../layout/supplementary/DragController.hpp"
#include "../protocols/core/DataDevice.hpp"
#include "input/UnifiedWorkspaceSwipeGesture.hpp"
#include "../ipc/s2/S2.hpp"
#include "../event/EventBus.hpp"
#include <algorithm>
#include <ranges>

SSessionLockSurface::SSessionLockSurface(SP<CSessionLockSurface> surface_) : surface(surface_) {
    pWlrSurface = surface->surface();

    listeners.map = surface_->m_events.map.listen([this] {
        mapped = true;

        g_pInputManager->simulateMouseMovement();

        const auto PMONITOR = State::monitorState()->query().id(iMonitorID).run();

        if (PMONITOR)
            g_pHyprRenderer->damageMonitor(PMONITOR);
    });

    listeners.destroy = surface_->m_events.destroy.listen([this] {
        if (pWlrSurface == Desktop::focusState()->surface())
            Desktop::focusState()->surface().reset();

        g_pSessionLockManager->removeSessionLockSurface(this);
    });

    listeners.commit = surface_->m_events.commit.listen([this] {
        const auto PMONITOR = State::monitorState()->query().id(iMonitorID).run();

        if (mapped && !Desktop::focusState()->surface())
            g_pInputManager->simulateMouseMovement();

        if (PMONITOR)
            g_pHyprRenderer->damageMonitor(PMONITOR);
    });
}

CSessionLockManager::CSessionLockManager() {
    m_listeners.newLock = PROTO::sessionLock->m_events.newLock.listen([this](const auto& lock) { this->onNewSessionLock(lock); });
    m_outputListeners.emplace_back(Event::bus()->m_events.monitor.added.listen([this](PHLMONITOR monitor) {
        watchOutput(monitor);
        if (isSessionLocked() && protectsOutput(monitor))
            invalidateOutputs();
    }));
    m_outputListeners.emplace_back(Event::bus()->m_events.monitor.removed.listen([this](PHLMONITOR monitor) {
        if (isSessionLocked() && protectsOutput(monitor))
            invalidateOutputs();
    }));
}

void CSessionLockManager::onNewSessionLock(SP<CSessionLock> pLock) {
    static auto PALLOWRELOCK = CConfigValue<Config::INTEGER>("misc:allow_session_lock_restore");

    const bool  upgrading = PROTO::sessionLock->isLocked() && m_scoped && !pLock->scoped();
    if (PROTO::sessionLock->isLocked() && ((!m_scoped && pLock->scoped()) || (m_sessionLock && !upgrading))) {
        pLock->sendDenied();
        return;
    }
    const bool recoveringFull  = PROTO::sessionLock->isLocked() && !m_scoped && !pLock->scoped() && !m_sessionLock;
    const bool recoveringHuman = PROTO::sessionLock->isLocked() && m_scoped && pLock->scoped() && !m_sessionLock;
    if (PROTO::sessionLock->isLocked() && !upgrading && !recoveringHuman && !recoveringFull && !*PALLOWRELOCK && g_pCompositor->m_startLockedCommand.empty()) {
        LOG(Log::DEBUG, "Cannot re-lock, misc:allow_session_lock_restore is disabled");
        pLock->sendDenied();
        return;
    }

    if (!upgrading && m_sessionLock && !clientDenied() && !clientLocked())
        return; // Not allowing to relock in case the old lock is still in a limbo

    LOG(Log::DEBUG, "Session got locked by {:x}", (uintptr_t)pLock.get());

    if (upgrading && m_sessionLock && m_sessionLock->lock)
        m_sessionLock->lock->sendDenied();
    m_scoped = pLock->scoped();
    advanceLockEpoch();
    m_renderedLocks.clear();
    m_presentedLocks.clear();
    m_lockId            = m_lockEpoch;
    m_sessionLock       = makeUnique<SSessionLock>();
    m_sessionLock->lock = pLock;
    m_sessionLock->lockTimer.reset();

    m_sessionLock->listeners.newSurface = pLock->m_events.newLockSurface.listen([this](const SP<CSessionLockSurface>& surface) {
        const auto PMONITOR = surface->monitor();

        const auto NEWSURFACE  = m_sessionLock->vSessionLockSurfaces.emplace_back(makeShared<SSessionLockSurface>(surface));
        NEWSURFACE->iMonitorID = PMONITOR->m_id;
        PROTO::fractional->sendScale(surface->surface(), PMONITOR->m_scale);

        NEWSURFACE->view = Desktop::View::CSessionLock::create(surface);
    });

    m_sessionLock->listeners.unlock = pLock->m_events.unlockAndDestroy.listen([this] {
        advanceLockEpoch();
        m_scoped = false;
        m_events.unlock.emit();

        m_sessionLock.reset();
        g_pInputManager->refocus();

        for (auto const& m : State::monitorState()->monitors())
            g_pHyprRenderer->damageMonitor(m);
    });

    m_sessionLock->listeners.destroy = pLock->m_events.destroyed.listen([this] {
        if (m_scoped) {
            abandonToFullLock();
            return;
        }
        m_sessionLock.reset();
        Desktop::focusState()->rawSurfaceFocus(nullptr);

        for (auto const& m : State::monitorState()->monitors())
            g_pHyprRenderer->damageMonitor(m);
    });

    // End physical grabs and held pointer/touch actions before routing to the
    // lock owner. Independent seat managers are handled by lock listeners.
    g_pSeatManager->setGrab(nullptr);
    g_layoutManager->dragController()->dragEnd();
    PROTO::data->abortDndIfPresent(g_pSeatManager.get());
    g_pInputManager->releaseAllMouseButtons();
    g_pSeatManager->setPointerFocus(nullptr, {});
    g_pSeatManager->sendTouchCancel();
    g_pInputManager->m_touchData.touchFocusSurface.reset();
    g_pInputManager->m_touchData.touchFocusWindow.reset();
    g_pInputManager->m_touchData.touchFocusLS.reset();
    g_pInputManager->m_touchData.touchFocusLockSurface.reset();
    if (g_pUnifiedWorkspaceSwipe && g_pUnifiedWorkspaceSwipe->isGestureInProgress())
        g_pUnifiedWorkspaceSwipe->cancel();
    g_pInputManager->m_touchData.workspaceSwipe.reset();
    IPC::Socket2::sock()->postEvent({"sessionlock", "locked"});
    m_events.lock.emit();

    Desktop::focusState()->rawSurfaceFocus(nullptr);
    g_pSeatManager->setGrab(nullptr);

    const bool NOACTIVEMONS = std::ranges::all_of(State::monitorState()->monitors(), [this](const auto& m) { return !protectsOutput(m) || !m->m_enabled || confirmedOff(m); });

    if (NOACTIVEMONS) {
        // Normally the locked event is sent after each output rendered a lock screen frame.
        // When there are no active outputs, send it right away.
        m_sessionLock->lock->sendLocked();
        m_sessionLock->hasSentLocked = true;
        return;
    }

    for (const auto& monitor : State::monitorState()->monitors())
        g_pHyprRenderer->damageMonitor(monitor);
    // secure is confirmed by actual presentation, never by a timeout.
}

void CSessionLockManager::removeSendLockedTimer() {
    if (!m_sessionLock || !m_sessionLock->sendLockedTimer)
        return;

    g_pEventLoopManager->removeTimer(m_sessionLock->sendLockedTimer);
    m_sessionLock->sendLockedTimer.reset();
}

bool CSessionLockManager::isSessionLocked() {
    return PROTO::sessionLock->isLocked();
}

WP<SSessionLockSurface> CSessionLockManager::getSessionLockSurfaceForMonitor(uint64_t id) {
    if (!m_sessionLock)
        return {};

    for (auto const& sls : m_sessionLock->vSessionLockSurfaces) {
        if (sls->iMonitorID == id) {
            if (sls->mapped)
                return sls;
            else
                return {};
        }
    }

    return {};
}

void CSessionLockManager::onLockscreenRenderedOnMonitor(uint64_t id) {
    if (isSessionLocked())
        m_renderedLocks[id] = m_lockEpoch;
}

uint64_t CSessionLockManager::takeRenderedLock(uint64_t id) {
    const auto it = m_renderedLocks.find(id);
    if (it == m_renderedLocks.end())
        return 0;
    const auto epoch = it->second;
    m_renderedLocks.erase(it);
    return epoch;
}

void CSessionLockManager::onLockscreenPresented(uint64_t id, uint64_t epoch) {
    if (!isSessionLocked() || epoch != m_lockEpoch)
        return;
    m_presentedLocks[id] = epoch;
    if (!m_sessionLock || m_sessionLock->hasSentLocked || m_sessionLock->hasSentDenied)
        return;
    const bool LOCKED = outputsSecure();

    if (LOCKED && m_sessionLock->lock->good()) {
        removeSendLockedTimer();
        m_sessionLock->lock->sendLocked();
        m_sessionLock->hasSentLocked = true;
    }
}

bool CSessionLockManager::isSurfaceSessionLock(SP<CWLSurfaceResource> pSurface) {
    // TODO: this has some edge cases when it's wrong (e.g. destroyed lock but not yet surfaces)
    // but can be easily fixed when I rewrite wlr_surface

    if (!m_sessionLock)
        return false;

    for (auto const& sls : m_sessionLock->vSessionLockSurfaces) {
        if (sls->surface->surface() == pSurface)
            return true;
    }

    return false;
}

bool CSessionLockManager::anySessionLockSurfacesPresent() {
    return m_sessionLock && std::ranges::any_of(m_sessionLock->vSessionLockSurfaces, [](const auto& surf) { return surf->mapped; });
}

void CSessionLockManager::removeSessionLockSurface(SSessionLockSurface* pSLS) {
    if (!m_sessionLock)
        return;

    std::erase_if(m_sessionLock->vSessionLockSurfaces, [&](const auto& other) { return pSLS == other.get(); });

    if (Desktop::focusState()->surface())
        return;

    for (auto const& sls : m_sessionLock->vSessionLockSurfaces) {
        if (!sls->mapped)
            continue;

        Desktop::focusState()->rawSurfaceFocus(sls->surface->surface());
        break;
    }
}

bool CSessionLockManager::clientLocked() {
    return m_sessionLock && m_sessionLock->hasSentLocked;
}

bool CSessionLockManager::clientDenied() {
    return m_sessionLock && m_sessionLock->hasSentDenied;
}

void CSessionLockManager::clearSessionLock() {
    advanceLockEpoch();
    m_scoped = false;
    m_events.unlock.emit();
    m_sessionLock = {};
}

void CSessionLockManager::forceUnlock() {
    PROTO::sessionLock->forceUnlock();
    clearSessionLock();

    Desktop::focusState()->rawSurfaceFocus(nullptr);
    for (auto const& m : State::monitorState()->monitors())
        g_pHyprRenderer->damageMonitor(m);

    g_pInputManager->refocus();
}

void CSessionLockManager::abandonToFullLock() {
    m_sessionLock.reset();
    m_scoped = false;
    advanceLockEpoch();
    m_lockId = m_lockEpoch;
    m_renderedLocks.clear();
    m_presentedLocks.clear();
    PROTO::sessionLock->abandonForFullLock();
    m_events.lock.emit();
    // End physical grabs and held pointer/touch actions before routing to the
    // lock owner. Independent seat managers are handled by lock listeners.
    g_pSeatManager->setGrab(nullptr);
    g_layoutManager->dragController()->dragEnd();
    PROTO::data->abortDndIfPresent(g_pSeatManager.get());
    g_pInputManager->releaseAllMouseButtons();
    g_pSeatManager->setPointerFocus(nullptr, {});
    g_pSeatManager->sendTouchCancel();
    g_pInputManager->m_touchData.touchFocusSurface.reset();
    g_pInputManager->m_touchData.touchFocusWindow.reset();
    g_pInputManager->m_touchData.touchFocusLS.reset();
    g_pInputManager->m_touchData.touchFocusLockSurface.reset();
    if (g_pUnifiedWorkspaceSwipe && g_pUnifiedWorkspaceSwipe->isGestureInProgress())
        g_pUnifiedWorkspaceSwipe->cancel();
    g_pInputManager->m_touchData.workspaceSwipe.reset();
    IPC::Socket2::sock()->postEvent({"sessionlock", "locked"});
    Desktop::focusState()->rawSurfaceFocus(nullptr);
    for (const auto& monitor : State::monitorState()->monitors())
        g_pHyprRenderer->damageMonitor(monitor);
}

void CSessionLockManager::forceLock() {
    m_scoped = false;
    advanceLockEpoch();
    PROTO::sessionLock->forceLock();
    m_events.lock.emit();
}

bool CSessionLockManager::shallConsiderLockMissing() {
    if (!m_sessionLock)
        return true;

    static auto LOCKDEAD_SCREEN_DELAY = CConfigValue<Config::INTEGER>("misc:lockdead_screen_delay");

    return m_sessionLock->lockTimer.getMillis() > *LOCKDEAD_SCREEN_DELAY;
}

bool CSessionLockManager::scoped() const {
    return m_scoped;
}
uint64_t CSessionLockManager::lockEpoch() const {
    return m_lockEpoch;
}
bool CSessionLockManager::protectsOutput(PHLMONITOR monitor) const {
    return !m_scoped || !m_sessionLock || !m_sessionLock->lock || !m_sessionLock->lock->excludesOutput(monitor);
}
bool CSessionLockManager::allowsSeatInput(const CSeatDesktop* seat) const {
    return m_scoped && m_sessionLock && m_sessionLock->lock && m_sessionLock->lock->allowsSeatInput(seat);
}

void CSessionLockManager::watchOutput(PHLMONITOR monitor) {
    m_outputListeners.emplace_back(monitor->m_events.modeChanged.listen([this, output = PHLMONITORREF{monitor}] {
        if (output && isSessionLocked() && protectsOutput(output.lock()))
            invalidateOutputs();
    }));
}
void CSessionLockManager::advanceLockEpoch() {
    ++m_lockEpoch;
    if (Screenshare::mgr())
        Screenshare::mgr()->discardInvalidatedFrames();
}

void CSessionLockManager::invalidateOutputs() {
    advanceLockEpoch();
    m_presentedLocks.clear();
    m_renderedLocks.clear();
    for (const auto& monitor : State::monitorState()->monitors())
        g_pHyprRenderer->damageMonitor(monitor);
}
bool CSessionLockManager::confirmedOff(PHLMONITOR monitor) const {
    return std::ranges::any_of(m_offOutputs, [&](const auto& output) { return output && output == monitor; });
}
void CSessionLockManager::onOutputCommit(PHLMONITOR monitor, bool enabled) {
    const bool wasOff = confirmedOff(monitor);
    std::erase_if(m_offOutputs, [&](const auto& output) { return !output || output == monitor; });
    if (!enabled)
        m_offOutputs.emplace_back(monitor);
    if (wasOff && enabled && isSessionLocked() && protectsOutput(monitor))
        invalidateOutputs();
    if (!enabled && isSessionLocked())
        onLockscreenPresented(monitor->m_id, m_lockEpoch);
}
bool CSessionLockManager::outputsSecure() const {
    if (!PROTO::sessionLock->isLocked())
        return false;
    return std::ranges::all_of(State::monitorState()->monitors(), [this](const auto& monitor) {
        const auto it = m_presentedLocks.find(monitor->m_id);
        return !protectsOutput(monitor) || !monitor->m_enabled || confirmedOff(monitor) || (it != m_presentedLocks.end() && it->second == m_lockEpoch);
    });
}
std::string CSessionLockManager::protectionStateJSON() const {
    const bool  locked  = PROTO::sessionLock->isLocked();
    std::string outputs = "[";
    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!protectsOutput(monitor))
            continue;
        if (outputs.size() > 1)
            outputs += ",";
        const auto it        = m_presentedLocks.find(monitor->m_id);
        const bool presented = it != m_presentedLocks.end() && it->second == m_lockEpoch;
        outputs += std::format("{{\"name\":\"{}\",\"id\":\"{}\",\"enabled\":{},\"offConfirmed\":{},\"covered\":{}}}", escapeJSONStrings(monitor->m_name), monitor->m_id,
                               monitor->m_enabled, confirmedOff(monitor), presented);
    }
    outputs += "]";
    return std::format(
        "{{\"scope\":\"{}\",\"phase\":\"{}\",\"locked\":{},\"secure\":{},\"ownerConnected\":{},\"lockId\":\"{}\",\"epoch\":\"{}\",\"lockEpoch\":\"{}\",\"protectedOutputs\":{}}}",
        locked ? (m_scoped ? "scoped" : "session") : "none",
        !locked             ? "unlocked" :
            !m_sessionLock  ? "orphaned" :
            outputsSecure() ? "secure" :
                              "preparing",
        locked, outputsSecure(), m_sessionLock != nullptr, m_lockId, m_lockEpoch, m_lockEpoch, outputs);
}
