#include "ShortcutsInhibit.hpp"
#include <algorithm>
#include "../Compositor.hpp"
#include "../desktop/state/FocusState.hpp"
#include "core/Compositor.hpp"
#include "core/Seat.hpp"
#include "../managers/SeatManager.hpp"

CKeyboardShortcutsInhibitor::CKeyboardShortcutsInhibitor(SP<CZwpKeyboardShortcutsInhibitorV1> resource_, SP<CWLSurfaceResource> surf, SP<CWLSeatResource> seat) :
    m_resource(resource_), m_surface(surf), m_seat(seat) {
    if UNLIKELY (!m_resource->resource())
        return;

    m_resource->setDestroy([this](CZwpKeyboardShortcutsInhibitorV1* pMgr) { PROTO::shortcutsInhibit->destroyInhibitor(this); });
    m_resource->setOnDestroy([this](CZwpKeyboardShortcutsInhibitorV1* pMgr) { PROTO::shortcutsInhibit->destroyInhibitor(this); });

    // I don't really care about following the spec here that much,
    // let's make the app believe it's always active
    m_resource->sendActive();
}

SP<CWLSurfaceResource> CKeyboardShortcutsInhibitor::surface() {
    return m_surface.lock();
}

CSeatManager* CKeyboardShortcutsInhibitor::manager() const {
    return m_seat->manager();
}

bool CKeyboardShortcutsInhibitor::good() {
    return m_resource->resource();
}

CKeyboardShortcutsInhibitProtocol::CKeyboardShortcutsInhibitProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    ;
}

void CKeyboardShortcutsInhibitProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_managers.emplace_back(makeUnique<CZwpKeyboardShortcutsInhibitManagerV1>(client, ver, id)).get();
    RESOURCE->setOnDestroy([this](CZwpKeyboardShortcutsInhibitManagerV1* p) { this->onManagerResourceDestroy(p->resource()); });

    RESOURCE->setDestroy([this](CZwpKeyboardShortcutsInhibitManagerV1* pMgr) { this->onManagerResourceDestroy(pMgr->resource()); });
    RESOURCE->setInhibitShortcuts(
        [this](CZwpKeyboardShortcutsInhibitManagerV1* pMgr, uint32_t id, wl_resource* surface, wl_resource* seat) { this->onInhibit(pMgr, id, surface, seat); });
}

void CKeyboardShortcutsInhibitProtocol::onManagerResourceDestroy(wl_resource* res) {
    std::erase_if(m_managers, [&](const auto& other) { return other->resource() == res; });
}

void CKeyboardShortcutsInhibitProtocol::destroyInhibitor(CKeyboardShortcutsInhibitor* inhibitor) {
    std::erase_if(m_inhibitors, [&](const auto& other) { return other.get() == inhibitor; });
}

void CKeyboardShortcutsInhibitProtocol::onInhibit(CZwpKeyboardShortcutsInhibitManagerV1* pMgr, uint32_t id, wl_resource* surface, wl_resource* seat) {
    SP<CWLSurfaceResource> surf   = CWLSurfaceResource::fromResource(surface);
    const auto             CLIENT = pMgr->client();
    const auto             SEAT   = CWLSeatResource::fromResource(seat);

    for (auto const& in : m_inhibitors) {
        if LIKELY (in->surface() != surf || in->manager() != SEAT->manager())
            continue;

        pMgr->error(ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_ERROR_ALREADY_INHIBITED, "Already inhibited for this surface and seat");
        return;
    }

    const auto RESOURCE =
        m_inhibitors.emplace_back(makeUnique<CKeyboardShortcutsInhibitor>(makeShared<CZwpKeyboardShortcutsInhibitorV1>(CLIENT, pMgr->version(), id), surf, SEAT)).get();

    if UNLIKELY (!RESOURCE->good()) {
        pMgr->noMemory();
        m_inhibitors.pop_back();
        LOG(Log::ERR, "Failed to create an inhibitor resource");
        return;
    }
}

bool CKeyboardShortcutsInhibitProtocol::isInhibited(CSeatManager* seat) {
    seat             = seat ? seat : g_pSeatManager.get();
    const auto FOCUS = seat->m_state.keyboardFocus.lock();
    if (!FOCUS)
        return false;

    if (const auto PWINDOW = Desktop::viewState()->query().type(Desktop::View::VIEW_TYPE_WINDOW).surface(FOCUS).runWindow();
        PWINDOW && PWINDOW->m_ruleApplicator->noShortcutsInhibit().valueOrDefault())
        return false;

    for (auto const& in : m_inhibitors) {
        if (in->manager() != seat || in->surface() != FOCUS)
            continue;

        return true;
    }

    return false;
}
