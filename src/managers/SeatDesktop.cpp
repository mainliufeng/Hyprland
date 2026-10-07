#include "../Compositor.hpp"
#include "../event/EventBus.hpp"
#include "../protocols/core/Output.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <system_error>
#include <filesystem>
#include <hyprutils/utils/ScopeGuard.hpp>
#include "XCursorManager.hpp"
#include "../protocols/CursorShape.hpp"
#include "../config/ConfigValue.hpp"
#include "SeatDesktop.hpp"
#include "SeatManager.hpp"
#include "SessionLockManager.hpp"
#include "input/InputManager.hpp"
#include "input/InputMethodRelay.hpp"
#include "input/InputMethodPopup.hpp"
#include "../protocols/InputMethodV2.hpp"
#include "../protocols/PointerConstraints.hpp"
#include "../protocols/XDGShell.hpp"
#include "../protocols/core/Seat.hpp"
#include "../protocols/core/DataDevice.hpp"
#include "../protocols/core/Compositor.hpp"
#include "../protocols/RelativePointer.hpp"
#include "../pointer/PointerManager.hpp"
#include "../pointer/cursor/CursorManager.hpp"
#include "../desktop/state/ViewState.hpp"
#include "../desktop/state/WindowState.hpp"
#include "../desktop/state/ViewHitTester.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../desktop/view/window/Window.hpp"
#include "../desktop/view/window/WindowPresentation.hpp"
#include "../desktop/view/Popup.hpp"
#include "../protocols/types/SurfaceState.hpp"
#include "../desktop/view/window/WindowBackend.hpp"
#include "../desktop/view/WLSurface.hpp"
#include "../output/Monitor.hpp"
#include "../workspace/RegularWorkspace.hpp"
#include "../state/WorkspaceState.hpp"
#include "../state/MonitorState.hpp"
#include "../protocols/LayerShell.hpp"
#include "../protocols/SessionLock.hpp"
#include <aquamarine/backend/Backend.hpp>
#include "../layout/supplementary/DragController.hpp"
#include "../layout/space/Space.hpp"
#include "../helpers/time/Time.hpp"
#include <algorithm>
#include <charconv>
#include <limits>
#include "eventLoop/EventLoopManager.hpp"
#include "eventLoop/EventLoopTimer.hpp"
#include "../render/Renderer.hpp"
#include "../desktop/view/GlobalViewMethods.hpp"
#include "../ipc/s2/S2.hpp"

struct SSeatInputClient {
    wl_listener   destroy;
    CSeatDesktop* seat       = nullptr;
    wl_client*    client     = nullptr;
    uint64_t      generation = 0;

    ~SSeatInputClient() {
        wl_list_remove(&destroy.link);
    }
};

