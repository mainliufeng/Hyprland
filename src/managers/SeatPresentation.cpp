#include "SeatPresentation.hpp"
#include "SeatConfiguration.hpp"
#include "../keybinds/Manager.hpp"
#include "SeatDesktop.hpp"
#include "SeatManager.hpp"
#include "SessionLockManager.hpp"
#include "input/InputManager.hpp"
#include "input/InputMethodRelay.hpp"
#include "../Compositor.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "../state/WorkspaceState.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../desktop/state/ViewState.hpp"
#include "../desktop/state/ViewHitTester.hpp"
#include "../desktop/view/window/Window.hpp"
#include "../workspace/RegularWorkspace.hpp"
#include "input/InputMethodPopup.hpp"
#include "../desktop/view/LayerSurface.hpp"
#include "../desktop/view/WLSurface.hpp"
#include "../output/Monitor.hpp"
#include "../pointer/PointerManager.hpp"
#include "../render/Renderer.hpp"
#include "../render/Context.hpp"
#include "../render/scene/SceneSelection.hpp"
#include "../render/pass/RendererHintsPassElement.hpp"

#include "../protocols/core/Seat.hpp"
#include "../protocols/InputMethodV2.hpp"
#include "../protocols/RelativePointer.hpp"
#include "../ipc/s2/S2.hpp"
#include "../protocols/core/DataDevice.hpp"
#include "../layout/supplementary/DragController.hpp"
#include <algorithm>
#include <format>
#include <xkbcommon/xkbcommon.h>

CSeatDesktop* CSeatPresentation::seat() const {
    return g_pSeatDesktopRegistry->forName(m_name);
}
PHLWORKSPACE CSeatPresentation::workspace() const {
    const auto s = seat();
    return m_workspace ? m_workspace : s ? s->workspace() : nullptr;
}
PHLMONITOR CSeatPresentation::output() const {
    return m_output.lock();
}
bool CSeatPresentation::active() const {
    return !m_owner.empty() && m_output && seat() && workspace();
}
bool CSeatPresentation::activeFor(PHLMONITOR monitor) const {
    return active() && output() == monitor;
}
const std::string& CSeatPresentation::owner() const {
    return m_owner;
}
bool CSeatPresentation::controlling() const {
    return active() && m_control;
}
bool CSeatPresentation::overlay() const {
    return m_overlay;
}

