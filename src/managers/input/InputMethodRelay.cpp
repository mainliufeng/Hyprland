#include "../SeatManager.hpp"
#include "../SeatDesktop.hpp"
#include "InputMethodRelay.hpp"
#include "../../desktop/state/FocusState.hpp"
#include "../../event/EventBus.hpp"
#include "../../protocols/TextInputV3.hpp"
#include "../../protocols/TextInputV1.hpp"
#include "../../protocols/InputMethodV2.hpp"
#include "../../protocols/core/Compositor.hpp"

CInputMethodRelay::CInputMethodRelay(CSeatManager* owner) : m_seat(owner) {
    // Layer and presentation focus changes use the seat directly. Listening
    // to its actual focus also keeps the primary IME in sync with these paths.
    m_listeners.focus = seat()->m_events.keyboardFocusChange.listen([this] { onKeyboardFocus(focus()); });

    m_listeners.newTIV3 = PROTO::textInputV3->m_events.newTextInput.listen([this](const auto& input) { onNewTextInput(input); });
    m_listeners.newTIV1 = PROTO::textInputV1->m_events.newTextInput.listen([this](const auto& input) { onNewTextInput(input); });
    m_listeners.newIME  = PROTO::ime->m_events.newIME.listen([this](const auto& ime) { onNewIME(ime); });
}

void CInputMethodRelay::onNewIME(SP<CInputMethodV2> pIME) {
    if (pIME->manager() != seat())
        return;
    if (!m_inputMethod.expired()) {
        LOG(Log::ERR, "Cannot register 2 IMEs at once!");

        pIME->unavailable();

        return;
    }

    m_inputMethod = pIME;

    m_listeners.commitIME = pIME->m_events.onCommit.listen([this] {
        const auto PTI = getFocusedTextInput();

        if (!PTI) {
            LOG(Log::DEBUG, "No focused TextInput on IME Commit");
            return;
        }

        PTI->updateIMEState(m_inputMethod.lock());
    });

    m_listeners.destroyIME = pIME->m_events.destroy.listen([this] {
        const auto PTI = getFocusedTextInput();

        LOG(Log::DEBUG, "IME Destroy");

        if (PTI)
            PTI->leave();

        m_inputMethod.reset();
    });

    m_listeners.newPopup = pIME->m_events.newPopup.listen([this](const SP<CInputMethodPopupV2>& popup) {
        m_inputMethodPopups.emplace_back(makeUnique<CInputPopup>(popup, this));
        LOG(Log::DEBUG, "New input popup");
    });

    if (!focus())
        return;

    for (auto const& ti : m_textInputs) {
        if (ti->client() != focus()->client())
            continue;

        if (ti->isV3())
            ti->enter(focus());
        else
            ti->onEnabled(focus());
    }
}

void CInputMethodRelay::removePopup(CInputPopup* pPopup) {
    std::erase_if(m_inputMethodPopups, [pPopup](const auto& other) { return other.get() == pPopup; });
}

CTextInput* CInputMethodRelay::getFocusedTextInput() {
    if (!focus())
        return nullptr;

    for (auto const& ti : m_textInputs) {
        if (ti->focusedSurface() == focus() && ti->isEnabled())
            return ti.get();
    }

    for (auto const& ti : m_textInputs) {
        if (ti->focusedSurface() == focus())
            return ti.get();
    }

    return nullptr;
}

void CInputMethodRelay::onNewTextInput(WP<CTextInputV3> tiv3) {
    if (!tiv3 || tiv3->manager() != seat())
        return;
    m_textInputs.emplace_back(makeUnique<CTextInput>(tiv3, this));
}

void CInputMethodRelay::onNewTextInput(WP<CTextInputV1> pTIV1) {
    const auto desktop = g_pSeatDesktopRegistry ? g_pSeatDesktopRegistry->forClient(pTIV1->client()) : nullptr;
    if ((desktop ? desktop->manager() : g_pSeatManager.get()) != seat())
        return;
    m_textInputs.emplace_back(makeUnique<CTextInput>(pTIV1, this));
}

void CInputMethodRelay::removeTextInput(CTextInput* pInput) {
    std::erase_if(m_textInputs, [pInput](const auto& other) { return other.get() == pInput; });
}

void CInputMethodRelay::updateAllPopups() {
    for (auto const& p : m_inputMethodPopups) {
        p->onCommit();
    }
}

void CInputMethodRelay::activateIME(CTextInput* pInput, bool shouldCommit) {
    if (m_inputMethod.expired())
        return;

    m_inputMethod->activate();
    if (shouldCommit)
        commitIMEState(pInput);
}

void CInputMethodRelay::deactivateIME(CTextInput* pInput, bool shouldCommit) {
    if (m_inputMethod.expired())
        return;

    m_inputMethod->deactivate();
    if (shouldCommit)
        commitIMEState(pInput);
}

void CInputMethodRelay::commitIMEState(CTextInput* pInput) {
    if (m_inputMethod.expired())
        return;

    pInput->commitStateToIME(m_inputMethod.lock());
}

void CInputMethodRelay::onKeyboardFocus(SP<CWLSurfaceResource> pSurface) {
    if (m_inputMethod.expired())
        return;

    if (pSurface == m_lastKbFocus)
        return;

    m_lastKbFocus = pSurface;

    for (auto const& ti : m_textInputs) {
        if (!ti->focusedSurface())
            continue;

        ti->leave();
    }

    if (!pSurface)
        return;

    for (auto const& ti : m_textInputs) {
        if (!ti->isV3())
            continue;

        if (ti->client() != pSurface->client())
            continue;

        ti->enter(pSurface);
    }
}

CInputPopup* CInputMethodRelay::popupFromCoords(const Vector2D& point) {
    for (auto const& p : m_inputMethodPopups) {
        if (p->shouldBeRendered() && p->isVecInPopup(point))
            return p.get();
    }

    return nullptr;
}

CInputPopup* CInputMethodRelay::popupFromSurface(const SP<CWLSurfaceResource> surface) {
    for (auto const& p : m_inputMethodPopups) {
        if (p->getSurface() == surface)
            return p.get();
    }

    return nullptr;
}

CSeatManager* CInputMethodRelay::seat() const {
    return m_seat ? m_seat : g_pSeatManager.get();
}
SP<CWLSurfaceResource> CInputMethodRelay::focus() const {
    return seat() ? seat()->m_state.keyboardFocus.lock() : nullptr;
}
const std::vector<UP<CInputPopup>>& CInputMethodRelay::popups() const {
    return m_inputMethodPopups;
}