CSeatDesktop::CSeatDesktop(const std::string& name, PHLMONITOR monitor) : m_monitor(monitor), m_homeOutput(monitor), m_workspace(monitor->m_activeWorkspace) {
    m_protocol           = makeUnique<CWLSeatProtocol>(&wl_seat_interface, 9, "WLSeat-" + name, name);
    m_manager            = makeUnique<CSeatManager>(m_protocol.get());
    m_manager->m_desktop = this;
    m_relay              = makeUnique<CInputMethodRelay>(m_manager.get());
    m_listeners.emplace_back(m_manager->m_events.dndPointerFocusChange.listen([this] { PROTO::data->onDndPointerFocus(m_manager.get()); }));
    m_pointer = makeUnique<Pointer::CPointerManager>(true);
    m_pointer->bindMonitor(monitor);
    m_dragController  = makeUnique<Layout::Supplementary::CDragStateController>(this);
    const auto& image = Pointer::mgr()->currentCursorImage();
    m_pointer->setCursorBuffer(Pointer::Cursor::mgr()->getCursorBuffer(), image.hotspot, image.scale);
    m_pointer->warpTo(monitor->m_position + monitor->m_size / 2.0);
    m_listeners.emplace_back(m_manager->m_events.setCursor.listen([this](const auto& event) {
        auto surface = Desktop::View::CWLSurface::fromResource(event.surf);
        if (!surface && event.surf) {
            surface = Desktop::View::CWLSurface::create();
            surface->assign(event.surf);
        }
        m_cursorSurface = surface;
        m_pointer->setCursorSurface(surface, event.hotspot);
    }));
    m_cursorTheme = makeUnique<CXCursorManager>();
    m_cursorTheme->loadTheme("", 24, monitor->m_scale);
    m_listeners.emplace_back(PROTO::cursorShape->m_events.setShape.listen([this](const auto& event) {
        if (event.seat != m_manager.get() || !inputAllowed() || !m_manager->m_state.pointerFocusResource)
            return;
        const auto focused = m_manager->m_state.pointerFocusResource.lock();
        if (focused->client() != wl_resource_get_client(event.pMgr->resource()) || !m_manager->serialValid(event.resource, event.serial, false))
            return;
        const auto shape = m_cursorTheme->getShape(event.shapeName, 24, m_monitor->m_scale);
        if (!shape || shape->images.empty())
            return;
        const auto& image  = shape->images.front();
        const auto  buffer = makeShared<Pointer::Cursor::CCursorBuffer>(rc<const uint8_t*>(image.pixels.data()), image.size, image.hotspot);
        m_cursorSurface.reset();
        m_pointer->setCursorBuffer(buffer, image.hotspot / m_monitor->m_scale, m_monitor->m_scale);
    }));
    m_listeners.emplace_back(g_pSessionLockManager->m_events.lock.listen([this] {
        if (g_pSessionLockManager->agentMayContinue(this))
            return;
        setPaused(true);
        m_dragController->dragEnd();
        PROTO::data->abortDndIfPresent(m_manager.get());
        m_manager->setGrab(nullptr);
        m_buttons.clear();
        m_deviceButtons.clear();
        focusWindow(nullptr);
        m_manager->setPointerFocus(nullptr, {});
        m_pointer->resetCursorImage();
    }));
    m_listeners.emplace_back(g_pSessionLockManager->m_events.unlock.listen([this] {
        if (!m_active)
            return;
        const auto& image = Pointer::mgr()->currentCursorImage();
        m_pointer->setCursorBuffer(Pointer::Cursor::mgr()->getCursorBuffer(), image.hotspot, image.scale);
        refocus(0, true);
    }));
    m_listeners.emplace_back(monitor->m_events.disconnect.listen([this, original = PHLMONITORREF{monitor}] { retire(); }));
    m_socketName        = std::format("seat-{}-{}", name, wl_display_next_serial(g_pCompositor->m_wlDisplay));
    m_socketPath        = (std::filesystem::path(g_pCompositor->m_hyprTempDataRoot).parent_path() / m_socketName).string();
    sockaddr_un address = {.sun_family = AF_UNIX};
    if (m_socketPath.size() >= sizeof(address.sun_path))
        throw std::runtime_error("seat socket path exceeds AF_UNIX limit");
    std::ranges::copy(m_socketPath, address.sun_path);
    m_socketFd = Hyprutils::OS::CFileDescriptor{socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (!m_socketFd.isValid() || bind(m_socketFd.get(), rc<sockaddr*>(&address), sizeof(address)) < 0)
        throw std::system_error(errno, std::generic_category(), "creating seat socket");
    bool                          ready = false;
    Hyprutils::Utils::CScopeGuard cleanup([this, &ready] {
        if (!ready)
            std::filesystem::remove(m_socketPath);
    });
    std::filesystem::permissions(m_socketPath, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    if (listen(m_socketFd.get(), 16) < 0)
        throw std::system_error(errno, std::generic_category(), "listening on seat socket");
    m_socketSource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, m_socketFd.get(), WL_EVENT_READABLE, acceptClient, this);
    if (!m_socketSource)
        throw std::runtime_error("registering seat socket failed");
    ready = true;
}

CSeatDesktop::~CSeatDesktop() {
    m_dragController.reset();
    if (m_socketSource)
        wl_event_source_remove(m_socketSource);
    if (!m_socketPath.empty()) {
        std::error_code error;
        std::filesystem::remove(m_socketPath, error);
    }
    m_inputClients.clear();
    m_listeners.clear();
    m_keyboards.clear();
    m_pointers.clear();
    if (PROTO::data)
        PROTO::data->forgetSeat(m_manager.get());
    if (PROTO::xdgShell)
        PROTO::xdgShell->forgetSeat(m_manager.get());
    m_relay.reset();
    m_pointer.reset();
    // Keep the manager alive while the protocol releases child resources.
    m_protocol.reset();
    m_manager.reset();
}

CInputMethodRelay* CSeatDesktop::relay() const {
    return m_relay.get();
}

CSeatManager* CSeatDesktop::manager() const {
    return m_manager.get();
}
CWLSeatProtocol* CSeatDesktop::protocol() const {
    return m_protocol.get();
}
Pointer::CPointerManager* CSeatDesktop::pointer() const {
    return m_pointer.get();
}
PHLMONITOR CSeatDesktop::monitor() const {
    return m_monitor.lock();
}
PHLWORKSPACE CSeatDesktop::workspace() const {
    return m_workspace;
}
PHLWINDOW CSeatDesktop::window() const {
    return m_window.lock();
}
bool CSeatDesktop::active() const {
    return m_active;
}
const std::vector<SP<IKeyboard>>& CSeatDesktop::keyboards() const {
    return m_keyboards;
}

bool CSeatDesktop::inputAllowed() const {
    return !m_paused && viewAvailable();
}

bool CSeatDesktop::viewAvailable() const {
    return m_active && m_homeOutput && m_homeOutput->m_enabled && m_monitor && m_monitor->m_enabled &&
        (g_pSeatDesktopRegistry->isPrivateOutput(m_homeOutput.lock()) || m_monitor->m_dpmsStatus) &&
        (!g_pSessionLockManager->isSessionLocked() || g_pSessionLockManager->agentMayContinue(this));
}

bool CSeatDesktop::paused() const {
    return m_paused;
}

uint64_t CSeatDesktop::viewEpoch() const {
    return m_viewEpoch;
}

uint64_t CSeatDesktop::controlGeneration() const {
    return m_controlGeneration;
}

void CSeatDesktop::setPaused(bool paused) {
    m_managedControl = true;
    // Every transition revokes existing virtual devices, including devices
    // that remained connected across pause/resume or a session lock.
    m_dragController->dragEnd();
    PROTO::data->abortDndIfPresent(m_manager.get());
    m_manager->setGrab(nullptr);
    const auto time = Time::millis(Time::steadyNow());
    for (const auto key : pressedKeys())
        m_manager->sendKeyboardKey(time, key, WL_KEYBOARD_KEY_STATE_RELEASED);
    m_manager->sendKeyboardMods(0, 0, 0, 0);
    for (const auto code : m_buttons)
        m_manager->sendPointerButton(time, code, WL_POINTER_BUTTON_STATE_RELEASED);
    m_manager->sendPointerFrame();
    m_buttons.clear();
    m_deviceButtons.clear();
    focusWindow(nullptr);
    m_manager->setKeyboardFocus(nullptr);
    m_manager->setPointerFocus(nullptr, {});
    ++m_controlGeneration;
    m_captureGrant.clear();
    m_paused = paused;
    IPC::Socket2::sock()->postEvent({"seatcontrol", std::format("{},{},{}", protocol()->seatName(), m_controlGeneration, m_paused ? "paused" : "active")});
}

Layout::Supplementary::CDragStateController* CSeatDesktop::dragController() const {
    return m_dragController.get();
}

void CSeatDesktop::setCursorShape(const std::string& name) {
    if (!m_monitor)
        return;
    const auto shape = m_cursorTheme->getShape(name, 24, m_monitor->m_scale);
    if (!shape || shape->images.empty())
        return;
    const auto& image  = shape->images.front();
    const auto  buffer = makeShared<Pointer::Cursor::CCursorBuffer>(rc<const uint8_t*>(image.pixels.data()), image.size, image.hotspot);
    m_cursorSurface.reset();
    m_pointer->setCursorBuffer(buffer, image.hotspot / m_monitor->m_scale, m_monitor->m_scale);
}

void CSeatDesktop::retire() {
    if (!m_active)
        return;
    m_dragController->dragEnd();
    PROTO::data->abortDndIfPresent(m_manager.get());
    m_manager->setGrab(nullptr);
    m_active = false;
    if (m_socketSource) {
        wl_event_source_remove(m_socketSource);
        m_socketSource = nullptr;
    }
    m_socketFd.reset();
    std::error_code error;
    std::filesystem::remove(m_socketPath, error);
    for (const auto button : m_buttons)
        m_manager->sendPointerButton(Time::millis(Time::steadyNow()), button, WL_POINTER_BUTTON_STATE_RELEASED);
    m_buttons.clear();
    m_deviceButtons.clear();
    focusWindow(nullptr);
    m_manager->setPointerFocus(nullptr, {});
    m_manager->updateCapabilities(0);
    m_pointer->resetCursorImage();
    m_protocol->removeGlobal();
    // Removing an input seat does not remove shared applications or workspaces.
    m_workspace.reset();
}

void CSeatDesktop::updateCapabilities() {
    uint32_t caps = 0;
    if (!m_keyboards.empty())
        caps |= HID_INPUT_CAPABILITY_KEYBOARD;
    if (!m_pointers.empty())
        caps |= HID_INPUT_CAPABILITY_POINTER;
    m_manager->updateCapabilities(m_active ? caps : 0);
}

void CSeatDesktop::attachKeyboard(SP<IKeyboard> keyboard) {
    if (!m_active || !keyboard)
        return;
    m_keyboards.emplace_back(keyboard);
    m_deviceGenerations[keyboard.get()] = m_controlGeneration;
    keyboard->m_hlName                  = protocol()->seatName() + ":" + keyboard->m_deviceName;
    g_pInputManager->applyConfigToKeyboard(keyboard);
    m_listeners.emplace_back(keyboard->m_events.destroy.listen([this, device = keyboard.get()] {
        auto keepAlive = device->m_self.lock();
        m_manager->m_keyboardEventHandlers.onKeyboardRemoved(keepAlive);
        m_deviceGenerations.erase(device);
        std::erase_if(m_keyboards, [device](const auto& other) { return other.get() == device; });
        if (m_manager->m_keyboard == keepAlive) {
            if (m_keyboards.empty())
                m_manager->setKeyboardFocus(nullptr);
            m_manager->setKeyboard(m_keyboards.empty() ? nullptr : m_keyboards.back());
        }
        updateCapabilities();
        if (m_active && m_managedControl && !m_paused &&
            std::ranges::none_of(m_keyboards, [this](const auto& keyboard) { return m_deviceGenerations[keyboard.get()] == m_controlGeneration; }))
            setPaused(true);
    }));
    m_listeners.emplace_back(keyboard->m_keyboardEvents.key.listen([this, device = WP<IKeyboard>{keyboard}](const auto& event) {
        if (device)
            keyboardKey(event, device.lock());
    }));
    m_listeners.emplace_back(keyboard->m_keyboardEvents.modifiers.listen([this, device = WP<IKeyboard>{keyboard}](const auto&) {
        if (device)
            keyboardModifiers(device.lock());
    }));
    m_listeners.emplace_back(keyboard->m_keyboardEvents.keymap.listen([this, device = WP<IKeyboard>{keyboard}](const auto&) {
        if (device && m_deviceGenerations[device.get()] == m_controlGeneration && m_manager->m_keyboard == device)
            m_manager->updateActiveKeyboardData();
    }));
    updateCapabilities();
    m_manager->setKeyboard(keyboard);
    refocus(0, true);
}

void CSeatDesktop::attachPointer(SP<IPointer> pointer) {
    if (!m_active || !pointer)
        return;
    m_pointers.emplace_back(pointer);
    m_deviceGenerations[pointer.get()] = m_controlGeneration;
    pointer->m_hlName                  = protocol()->seatName() + ":" + pointer->m_deviceName;
    m_listeners.emplace_back(pointer->m_events.destroy.listen([this, device = pointer.get()] {
        auto       keepAlive = device->m_self.lock();
        const auto owned     = m_deviceButtons[device];
        for (const auto code : owned)
            button(IPointer::SButtonEvent{.timeMs = Time::millis(Time::steadyNow()), .button = code, .state = WL_POINTER_BUTTON_STATE_RELEASED}, device);
        m_deviceButtons.erase(device);
        m_deviceGenerations.erase(device);
        std::erase_if(m_pointers, [device](const auto& other) { return other.get() == device; });
        if (m_manager->m_mouse == keepAlive)
            m_manager->setMouse(m_pointers.empty() ? nullptr : m_pointers.back());
        if (m_pointers.empty())
            m_manager->setPointerFocus(nullptr, {});
        updateCapabilities();
        if (m_active && m_managedControl && !m_paused &&
            std::ranges::none_of(m_pointers, [this](const auto& pointer) { return m_deviceGenerations[pointer.get()] == m_controlGeneration; }))
            setPaused(true);
    }));
    m_listeners.emplace_back(pointer->m_pointerEvents.motion.listen([this, device = pointer.get()](const auto& event) {
        if (m_deviceGenerations[device] == m_controlGeneration)
            move(event);
    }));
    m_listeners.emplace_back(pointer->m_pointerEvents.motionAbsolute.listen([this, device = pointer.get()](const auto& event) {
        if (m_deviceGenerations[device] == m_controlGeneration)
            warp(event);
    }));
    m_listeners.emplace_back(pointer->m_pointerEvents.button.listen([this, device = pointer.get()](const auto& event) {
        if (m_deviceGenerations[device] == m_controlGeneration)
            button(event, device);
    }));
    m_listeners.emplace_back(pointer->m_pointerEvents.axis.listen([this, device = pointer.get()](const auto& event) {
        if (m_deviceGenerations[device] == m_controlGeneration)
            axis(event);
    }));
    m_listeners.emplace_back(pointer->m_pointerEvents.frame.listen([this] {
        if (inputAllowed())
            m_manager->sendPointerFrame();
    }));
    m_manager->setMouse(pointer);
    updateCapabilities();
    refocus();
}

std::vector<uint32_t> CSeatDesktop::pressedKeys() const {
    std::vector<uint32_t> keys;
    for (const auto& keyboard : m_keyboards) {
        if (m_deviceGenerations.at(keyboard.get()) != m_controlGeneration || !keyboard->m_enabled || !keyboard->m_allowed)
            continue;
        for (const auto key : keyboard->pressedKeys()) {
            if (std::ranges::find(keys, key) == keys.end())
                keys.emplace_back(key);
        }
    }
    return keys;
}

void CSeatDesktop::keyboardKey(const IKeyboard::SKeyEvent& event, SP<IKeyboard> keyboard) {
    if (!inputAllowed() || m_deviceGenerations[keyboard.get()] != m_controlGeneration || !keyboard->m_enabled || !keyboard->m_allowed)
        return;
    if (m_window && (m_window->m_workspace != workspace() || !m_window->mapped() || !m_window->acceptsInput())) {
        focusWindow(nullptr);
        refocus(0, true);
    }
    m_manager->setKeyboard(keyboard);
    if (event.keycode == 1 && event.state == WL_KEYBOARD_KEY_STATE_PRESSED)
        PROTO::data->abortDndIfPresent(m_manager.get());
    if (event.updateMods) {
        keyboard->updateXkbStateWithKey(event.keycode + 8, event.state == WL_KEYBOARD_KEY_STATE_PRESSED);
        keyboard->updateModifiersState();
        keyboardModifiers(keyboard);
    }
    if (m_manager->m_keyboardEventHandlers.dispatch(event, keyboard, true))
        return;
    const auto ime = m_relay->m_inputMethod.lock();
    if (ime && ime->hasGrab() && ime->grabClient() != keyboard->getClient()) {
        ime->setKeyboard(keyboard);
        ime->sendKey(event.timeMs, event.keycode, event.state);
    } else
        m_manager->sendKeyboardKey(event.timeMs, event.keycode, event.state);
}

void CSeatDesktop::keyboardModifiers(SP<IKeyboard> keyboard) {
    if (!inputAllowed() || m_deviceGenerations[keyboard.get()] != m_controlGeneration || !keyboard->m_enabled || !keyboard->m_allowed)
        return;
    m_manager->setKeyboard(keyboard);
    uint32_t depressed = keyboard->m_modifiersState.depressed;
    uint32_t latched   = keyboard->m_modifiersState.latched;
    uint32_t locked    = keyboard->m_modifiersState.locked;
    for (const auto& other : m_keyboards) {
        if (m_deviceGenerations[other.get()] != m_controlGeneration || !other->m_enabled || !other->m_allowed || !other->shareStates())
            continue;
        depressed |= other->m_modifiersState.depressed;
        latched |= other->m_modifiersState.latched;
        locked |= other->m_modifiersState.locked;
    }
    const auto ime = m_relay->m_inputMethod.lock();
    if (ime && ime->hasGrab() && ime->grabClient() != keyboard->getClient())
        ime->sendMods(depressed, latched, locked, keyboard->m_modifiersState.group);
    else
        m_manager->sendKeyboardMods(depressed, latched, locked, keyboard->m_modifiersState.group);
}

void CSeatDesktop::move(const IPointer::SMotionEvent& event) {
    if (!inputAllowed())
        return;
    m_pointer->move(event.delta);
    m_dragController->mouseMove(m_pointer->position());
    PROTO::relativePointer->sendRelativeMotion(sc<uint64_t>(event.timeMs) * 1000, event.delta, event.unaccel, m_manager.get());
    refocus(event.timeMs);
}

void CSeatDesktop::warp(const IPointer::SMotionAbsoluteEvent& event) {
    if (!inputAllowed() || !std::isfinite(event.absolute.x) || !std::isfinite(event.absolute.y))
        return;
    m_pointer->warpTo(m_monitor->m_position + Vector2D{std::clamp(event.absolute.x, 0.0, 1.0), std::clamp(event.absolute.y, 0.0, 1.0)} * m_monitor->m_size);
    m_dragController->mouseMove(m_pointer->position());
    refocus(event.timeMs);
}

void CSeatDesktop::button(const IPointer::SButtonEvent& event, IPointer* device) {
    if (!inputAllowed())
        return;
    auto& owned = m_deviceButtons[device];
    if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (std::ranges::find(owned, event.button) != owned.end())
            return;
        owned.emplace_back(event.button);
        if (std::ranges::find(m_buttons, event.button) != m_buttons.end())
            return;
        refocus(event.timeMs, true);
        if (std::ranges::find(m_buttons, event.button) == m_buttons.end())
            m_buttons.emplace_back(event.button);
    } else {
        if (std::ranges::find(owned, event.button) == owned.end())
            return;
        std::erase(owned, event.button);
        for (const auto& [other, buttons] : m_deviceButtons) {
            if (std::ranges::find(buttons, event.button) != buttons.end())
                return;
        }
        std::erase(m_buttons, event.button);
    }
    m_manager->sendPointerButton(event.timeMs, event.button, event.state);
    PROTO::data->dragButton(m_manager.get(), event.state);
    if (event.state == WL_POINTER_BUTTON_STATE_RELEASED && m_buttons.empty() && m_dragController->dragEnd())
        refocus(event.timeMs);
}