std::string CSeatPresentation::show(const std::string& name, const std::string& identity, PHLMONITOR monitor, const std::string& requested, const std::string& owner) {
    const auto s = g_pSeatDesktopRegistry->forName(name);
    if (g_pSessionLockManager->isSessionLocked() || !s || !s->viewAvailable() || s->socketName() != identity || !monitor || !monitor->m_enabled ||
        g_pSeatDesktopRegistry->isPrivateOutput(monitor) || owner.size() < 32 || owner.size() > 128)
        return "native presentation unavailable";
    if (active() && owner != m_owner)
        return "another viewer owns native presentation";
    PHLWORKSPACE view;
    if (requested != "current") {
        view = State::workspaceState()->query().input(requested).run();
        if (!view || !view->m_monitor)
            return "workspace unavailable";
    }
    if (m_control && (name != m_name || view))
        control(m_owner, false);
    if (!active()) {
        g_pInputManager->releaseAllMouseButtons();
        const auto now = Time::millis(Time::steadyNow());
        if (const auto keyboard = g_pSeatManager->m_keyboard.lock()) {
            for (const auto code : keyboard->pressedKeys())
                g_pSeatManager->sendKeyboardKey(now, code, WL_KEYBOARD_KEY_STATE_RELEASED);
        }
        g_pSeatManager->sendKeyboardMods(0, 0, 0, 0);
        g_pSeatManager->setKeyboardFocus(nullptr);
        g_pSeatManager->setPointerFocus(nullptr, {});
        m_savedCursor = Pointer::mgr()->untransformedPosition();
        m_cursor      = m_savedCursor;
        Pointer::mgr()->lockSoftwareForMonitor(monitor);
    } else if (output() != monitor) {
        Pointer::mgr()->unlockSoftwareForMonitor(output());
        Pointer::mgr()->lockSoftwareForMonitor(monitor);
    }
    ++m_viewEpoch;
    m_name      = name;
    m_identity  = identity;
    m_owner     = owner;
    m_output    = monitor;
    m_workspace = view;
    m_heartbeat = Time::steadyNow();
    g_pHyprRenderer->damageMonitor(monitor);
    IPC::Socket2::sock()->postEvent({"seatpresentation", m_name + "," + workspace()->addressableName()});
    return status(owner);
}
std::string CSeatPresentation::status(const std::string& owner, bool renew) {
    validate();
    if (owner != m_owner || !active())
        return "{\"active\":false}";
    if (renew)
        m_heartbeat = Time::steadyNow();
    return std::format("{{\"active\":true,\"native\":true,\"name\":\"{}\",\"output\":\"{}\",\"workspace\":\"{}\",\"following\":{},\"humanControl\":{},\"frames\":{}}}",
                       escapeJSONStrings(m_name), escapeJSONStrings(output()->m_name), escapeJSONStrings(workspace()->addressableName()), !m_workspace, m_control, m_frames);
}
std::string CSeatPresentation::control(const std::string& owner, bool enabled) {
    validate();
    if (owner != m_owner || !active())
        return "native presentation control unavailable";
    const auto s = seat();
    if (m_control != enabled) {
        ++m_viewEpoch;
        s->setPaused(true);
        if (enabled) {
            s->setPaused(false);
            s->m_nativeControl = true;
            s->manager()->setKeyboard(g_pSeatManager->m_keyboard.lock());
            s->manager()->setMouse(g_pSeatManager->m_mouse.lock());
            s->updateCapabilities();
            if (m_workspace) {
                const auto result = s->switchWorkspace(Workspace::selector(*m_workspace));
                if (result != "ok") {
                    s->setPaused(true);
                    return result;
                }
                m_workspace.reset();
            }
        }
        m_control    = enabled;
        m_generation = s->controlGeneration();
        pointerMotion(Time::millis(Time::steadyNow()));
        IPC::Socket2::sock()->postEvent({"seatpresentation", m_name + "," + workspace()->addressableName()});
    }
    return status(owner);
}
std::string CSeatPresentation::hide(const std::string& owner) {
    if (owner != m_owner)
        return "native presentation owner mismatch";
    clear();
    return "ok";
}
void CSeatPresentation::clear() {
    if (m_owner.empty())
        return;
    const auto monitor = output();
    if (m_control) {
        if (const auto s = seat())
            s->setPaused(true);
    }
    for (const auto key : m_overlayKeys)
        g_pSeatManager->sendKeyboardKey(Time::millis(Time::steadyNow()), key, WL_KEYBOARD_KEY_STATE_RELEASED);
    for (const auto& [button, overlay] : m_buttonOwners)
        if (overlay)
            g_pSeatManager->sendPointerButton(Time::millis(Time::steadyNow()), button, WL_POINTER_BUTTON_STATE_RELEASED);
    m_overlayKeys.clear();
    m_buttonOwners.clear();
    ++m_viewEpoch;
    const auto previousName = m_name;
    m_control               = false;
    m_overlay               = false;
    m_owner.clear();
    m_name.clear();
    m_workspace.reset();
    m_output.reset();
    if (monitor)
        Pointer::mgr()->unlockSoftwareForMonitor(monitor);
    Pointer::mgr()->warpTo(m_savedCursor);
    if (!g_pSessionLockManager->isSessionLocked()) {
        if (const auto window = Desktop::focusState()->window())
            g_pSeatManager->setKeyboardFocus(window->wlSurface()->resource());
        g_pInputManager->simulateMouseMovement();
    }
    if (monitor)
        g_pHyprRenderer->damageMonitor(monitor);
    IPC::Socket2::sock()->postEvent({"seatpresentation", previousName + ","});
}
void CSeatPresentation::validate() {
    if (m_owner.empty())
        return;
    const auto s = seat();
    if (g_pSessionLockManager->isSessionLocked() || !output() || !output()->m_enabled || !output()->m_dpmsStatus || !s || !s->viewAvailable() || s->socketName() != m_identity ||
        !workspace() || !workspace()->m_monitor || !workspace()->m_monitor->m_enabled || Time::steadyNow() - m_heartbeat > std::chrono::seconds(5)) {
        clear();
        return;
    }
    if (m_control && (s->paused() || s->controlGeneration() != m_generation))
        m_control = false;
}
Vector2D CSeatPresentation::scenePoint(const Vector2D& position) const {
    const auto source = workspace()->m_monitor.lock();
    const auto target = output();
    const auto scale  = std::min(target->m_transformedSize.x / source->m_transformedSize.x, target->m_transformedSize.y / source->m_transformedSize.y);
    const auto offset = (target->m_transformedSize - source->m_transformedSize * scale) / 2.0;
    const auto pixels = (position - target->m_position) * target->m_scale;
    return source->m_position + (pixels - offset) / (scale * source->m_scale);
}
bool CSeatPresentation::overlayFocus(uint32_t time) {
    if (std::ranges::any_of(m_buttonOwners, [](const auto& item) { return !item.second; })) {
        m_overlay = false;
        return false;
    }
    std::vector<PHLLSREF> layers;
    for (const auto& plane : output()->m_layerSurfaceLayers)
        for (const auto& ref : plane)
            if (const auto layer = ref.lock(); layer && SeatConfig::manager()->overlay(layer, m_identity))
                layers.emplace_back(ref);
    Vector2D   local;
    PHLLS      found;
    const auto surface = Desktop::viewState()->hitTest().layerSurfaceAt(m_cursor, &layers, &local, &found);
    m_overlay          = bool(surface);
    g_pSeatManager->setPointerFocus(surface, local);
    if (surface)
        g_pSeatManager->sendPointerMotion(time, local);
    return m_overlay;
}
void CSeatPresentation::pointerMotion(uint32_t time) {
    if (!active())
        return;
    const auto target = output();
    m_cursor.x        = std::clamp(m_cursor.x, target->m_position.x, target->m_position.x + target->m_size.x - 0.01);
    m_cursor.y        = std::clamp(m_cursor.y, target->m_position.y, target->m_position.y + target->m_size.y - 0.01);
    Pointer::mgr()->warpTo(m_cursor);
    if (!overlayFocus(time) && controlling()) {
        const auto s = seat();
        s->m_pointer->warpTo(scenePoint(m_cursor));
        s->m_dragController->mouseMove(s->m_pointer->position());
        s->refocus(time);
        s->manager()->sendPointerFrame();
    }
    g_pHyprRenderer->damageMonitor(target);
}
bool CSeatPresentation::motion(const IPointer::SMotionEvent& event, const Vector2D& delta) {
    validate();
    if (!active())
        return false;
    m_cursor += delta;
    pointerMotion(event.timeMs);
    return true;
}
bool CSeatPresentation::absolute(const IPointer::SMotionAbsoluteEvent& event) {
    validate();
    if (!active())
        return false;
    m_cursor = output()->m_position + event.absolute * output()->m_size;
    pointerMotion(event.timeMs);
    return true;
}
bool CSeatPresentation::button(const IPointer::SButtonEvent& event) {
    validate();
    if (!active())
        return false;
    bool human = overlayFocus(event.timeMs);
    if (event.state == WL_POINTER_BUTTON_STATE_PRESSED)
        m_buttonOwners[event.button] = human;
    else if (const auto it = m_buttonOwners.find(event.button); it != m_buttonOwners.end()) {
        human = it->second;
        m_buttonOwners.erase(it);
    } else
        return true;
    if (human)
        g_pSeatManager->sendPointerButton(event.timeMs, event.button, event.state);
    else if (controlling())
        seat()->button(event, nullptr);
    frame();
    return true;
}
bool CSeatPresentation::axis(const IPointer::SAxisEvent& event) {
    validate();
    if (!active())
        return false;
    if (overlayFocus(event.timeMs))
        g_pSeatManager->sendPointerAxis(event.timeMs, event.axis, event.delta, event.deltaDiscrete / 120, event.deltaDiscrete, event.source, event.relativeDirection);
    else if (controlling())
        seat()->axis(event);
    frame();
    return true;
}
bool CSeatPresentation::frame() {
    if (!active())
        return false;
    g_pSeatManager->sendPointerFrame();
    if (controlling())
        seat()->manager()->sendPointerFrame();
    return true;
}
bool CSeatPresentation::keyboardOverlay() const {
    if (!active())
        return false;
    for (const auto& plane : output()->m_layerSurfaceLayers)
        for (const auto& ref : plane)
            if (const auto layer = ref.lock(); layer && layer->mapped() && !layer->seatDesktop() && SeatConfig::manager()->overlay(layer, m_identity, true) &&
                layer->resource() == g_pSeatManager->m_state.keyboardFocus)
                return true;
    return false;
}
bool CSeatPresentation::key(const IKeyboard::SKeyEvent& event, SP<IKeyboard> keyboard) {
    validate();
    if (!active())
        return false;
    // Input-method feedback is already processed by the native input path.
    // Never replay it as a physical shortcut, or deliver it to a hidden app.
    const auto inputMethod = g_pInputManager->m_relay.m_inputMethod.lock();
    if (inputMethod && inputMethod->hasGrab() && inputMethod->grabClient() == keyboard->getClient())
        return !keyboardOverlay();
    if (event.updateMods) {
        keyboard->updateXkbStateWithKey(event.keycode + 8, event.state == WL_KEYBOARD_KEY_STATE_PRESSED);
        keyboard->updateModifiersState();
    }
    if (keyboardOverlay()) {
        auto routed       = event;
        routed.updateMods = false;
        if (!Keybinds::mgr()->onKeyEvent(routed, keyboard))
            return true;
        if (event.state == WL_KEYBOARD_KEY_STATE_PRESSED && std::ranges::find(m_overlayKeys, event.keycode) == m_overlayKeys.end())
            m_overlayKeys.push_back(event.keycode);
        else if (event.state == WL_KEYBOARD_KEY_STATE_RELEASED)
            std::erase(m_overlayKeys, event.keycode);
        g_pSeatManager->setKeyboard(keyboard);
        if (g_pSeatManager->m_keyboardEventHandlers.dispatch(event, keyboard, true))
            return true;
        const auto ime = g_pInputManager->m_relay.m_inputMethod.lock();
        if (ime && ime->hasGrab() && ime->grabClient() != keyboard->getClient()) {
            ime->setKeyboard(keyboard);
            ime->sendKey(event.timeMs, event.keycode, event.state);
        } else
            g_pSeatManager->sendKeyboardKey(event.timeMs, event.keycode, event.state);
        return true;
    }
    if (!controlling()) {
        auto routed       = event;
        routed.updateMods = false;
        Keybinds::mgr()->onKeyEvent(routed, keyboard);
    }
    const auto ctrl = xkb_state_mod_name_is_active(keyboard->m_xkbState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE);
    const auto alt  = xkb_state_mod_name_is_active(keyboard->m_xkbState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE);
    if (event.keycode == 1 && event.state == WL_KEYBOARD_KEY_STATE_PRESSED && ((ctrl && alt) || !controlling())) {
        clear();
        return true;
    }
    if (controlling()) {
        auto routed       = event;
        routed.updateMods = false;
        seat()->keyboardKey(routed, keyboard, true);
    }
    return true;
}
bool CSeatPresentation::modifiers(SP<IKeyboard> keyboard) {
    validate();
    if (!active())
        return false;
    const auto inputMethod = g_pInputManager->m_relay.m_inputMethod.lock();
    if (inputMethod && inputMethod->hasGrab() && inputMethod->grabClient() == keyboard->getClient())
        return !keyboardOverlay();
    if (keyboardOverlay()) {
        g_pSeatManager->setKeyboard(keyboard);
        const auto mods = keyboard->m_modifiersState;
        const auto ime  = g_pInputManager->m_relay.m_inputMethod.lock();
        if (ime && ime->hasGrab() && ime->grabClient() != keyboard->getClient())
            ime->sendMods(mods.depressed, mods.latched, mods.locked, mods.group);
        else
            g_pSeatManager->sendKeyboardMods(mods.depressed, mods.latched, mods.locked, mods.group);
    } else if (controlling())
        seat()->keyboardModifiers(keyboard, true);
    return true;
}