void CSeatDesktop::axis(const IPointer::SAxisEvent& event) {
    if (inputAllowed())
        m_manager->sendPointerAxis(event.timeMs, event.axis, event.delta, event.deltaDiscrete / 120, event.deltaDiscrete, event.source, event.relativeDirection);
}

void CSeatDesktop::focusWindow(PHLWINDOW window, SP<CWLSurfaceResource> surface) {
    if (window &&
        (!inputAllowed() || window->backend().isX11() || !window->mapped() || window->isHidden() || !window->acceptsInput() || window->shouldntFocus() ||
         window->m_ruleApplicator->noFocus().valueOrDefault() || window->m_workspace != workspace()))
        return;
    static auto modalBlocking = CConfigValue<Config::INTEGER>("general:modal_parent_blocking");
    if (window && *modalBlocking && window->backend().traits().hasModalChild)
        return;
    auto previous = m_window.lock();
    m_windowUnmap.reset();
    m_windowDestroy.reset();
    if (previous != window)
        ++m_viewEpoch;
    m_window = window;
    if (previous && previous != window) {
        if (!Desktop::focusState()->isWindowActive(previous))
            previous->backend().setActive(false);
        previous->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_FOCUS);
        previous->presentation().refreshValues();
    }
    m_manager->setKeyboardFocus(window ? (surface ? surface : window->wlSurface()->resource()) : nullptr);
    if (window) {
        auto removed = [this, disappearing = PHLWINDOWREF{window}] {
            focusWindow(nullptr);
            if (inputAllowed()) {
                auto current = workspace();
                for (const auto& candidate : Desktop::windowState()->windows()) {
                    if (candidate != disappearing && candidate->mapped() && !candidate->isHidden() && candidate->m_workspace == current) {
                        focusWindow(candidate);
                        if (m_window)
                            break;
                    }
                }
                m_manager->setPointerFocus(nullptr, {});
            }
        };
        m_windowUnmap   = window->m_events.unmap.listen(removed);
        m_windowDestroy = window->m_events.destroy.listen(removed);
        window->backend().setActive(true);
        window->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_FOCUS);
        window->presentation().refreshValues();
    }
}

void CSeatDesktop::refocus(uint32_t timeMs, bool keyboard) {
    if (!inputAllowed())
        return;
    if (m_dragController->exclusiveDeviceGrab())
        return;
    auto position = m_pointer->position();
    if (const auto focused = Desktop::View::CWLSurface::fromResource(m_manager->m_state.pointerFocus.lock())) {
        if (const auto constraint = focused->constraint(m_manager.get()); constraint && constraint->isActive()) {
            position = constraint->isLocked() ? constraint->logicPositionHint() : constraint->logicConstraintRegion().closestPoint(position);
            m_pointer->warpTo(position);
            const auto box = focused->getSurfaceBoxGlobal();
            m_manager->sendPointerMotion(timeMs, box ? position - box->pos() : Vector2D{});
            return;
        }
    }
    auto                   hitTest = Desktop::viewState()->hitTest();
    Vector2D               local;
    PHLWINDOW              window;
    SP<CWLSurfaceResource> surface;
    if (!PROTO::data->dndActive(m_manager.get()) && !m_buttons.empty() && m_manager->m_state.pointerFocus) {
        surface = m_manager->m_state.pointerFocus.lock();
        if (const auto owner = Desktop::View::CWLSurface::fromResource(surface)) {
            if (const auto box = owner->getSurfaceBoxGlobal())
                local = position - box->pos();
        }
    } else {
        if (auto popup = m_relay->popupFromCoords(position)) {
            surface = popup->getSurface();
            local   = position - popup->globalBox().pos();
        }
        if (!surface) {
            auto current = workspace();
            window       = hitTest.windowAtWorkspace(position, current, Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
            if (window)
                surface = hitTest.windowSurfaceAt(position, window, local);
        }
    }
    if (window && (window->backend().isX11() || window->m_workspace != workspace())) {
        surface.reset();
        window.reset();
    }
    if (m_manager->m_seatGrab && !m_manager->m_seatGrab->accepts(surface)) {
        if (keyboard)
            m_manager->setGrab(nullptr);
        else
            return;
    }
    m_manager->setPointerFocus(surface, local);
    m_manager->sendPointerMotion(timeMs, local);
    PROTO::data->dragMotion(m_manager.get(), position, timeMs);
    if (keyboard) {
        if (window)
            focusWindow(window, surface);
        else {
            focusWindow(nullptr);
            m_manager->setKeyboardFocus(surface);
        }
    }
}

std::string CSeatDesktop::switchWorkspace(const std::string& name) {
    if (!inputAllowed())
        return "seat is unavailable or session is locked";
    auto current = State::workspaceState()->query().input(name).run();
    if (!current) {
        int64_t number    = 0;
        auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), number);
        if (error == std::errc{} && end == name.data() + name.size() && number > 0)
            current = State::workspaceState()->createNumbered(Workspace::SWorkspaceNumberedID{number}, monitor(), name);
        else if (name.starts_with("name:") && name.size() > 5)
            current = State::workspaceState()->createNamed(name.substr(5), monitor());
        else
            return "expected a positive workspace number or name:<name>";
    }
    if (!current || !current->m_monitor.lock() || !current->m_monitor.lock()->m_enabled)
        return "workspace has no enabled output";
    m_dragController->dragEnd();
    PROTO::data->abortDndIfPresent(m_manager.get());
    m_manager->setGrab(nullptr);
    for (const auto button : m_buttons)
        m_manager->sendPointerButton(Time::millis(Time::steadyNow()), button, WL_POINTER_BUTTON_STATE_RELEASED);
    m_buttons.clear();
    m_deviceButtons.clear();
    focusWindow(nullptr);
    m_manager->setPointerFocus(nullptr, {});
    const auto oldMonitor = monitor();
    ++m_viewEpoch;
    m_workspace = current;
    m_monitor   = current->m_monitor.lock();
    m_pointer->bindMonitor(monitor());
    if (oldMonitor != monitor())
        m_pointer->warpTo(monitor()->m_position + monitor()->m_size / 2.0);
    // Layout remains shared, but choosing this view never activates the monitor's workspace.
    current->space()->recalculate(Layout::RECALCULATE_REASON_RENDER_MONITOR);
    for (const auto& window : Desktop::windowState()->windows()) {
        if (window->m_workspace == current)
            window->setSuspended(false);
    }
    focusWindow(current->getFocusCandidate());
    refocus();
    return "ok";
}