bool CSeatPresentation::draw(Render::CRenderContext& ctx, PHLMONITOR target, const Time::steady_tp& now) {
    validate();
    if (!activeFor(target))
        return false;
    const auto s      = seat();
    const auto ws     = workspace();
    const auto source = ws->m_monitor.lock();
    {
        auto saved                      = ctx.saveDrawState();
        ctx.m_sceneSeat                 = s;
        ctx.m_data.pMonitor             = source;
        const auto               scale  = std::min(target->m_transformedSize.x / source->m_transformedSize.x, target->m_transformedSize.y / source->m_transformedSize.y);
        const auto               offset = (target->m_transformedSize - source->m_transformedSize * scale) / 2.0;
        Render::SRenderModifData modifiers;
        modifiers.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALE, static_cast<float>(scale));
        modifiers.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE, offset);
        g_pHyprRenderer->addPassElement(ctx, makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{modifiers}));
        g_pHyprRenderer->renderWorkspace(ctx, ws, now, Render::eSceneMode::WORKSPACE_WITH_SHELL);
        if (ws == s->workspace()) {
            g_pHyprRenderer->renderDragIcon(ctx, source, now);
            for (const auto& popup : s->relay()->popups())
                if (popup->shouldBeRendered())
                    g_pHyprRenderer->renderIMEPopup(ctx, popup.get(), source, now);
            s->pointer()->renderSoftwareCursorsFor(ctx, source, now, ctx.m_data.damage, std::nullopt, true, true);
        }
        g_pHyprRenderer->addPassElement(ctx, makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{Render::SRenderModifData{}}));
        ctx.m_sceneSeat = nullptr;
    }
    // Registered local surfaces remain on the target output.
    for (const auto& plane : target->m_layerSurfaceLayers)
        for (const auto& ref : plane)
            if (const auto layer = ref.lock(); layer && SeatConfig::manager()->overlay(layer, m_identity))
                g_pHyprRenderer->renderLayer(ctx, layer, target, now);
    for (const auto& popup : g_pInputManager->m_relay.popups())
        if (popup->shouldBeRendered())
            g_pHyprRenderer->renderIMEPopup(ctx, popup.get(), target, now);
    ++m_frames;
    return true;
}