void CSeatDesktopRegistry::refreshWorkspace(PHLWORKSPACE workspace) {
    if (!workspace || (workspace->visible() && !g_pSessionLockManager->isSessionLocked() && workspace->m_monitor && workspace->m_monitor->m_dpmsStatus))
        return;
    const auto now  = Time::steadyNow();
    auto       tick = [&now](SP<CWLSurfaceResource> surface) {
        if (!surface || !surface->m_mapped)
            return;
        // Hidden views have no output presentation to release FIFO commits.
        // Acquire fences remain enforced by the surface state queue.
        surface->m_stateQueue.unlockFirst(LOCK_REASON_FIFO | LOCK_REASON_TIMER);
        surface->frame(now);
    };
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window->mapped() || window->isHidden() || window->m_workspace != workspace || !window->wlSurface()->resource())
            continue;
        window->setSuspended(false);
        window->wlSurface()->resource()->breadthfirst([&tick](SP<CWLSurfaceResource> surface, const auto&, void*) { tick(surface); }, nullptr);
        if (window->popupHead())
            window->popupHead()->breadthfirst([&tick](SP<Desktop::View::CPopup> popup, void*) { tick(popup->resource()); }, nullptr);
    }
}

class CSharedPrimarySeat : public IWaylandProtocol {
  public:
    CSharedPrimarySeat() : IWaylandProtocol(&wl_seat_interface, 9, "shared-primary-seat") {}
    void bindManager(wl_client* client, void* data, uint32_t version, uint32_t id) override {
        g_pSeatManager->protocol()->bindManager(client, this, version, id);
    }
};

CSeatDesktopRegistry::CSeatDesktopRegistry() {
    m_sharedPrimarySeat  = makeUnique<CSharedPrimarySeat>();
    m_privateOutputAdded = Event::bus()->m_events.monitor.added.listen([this](PHLMONITOR monitor) {
        if (m_pendingPrivateOutputs.erase(monitor->m_name))
            m_privateOutputs.emplace_back(monitor);
    });
    m_frameTimer         = makeShared<CEventLoopTimer>(
        std::nullopt,
        [this](auto timer, void*) {
            if (g_pHyprRenderer && g_pSessionLockManager) {
                std::vector<PHLWORKSPACE> updated;
                for (const auto& seat : m_seats) {
                    const auto current = seat->workspace();
                    if (!seat->viewAvailable() || !current || (current->visible() && !g_pSessionLockManager->isSessionLocked() && seat->monitor()->m_dpmsStatus) ||
                        std::ranges::find(updated, current) != updated.end())
                        continue;
                    updated.emplace_back(current);
                    refreshWorkspace(current);
                }
            }
            if (std::ranges::any_of(m_seats, [](const auto& seat) { return seat->active(); }))
                timer->updateTimeout(std::chrono::milliseconds(33));
        },
        nullptr);
    g_pEventLoopManager->addTimer(m_frameTimer);
}

CSeatDesktopRegistry::~CSeatDesktopRegistry() {
    m_frameTimer->cancel();
}

bool CSeatDesktopRegistry::usesWorkspace(PHLWORKSPACE current) const {
    return current && std::ranges::any_of(m_seats, [&](const auto& seat) { return seat->active() && seat->workspace() == current; });
}

bool CSeatDesktopRegistry::focusesWindow(PHLWINDOW window) const {
    return window && std::ranges::any_of(m_seats, [&](const auto& seat) { return seat->active() && seat->window() == window; });
}

std::string CSeatDesktopRegistry::create(const std::string& name, PHLMONITOR monitor) {
    if (g_pSessionLockManager->isSessionLocked())
        return "cannot create seats while locked";
    collectRetired();
    if (name.empty() || name == HL_SEAT_NAME || name.size() > 64 ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != std::string::npos)
        return "invalid or reserved seat name";
    if (forName(name))
        return "seat already exists";
    if (!monitor || !monitor->m_enabled || monitor->isMirror())
        return "output is unavailable";
    try {
        m_seats.emplace_back(makeUnique<CSeatDesktop>(name, monitor));
    } catch (const std::exception& error) { return std::format("seat creation failed: {}", error.what()); }
    m_frameTimer->updateTimeout(std::chrono::milliseconds(33));
    return "ok";
}

std::string CSeatDesktopRegistry::remove(const std::string& name) {
    collectRetired();
    if (auto seat = forName(name)) {
        seat->retire();
        return "ok";
    }
    return "seat not found";
}

CSeatDesktop* CSeatDesktopRegistry::forName(const std::string& name) const {
    for (const auto& seat : m_seats) {
        if (seat->active() && seat->protocol()->seatName() == name)
            return seat.get();
    }
    return nullptr;
}
CSeatDesktop* CSeatDesktopRegistry::forManager(CSeatManager* manager) const {
    for (const auto& seat : m_seats) {
        if (seat->manager() == manager)
            return seat.get();
    }
    return nullptr;
}
bool CSeatDesktopRegistry::isSeatGlobal(const wl_global* global) const {
    if (g_pSeatManager && g_pSeatManager->protocol()->getGlobal() == global)
        return true;
    for (const auto& seat : m_seats) {
        if (seat->protocol()->getGlobal() == global)
            return true;
    }
    return false;
}
const std::vector<UP<CSeatDesktop>>& CSeatDesktopRegistry::seats() const {
    return m_seats;
}

const std::string& CSeatDesktop::socketName() const {
    return m_socketName;
}

int CSeatDesktop::acceptClient(int fd, uint32_t mask, void* data) {
    auto seat = sc<CSeatDesktop*>(data);
    if (!seat->active() || !(mask & WL_EVENT_READABLE))
        return 0;
    while (true) {
        const int clientFd = accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (clientFd < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (!wl_client_create(g_pCompositor->m_wlDisplay, clientFd))
            close(clientFd);
    }
}

bool CSeatDesktop::ownsClient(const wl_client* client) const {
    sockaddr_un address = {};
    socklen_t   length  = sizeof(address);
    if (getsockname(wl_client_get_fd(cc<wl_client*>(client)), rc<sockaddr*>(&address), &length) < 0 || address.sun_family != AF_UNIX)
        return false;
    return m_socketPath == std::string_view(address.sun_path, strnlen(address.sun_path, sizeof(address.sun_path)));
}

CSeatDesktop* CSeatDesktopRegistry::forClient(const wl_client* client) const {
    for (const auto& seat : m_seats) {
        if (seat->ownsClient(client))
            return seat.get();
    }
    return nullptr;
}

bool CSeatDesktopRegistry::allowsGlobal(const wl_client* client, const wl_global* global) const {
    // The first (primary) global binds the socket's preferred seat. All other
    // seats must be in the initial registry roundtrip: GTK3 does not instantiate
    // devices from late seat announcements. A filtered alias exposes the real
    // primary seat to shared clients without changing any compositor globals.
    const auto preferred = forClient(client);
    if (global == m_sharedPrimarySeat->getGlobal())
        return preferred != nullptr;
    if (!preferred) {
        for (const auto& [name, output] : PROTO::outputs) {
            if (output->getGlobal() == global && isPrivateOutput(output->m_monitor.lock()))
                return false;
        }
    }
    if (preferred && PROTO::humanLock && PROTO::humanLock->getGlobal() == global)
        return false;
    return !preferred || preferred->protocol()->getGlobal() != global;
}

void CSeatDesktopRegistry::collectRetired() {
    std::erase_if(m_seats, [](const auto& seat) {
        if (seat->active() || seat->protocol()->hasResources())
            return false;
        wl_client* client = nullptr;
        wl_client_for_each(client, wl_display_get_client_list(g_pCompositor->m_wlDisplay)) {
            if (seat->ownsClient(client))
                return false;
        }
        return true;
    });
}

std::string CSeatDesktopRegistry::windowIdentity(PHLWINDOW window) {
    if (!window)
        return "";
    std::erase_if(m_windowIdentities, [](const auto& item) { return !item.second.first; });
    auto& entry = m_windowIdentities[window.get()];
    if (!entry.first) {
        entry.first  = window;
        entry.second = std::format("window-{}", ++m_nextWindowIdentity);
    }
    return entry.second;
}

void CSeatDesktop::forgetInputClient(wl_client* client) {
    m_inputClients.erase(client);
}

bool CSeatDesktop::allowsInputSource(wl_client* client) {
    auto& source = m_inputClients[client];
    if (!source) {
        source                 = makeUnique<SSeatInputClient>();
        source->seat           = this;
        source->client         = client;
        source->destroy.notify = [](wl_listener* listener, void*) {
            const auto registration = rc<SSeatInputClient*>(listener);
            registration->seat->forgetInputClient(registration->client);
        };
        wl_client_add_destroy_listener(client, &source->destroy);
    }
    if (!source->generation)
        source->generation = m_controlGeneration;
    return m_active && (!m_managedControl || (!m_paused && source->generation == m_controlGeneration));
}

bool CSeatDesktop::continuesOnHumanLock() const {
    return m_continueOnHumanLock;
}
void CSeatDesktop::setHumanLockPolicy(bool allow) {
    m_continueOnHumanLock = allow;
}
bool CSeatDesktopRegistry::isPrivateOutput(PHLMONITOR monitor) const {
    return monitor && (m_pendingPrivateOutputs.contains(monitor->m_name) || std::ranges::any_of(m_privateOutputs, [&](const auto& ref) { return ref == monitor; }));
}
std::string CSeatDesktopRegistry::registerPrivateOutput(PHLMONITOR monitor) {
    if (g_pSessionLockManager->isSessionLocked())
        return "output policy cannot change while locked";
    if (!monitor || !monitor->m_createdByUser || !monitor->m_output || monitor->m_output->getBackend()->type() != Aquamarine::AQ_BACKEND_HEADLESS)
        return "only managed headless outputs can be private";
    if (!isPrivateOutput(monitor))
        m_privateOutputs.emplace_back(monitor);
    return "ok";
}

void CSeatDesktop::setCaptureGrant(const std::string& grant) {
    m_captureGrant = grant;
}
bool CSeatDesktop::captureGranted(const std::string& grant) const {
    return !grant.empty() && grant == m_captureGrant;
}

std::string CSeatDesktopRegistry::createPrivateOutput(const std::string& name) {
    if (g_pSessionLockManager->isSessionLocked())
        return "cannot create outputs while locked";
    if (name.empty() || name.size() > 96 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != std::string::npos)
        return "invalid private output name";
    if (m_pendingPrivateOutputs.contains(name) || State::monitorState()->query().name(name).run())
        return "output already exists";
    m_pendingPrivateOutputs.insert(name);
    for (const auto& implementation : g_pCompositor->m_aqBackend->getImplementations()) {
        if (implementation->type() != Aquamarine::AQ_BACKEND_HEADLESS)
            continue;
        if (implementation->createOutput(name))
            return "ok";
    }
    m_pendingPrivateOutputs.erase(name);
    return "headless output creation failed";
}
