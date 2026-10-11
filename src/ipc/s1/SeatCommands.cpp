#include "SeatCommands.hpp"
#include "Commands.hpp"
#include "CommandFormatter.hpp"
#include "../../Compositor.hpp"
#include "../../render/Renderer.hpp"
#include "../../managers/SeatDesktop.hpp"
#include "../../managers/SeatPresentation.hpp"
#include "../../managers/SeatActionContext.hpp"
#include "../../managers/SeatConfiguration.hpp"
#include "../../managers/SeatManager.hpp"
#include "../../managers/SessionLockManager.hpp"
#include "../../managers/input/InputManager.hpp"
#include "../../managers/input/TextInput.hpp"
#include "../../desktop/state/FocusState.hpp"
#include "../../desktop/state/WindowState.hpp"
#include "../../desktop/state/ViewState.hpp"
#include "../../desktop/view/Popup.hpp"
#include "../../desktop/view/LayerSurface.hpp"
#include "../../desktop/view/WLSurface.hpp"
#include "../../desktop/view/window/Window.hpp"
#include "../../protocols/core/Compositor.hpp"
#include "../../protocols/core/Seat.hpp"
#include "../../pointer/PointerManager.hpp"
#include "../../keybinds/Manager.hpp"
#include "../../keybinds/Resolver.hpp"
#include "../../state/MonitorState.hpp"
#include "../../state/WorkspaceState.hpp"
#include "../../output/Monitor.hpp"
#include "../../workspace/RegularWorkspace.hpp"
#include "../../helpers/MiscFunctions.hpp"
#include "../../helpers/time/Time.hpp"
#include "../s2/S2.hpp"
#include <glaze/glaze.hpp>
#include <hyprutils/string/VarList.hpp>
#include <charconv>
#include <format>

using namespace Hyprutils::String;
using namespace IPC::Socket1;
using eHyprCtlOutputFormat = eOutputFormat;

// Retain the initial experimental spelling for existing controllers.
static bool authorizedSnapshotCommand(const std::string& command) {
    return command == "authorized-snapshot" || command == "agent-snapshot";
}

static std::string seatState(CSeatDesktop* seat, PHLWORKSPACE view = nullptr) {
    const auto output = view ? view->m_monitor.lock() : seat->monitor();
    if (!view)
        view = seat->workspace();
    if (!output || !view)
        return "seat view is unavailable";
    const auto cursor = seat->pointer()->position();
    return std::format("{{\"name\":\"{}\",\"seatId\":\"{}\",\"generation\":\"{}\",\"paused\":{},\"available\":{},"
                       "\"scopePolicy\":\"{}\",\"locked\":{},\"lockScope\":\"{}\",\"lockEpoch\":\"{}\",\"viewEpoch\":\"{}\","
                       "\"output\":\"{}\",\"display\":\"{}\",\"workspace\":\"{}\",\"viewWorkspace\":\"{}\","
                       "\"windowId\":\"{}\",\"window\":\"{}\",\"windowAddress\":\"0x{:x}\",\"cursor\":[{},{}],\"cursorVisible\":{},"
                       "\"position\":[{},{}],\"logicalSize\":[{},{}],\"pixelSize\":[{},{}],\"scale\":{},\"transform\":{}}}",
                       escapeJSONStrings(seat->protocol()->seatName()), escapeJSONStrings(seat->socketName()), seat->controlGeneration(), seat->paused(), seat->viewAvailable(),
                       seat->scopeExemptionAllowed() ? "allow" : "block", g_pSessionLockManager->isSessionLocked(),
                       g_pSessionLockManager->isSessionLocked() ? (g_pSessionLockManager->scoped() ? "scoped" : "session") : "none", g_pSessionLockManager->lockEpoch(),
                       seat->viewEpoch(), escapeJSONStrings(output->m_name), escapeJSONStrings(seat->socketName()), escapeJSONStrings(seat->workspace()->addressableName()),
                       escapeJSONStrings(view->addressableName()), SeatInput::registry()->windowIdentity(seat->window()),
                       seat->window() ? escapeJSONStrings(seat->window()->metadata().title()) : "", rc<uintptr_t>(seat->window().get()), cursor.x, cursor.y,
                       view == seat->workspace(), output->m_position.x, output->m_position.y, output->m_size.x, output->m_size.y, output->m_transformedSize.x,
                       output->m_transformedSize.y, output->m_scale, sc<int>(output->m_transform));
}

static uint64_t    primaryInputFocusEpoch = 1;

static std::string primaryInputFocusToken() {
    return std::format("{}:{}:{}", HL_SEAT_NAME, primaryInputFocusEpoch, g_pSessionLockManager->lockEpoch());
}

static void ensurePrimaryInputFocusListener() {
    static auto listener = g_pSeatManager->m_events.keyboardFocusChange.listen([] {
        ++primaryInputFocusEpoch;
        IPC::Socket2::sock()->postEvent({"seatinputfocus", std::string{HL_SEAT_NAME} + "," + primaryInputFocusToken()});
    });
}

struct SPhysicalInputTarget {
    CSeatManager*                           manager = nullptr;
    PHLWINDOW                               window;
    PHLLS                                   layer;
    std::optional<CTextInput::SEditorState> editor;
    std::string                             seatName = HL_SEAT_NAME;
    std::string                             token, reason;
};

static bool windowOwnsInputSurface(PHLWINDOW window, SP<CWLSurfaceResource> focused) {
    if (!window || !focused || !window->wlSurface()->resource())
        return false;
    bool owns  = false;
    auto visit = [&owns, &focused](SP<CWLSurfaceResource> surface, const auto&, void*) {
        if (surface == focused)
            owns = true;
    };
    window->wlSurface()->resource()->breadthfirst(visit, nullptr);
    if (window->popupHead()) {
        window->popupHead()->breadthfirst(
            [&visit](SP<Desktop::View::CPopup> popup, void*) {
                if (popup->resource())
                    popup->resource()->breadthfirst(visit, nullptr);
            },
            nullptr);
    }
    return owns;
}

static SPhysicalInputTarget physicalInputTarget(bool localEditor = false) {
    ensurePrimaryInputFocusListener();
    SPhysicalInputTarget target;
    if (g_pSessionLockManager->isSessionLocked()) {
        target.reason = "session locked";
        return target;
    }
    SeatInput::presentation()->validate();
    // A registered local editor belongs to the physical seat, even while
    // another seat is observed. Never grant input to the observed application.
    const auto focused  = g_pSeatManager->m_state.keyboardFocus.lock();
    const auto surface  = focused ? Desktop::View::CWLSurface::fromResource(focused) : nullptr;
    const auto layer    = surface ? Desktop::View::CLayerSurface::fromView(surface->view()) : nullptr;
    const auto identity = SeatInput::presentation()->active() ? SeatInput::presentation()->seat()->socketName() : "";
    if (localEditor && layer && layer->mapped() && !layer->seatDesktop() && SeatConfig::manager()->overlay(layer, identity, true)) {
        const auto input = g_pInputManager->m_relay.getFocusedTextInput();
        if (input && input->focusedSurface() == focused)
            target.editor = input->editorState();
        if (!target.editor || target.editor->protectedField) {
            target.reason = "local editor is unavailable or protected";
            return target;
        }
        target.layer   = layer;
        target.manager = g_pSeatManager.get();
        target.token   = primaryInputFocusToken() + ":" + std::to_string(target.editor->revision);
        if (SeatInput::presentation()->active())
            target.token +=
                ":" + identity + ":" + std::to_string(SeatInput::presentation()->seat()->controlGeneration()) + ":" + std::to_string(SeatInput::presentation()->viewEpoch());
        if (!target.manager->m_keyboard || !target.manager->m_state.keyboardFocusResource)
            target.reason = "local editor keyboard is unavailable";
        return target;
    }
    if (SeatInput::presentation()->active()) {
        if (!SeatInput::presentation()->controlling()) {
            target.reason = "desktop is read only";
            return target;
        }
        // The primary seat owns the native desktop's prompt and other overlays.
        // Its hidden application must never become a fallback input target.
        if (g_pSeatManager->m_state.keyboardFocus) {
            target.reason = "desktop overlay has keyboard focus";
            return target;
        }
        const auto seat = SeatInput::presentation()->seat();
        if (!seat || !seat->inputAllowed()) {
            target.reason = "controlled seat is unavailable";
            return target;
        }
        target.seatName = seat->protocol()->seatName();
        target.manager  = seat->manager();
        target.window   = seat->window();
        target.token    = seat->inputFocusToken();
        if (!target.window || target.window->m_workspace != seat->workspace()) {
            target.reason = "controlled workspace has no focused application";
            return target;
        }
    } else {
        target.manager = g_pSeatManager.get();
        target.window  = Desktop::focusState()->window();
        target.token   = primaryInputFocusToken();
    }
    if (!target.window || !target.window->mapped() || target.window->isHidden() || !target.window->acceptsInput() ||
        !windowOwnsInputSurface(target.window, target.manager->m_state.keyboardFocus.lock())) {
        target.reason = "keyboard focus does not belong to an application";
        return target;
    }
    if (!target.manager->m_keyboard || !target.manager->m_state.keyboardFocusResource)
        target.reason = "application keyboard is unavailable";
    return target;
}

static std::string physicalInputTargetJSON(bool localEditor = false) {
    const auto target  = physicalInputTarget(localEditor);
    const auto allowed = target.reason.empty();
    if (allowed && target.layer) {
        const auto& editor = *target.editor;
        const auto  text   = glz::write_json(editor.text).value_or("null");
        return std::format(
            "{{\"allowed\":true,\"reason\":\"\",\"kind\":\"local-editor\",\"token\":\"{}\",\"seatName\":\"{}\",\"pid\":{},"
            "\"namespace\":\"{}\",\"surfaceId\":\"{:x}\",\"editor\":{{\"text\":{},\"cursor\":{},\"anchor\":{},\"revision\":{},\"protected\":false,\"surroundingAvailable\":{}}}}}",
            escapeJSONStrings(target.token), escapeJSONStrings(target.seatName), target.layer->getPID(), escapeJSONStrings(target.layer->m_namespace),
            rc<uintptr_t>(target.layer->resource().get()), text, editor.cursor, editor.anchor, editor.revision, editor.surroundingAvailable);
    }
    auto window = allowed ? CCommandFormatter::getWindowData(target.window, FORMAT_JSON) : "{}";
    if (allowed)
        window.pop_back(); // The shared list formatter appends a comma.
    return std::format("{{\"allowed\":{},\"reason\":\"{}\",\"token\":\"{}\",\"seatName\":\"{}\",\"window\":{}}}", allowed, escapeJSONStrings(target.reason),
                       allowed ? escapeJSONStrings(target.token) : "", escapeJSONStrings(target.seatName), window);
}

static std::string physicalInputShortcut(const std::string& token, const std::string& mods, const std::string& key, bool localEditor = false) {
    const auto target = physicalInputTarget(localEditor);
    if (!target.reason.empty())
        return "input target unavailable: " + target.reason;
    if (token != target.token)
        return "stale input target";
    if (key.starts_with("mouse:"))
        return "input shortcut requires a keyboard key";
    const auto keycode = Keybinds::resolver()->resolveKeycode(key);
    if (!keycode || *keycode < 8 || *keycode > 0x2ff + 8)
        return "invalid input shortcut key";
    const auto keyboard = target.manager->m_keyboard.lock();
    const auto previous = keyboard->m_modifiersState;
    const auto now      = Time::millis(Time::steadyNow());
    target.manager->sendKeyboardMods(g_pInputManager->hyprlandModsToXkb(keyboard, Keybinds::modMaskFromString(mods)), 0, 0, 0);
    target.manager->sendKeyboardKey(now, *keycode - 8, WL_KEYBOARD_KEY_STATE_PRESSED);
    target.manager->sendKeyboardKey(now, *keycode - 8, WL_KEYBOARD_KEY_STATE_RELEASED);
    target.manager->sendKeyboardMods(previous.depressed, previous.latched, previous.locked, previous.group);
    return "ok";
}

// The primary desktop exposes the same snapshot/action contract as the other
// seats, while retaining its native physical devices and shell.
static std::string primaryDesktopId() {
    return g_pCompositor->m_instanceSignature + "-primary";
}
static std::string primaryDesktopState(PHLWORKSPACE view = nullptr) {
    ensurePrimaryInputFocusListener();
    const auto output  = Desktop::focusState()->monitor();
    const auto current = output ? output->m_activeWorkspace : nullptr;
    if (!output || !current)
        return "primary desktop unavailable";
    if (!view)
        view = current;
    const auto window    = Desktop::focusState()->window();
    const auto cursor    = Pointer::mgr()->position();
    const bool available = output->m_enabled && output->m_dpmsStatus && !SeatInput::presentation()->active();
    return std::format("{{\"name\":\"main\",\"primary\":true,\"seatName\":\"{}\",\"seatId\":\"{}\",\"generation\":\"{}\","
                       "\"paused\":{},\"available\":{},\"scopePolicy\":\"block\",\"locked\":{},\"lockScope\":\"{}\","
                       "\"lockEpoch\":\"{}\",\"viewEpoch\":\"{}\",\"output\":\"{}\",\"display\":\"{}\",\"workspace\":\"{}\",\"viewWorkspace\":\"{}\","
                       "\"windowId\":\"{}\",\"window\":\"{}\",\"windowAddress\":\"0x{:x}\",\"cursor\":[{},{}],\"cursorVisible\":{},"
                       "\"position\":[{},{}],\"logicalSize\":[{},{}],\"pixelSize\":[{},{}],\"scale\":{},\"transform\":{}}}",
                       HL_SEAT_NAME, escapeJSONStrings(primaryDesktopId()), SeatInput::registry()->primaryGeneration(), SeatInput::registry()->primaryPaused(), available,
                       g_pSessionLockManager->isSessionLocked(), g_pSessionLockManager->isSessionLocked() ? (g_pSessionLockManager->scoped() ? "scoped" : "session") : "none",
                       g_pSessionLockManager->lockEpoch(), primaryInputFocusEpoch, escapeJSONStrings(output->m_name), escapeJSONStrings(g_pCompositor->m_wlDisplaySocket),
                       escapeJSONStrings(current->addressableName()), escapeJSONStrings(view->addressableName()), SeatInput::registry()->windowIdentity(window),
                       window ? escapeJSONStrings(window->metadata().title()) : "", rc<uintptr_t>(window.get()), cursor.x, cursor.y, view == current, output->m_position.x,
                       output->m_position.y, output->m_size.x, output->m_size.y, output->m_transformedSize.x, output->m_transformedSize.y, output->m_scale,
                       sc<int>(output->m_transform));
}
static std::string primaryDesktopRequest(eHyprCtlOutputFormat format, const CVarList& args) {
    if (args.size() == 3 && args[1] == "state")
        return primaryDesktopState();
    const auto output    = Desktop::focusState()->monitor();
    const auto workspace = output ? output->m_activeWorkspace : nullptr;
    if (!output || !workspace)
        return "primary desktop unavailable";
    if (args.size() == 3 && args[1] == "windows") {
        std::string result = "[";
        for (const auto& window : Desktop::windowState()->windows()) {
            if (window->m_workspace != workspace || !window->mapped() || window->isHidden())
                continue;
            if (result.size() > 1)
                result += ",";
            result += std::format("{{\"id\":\"{}\",\"title\":\"{}\",\"address\":\"0x{:x}\"}}", SeatInput::registry()->windowIdentity(window),
                                  escapeJSONStrings(window->metadata().title()), rc<uintptr_t>(window.get()));
        }
        return result + "]";
    }
    if (args.size() >= 5 && (args[3] != primaryDesktopId() || args[4] != std::to_string(SeatInput::registry()->primaryGeneration())) &&
        (args[1] == "control" || args[1] == "act" || args[1] == "export-grant"))
        return "stale primary desktop identity or generation";
    if (args[1] == "control" && args.size() >= 6) {
        if (args[5] == "pause" && args.size() == 6)
            SeatInput::registry()->setPrimaryPaused(true);
        else if (args[5] == "resume-composed" && args.size() == 7) {
            const auto ownerArgument = args[6];
            pid_t      owner         = 0;
            const auto parsed        = std::from_chars(ownerArgument.data(), ownerArgument.data() + ownerArgument.size(), owner);
            if (parsed.ec != std::errc{} || parsed.ptr != ownerArgument.data() + ownerArgument.size() || owner <= 0 || g_pSessionLockManager->isSessionLocked() ||
                SeatInput::presentation()->active() || !output->m_dpmsStatus)
                return "primary control unavailable";
            SeatInput::registry()->setPrimaryPaused(false, owner);
        } else
            return "invalid primary control request";
        return primaryDesktopState();
    }
    if (args[1] == "export-grant" && args.size() == 6) {
        if (SeatInput::registry()->primaryPaused() || g_pSessionLockManager->isSessionLocked() || args[5].size() < 32 || args[5].size() > 128)
            return "primary capture grant unavailable";
        SeatInput::registry()->setPrimaryCaptureGrant(args[5]);
        return primaryDesktopState();
    }
    if ((args[1] == "snapshot" && args.size() == 7) || (authorizedSnapshotCommand(args[1]) && args.size() == 9)) {
        if (args[3] != primaryDesktopId())
            return "stale primary desktop identity";
        const bool authorized = authorizedSnapshotCommand(args[1]);
        if (authorized && (args[7] != std::to_string(SeatInput::registry()->primaryGeneration()) || !SeatInput::registry()->primaryCaptureGranted(args[8]) || args[4] != "current"))
            return "invalid primary export authorization";
        const auto view = args[4] == "current" ? workspace : State::workspaceState()->query().input(args[4]).run();
        if (!view || (args[5] != "argb" && args[5] != "png"))
            return "invalid primary snapshot";
        const auto captured = g_pHyprRenderer->captureSeatWorkspace(view, nullptr, args[6], args[5] == "argb", authorized);
        if (captured != "ok")
            return captured;
        static uint64_t frame = 0;
        auto            state = primaryDesktopState(view);
        state.pop_back();
        return state +
            std::format(",\"frameId\":\"main-{}\",\"timestampNs\":\"{}\",\"format\":\"{}\"}}", ++frame,
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Time::steadyNow().time_since_epoch()).count(), args[5]);
    }
    if (args[1] == "act" && args.size() == 7) {
        if (SeatInput::registry()->primaryPaused() || g_pSessionLockManager->isSessionLocked() || SeatInput::presentation()->active())
            return "primary control paused";
        if (args[5] == "workspace")
            return dispatchRequest(format, "dispatch hl.dsp.focus({workspace=" + ("\"" + escapeJSONStrings(args[6]) + "\"") + "})");
        if (args[5] == "focus") {
            for (const auto& window : Desktop::windowState()->windows()) {
                if (window->m_workspace == workspace && SeatInput::registry()->windowIdentity(window) == args[6])
                    return dispatchRequest(format, std::format("dispatch hl.dsp.focus({{window=\"address:0x{:x}\"}})", rc<uintptr_t>(window.get())));
            }
            return "window identity not found in primary workspace";
        }
    }
    return "unsupported primary desktop action";
}

std::string IPC::Socket1::seatRequest(eOutputFormat format, std::string request) {
    CVarList args(request, 0, ' ');
    if (request.starts_with("seat configure "))
        return SeatConfig::manager()->configure(request.substr(std::string{"seat configure "}.size()));
    if (args.size() == 3 && args[1] == "configuration-renew")
        return SeatConfig::manager()->renew(args[2]);
    if (args.size() == 3 && args[1] == "configuration-remove")
        return SeatConfig::manager()->remove(args[2]);
    if (args.size() == 5 && args[1] == "ensure-workspace")
        return SeatConfig::manager()->ensureWorkspace(args[2], args[3], args[4]);
    if (args.size() >= 3 && args[2] == "main")
        return primaryDesktopRequest(format, args);
    if (args.size() == 2 && args[1] == "input-target-v2")
        return physicalInputTargetJSON(true);
    if (args.size() == 5 && args[1] == "input-shortcut-v2")
        return physicalInputShortcut(args[2], args[3], args[4], true);
    if (args.size() == 2 && args[1] == "input-target")
        return physicalInputTargetJSON();
    if (args.size() == 5 && args[1] == "input-shortcut")
        return physicalInputShortcut(args[2], args[3], args[4]);
    if ((args.size() == 3 || args.size() == 4) && args[1] == "validate-context")
        return SeatInput::validateContext(args[2], args.size() == 4 ? args[3] : "");
    if (args.size() >= 4 && args[1] == "context-dispatch") {
        std::string owner;
        if (!SeatInput::validateContext(args[2]).starts_with("{")) {
            if (args.size() < 5 || !SeatInput::validateContext(args[2], args[3]).starts_with("{"))
                return "stale action context";
            owner = args[3];
        }
        if (SeatInput::contextReadonly(args[2]))
            return "readonly action context cannot dispatch";
        auto prefix = "seat context-dispatch " + args[2] + " ";
        if (!owner.empty())
            prefix += owner + " ";
        if (!request.starts_with(prefix))
            return "invalid action dispatch";
        SeatInput::CActionScope actionScope(SeatInput::contextSeat(args[2]));
        return dispatchRequest(format, "dispatch " + request.substr(prefix.size()));
    }
    if (args.size() == 2 && args[1] == "management-display") {
        try {
            return std::format("{{\"display\":\"{}\"}}", escapeJSONStrings(SeatInput::registry()->managementDisplay()));
        } catch (const std::exception& error) { return std::format("management display creation failed: {}", error.what()); }
    }
    if (args.size() == 2 && args[1] == "capabilities")
        return R"({"protocol":1,"dialect":"lua","features":["seat-input","seat-identity","atomic-snapshot","readonly-workspace","argb-frame","input-pause","lock-scope-v1","private-output-v1","management-display-v1","lock-aware-seat-input","lock-aware-export-v1","session-guard-v1","composed-seat-input","seat-shell-v1","native-seat-presentation-v1","physical-input-target-v1","physical-input-target-v2","primary-desktop-v1","managed-seat-config-v1","seat-action-context-v1","owned-seat-action-context-v1","authorized-seat-snapshot-v1"]})";
    if (args.size() == 7 && args[1] == "present")
        return SeatInput::presentation()->show(args[2], args[3], State::monitorState()->query().name(args[4]).run(), args[5], args[6]);
    if (args.size() == 3 && args[1] == "presentation")
        return SeatInput::presentation()->status(args[2]);
    if (args.size() == 3 && args[1] == "unpresent")
        return SeatInput::presentation()->hide(args[2]);
    if (args.size() == 4 && args[1] == "present-control" && (args[3] == "yes" || args[3] == "no"))
        return SeatInput::presentation()->control(args[2], args[3] == "yes");
    if (args.size() >= 6 && args[1] == "dispatch") {
        const auto seat = SeatInput::registry()->forName(args[2]);
        if (!seat || seat->socketName() != args[3] || std::to_string(seat->controlGeneration()) != args[4] || !seat->inputAllowed())
            return "stale or unavailable seat dispatcher";
        const auto prefix = std::format("seat dispatch {} {} {} ", args[2], args[3], args[4]);
        if (!request.starts_with(prefix))
            return "invalid seat dispatcher";
        SeatInput::CActionScope actionScope(seat);
        return dispatchRequest(format, "dispatch " + request.substr(prefix.size()));
    }
    if (args.size() == 2 && args[1] == "lock-state")
        return g_pSessionLockManager->protectionStateJSON();
    if (args.size() == 3 && args[1] == "create-private-output")
        return SeatInput::registry()->createPrivateOutput(args[2]);
    if (args.size() == 3 && args[1] == "private-output")
        return SeatInput::registry()->registerPrivateOutput(State::monitorState()->query().name(args[2]).run());
    if (args.size() == 2 && args[1] == "private-outputs") {
        std::string result = "[";
        for (const auto& monitor : State::monitorState()->monitors()) {
            if (!SeatInput::registry()->isPrivateOutput(monitor))
                continue;
            if (result.size() > 1)
                result += ",";
            result += std::format("\"{}\"", escapeJSONStrings(monitor->m_name));
        }
        return result + "]";
    }
    if (args.size() == 3 && args[1] == "state") {
        const auto seat = SeatInput::registry()->forName(args[2]);
        return seat ? seatState(seat) : "seat not found";
    }
    if (args.size() == 6 && (args[1] == "lock-policy" || args[1] == "export-grant")) {
        const auto seat = SeatInput::registry()->forName(args[2]);
        if (g_pSessionLockManager->isSessionLocked())
            return "management grants are disabled while locked";
        if (!seat || seat->socketName() != args[3] || std::to_string(seat->controlGeneration()) != args[4])
            return "stale seat identity or generation";
        if (args[1] == "lock-policy") {
            if (args[5] != "block" && args[5] != "allow")
                return "invalid lock policy";
            seat->setScopeExemptionAllowed(args[5] == "allow");
        } else {
            if (args[5].size() < 32 || args[5].size() > 128)
                return "invalid export grant";
            seat->setCaptureGrant(args[5]);
        }
        return seatState(seat);
    }
    if (args.size() >= 6 && (args[1] == "control" || args[1] == "act")) {
        const auto seat = SeatInput::registry()->forName(args[2]);
        if (!seat || seat->socketName() != args[3] || std::to_string(seat->controlGeneration()) != args[4])
            return "stale seat identity or control generation";
        if (args[1] == "control" && args.size() == 6 && (args[5] == "pause" || args[5] == "resume" || args[5] == "resume-composed")) {
            if (args[5] != "pause" && (g_pSessionLockManager->isSessionLocked() || !seat->viewAvailable()))
                return "seat view is unavailable";
            seat->setPaused(args[5] == "pause", args[5] == "resume-composed");
            return seatState(seat);
        }
        if (!seat->inputAllowed())
            return "seat input is paused or unavailable";
        if (args[5] == "workspace" && args.size() == 7)
            return seat->switchWorkspace(args[6]);
        if (args[5] == "focus" && args.size() == 7) {
            for (const auto& window : Desktop::windowState()->windows()) {
                if (window->m_workspace == seat->workspace() && SeatInput::registry()->windowIdentity(window) == args[6]) {
                    seat->focusWindow(window);
                    return seat->window() == window ? "ok" : "window cannot receive seat focus";
                }
            }
            return "window identity not found in seat workspace";
        }
        return "invalid seat action";
    }
    if ((args.size() == 7 && args[1] == "snapshot") || (args.size() == 9 && authorizedSnapshotCommand(args[1]))) {
        const auto seat = SeatInput::registry()->forName(args[2]);
        if (!seat || seat->socketName() != args[3])
            return "stale seat identity";
        const bool authorized = authorizedSnapshotCommand(args[1]);
        if (authorized && (std::to_string(seat->controlGeneration()) != args[7] || !seat->captureGranted(args[8]) || args[4] != "current"))
            return "invalid seat export authorization";
        const auto view = args[4] == "current" ? seat->workspace() : State::workspaceState()->query().input(args[4]).run();
        if (!view)
            return "observed workspace does not exist";
        if (args[5] != "png" && args[5] != "argb")
            return "unsupported capture format";
        // The compositor thread renders and serializes metadata in one request.
        // Existing-workspace observation never calls switchWorkspace or focus.
        const auto result = g_pHyprRenderer->captureSeatWorkspace(view, seat, args[6], args[5] == "argb", authorized);
        if (result != "ok")
            return result;
        static uint64_t nextFrame = 0;
        auto            metadata  = seatState(seat, view);
        metadata.pop_back();
        return metadata +
            std::format(",\"frameId\":\"{}\",\"timestampNs\":\"{}\",\"format\":\"{}\"}}", ++nextFrame,
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Time::steadyNow().time_since_epoch()).count(), args[5]);
    }
    if (args.size() == 3 && args[1] == "windows") {
        const auto seat = SeatInput::registry()->forName(args[2]);
        if (!seat)
            return "seat not found";
        std::string result = "[";
        for (const auto& window : Desktop::windowState()->windows()) {
            if (window->m_workspace != seat->workspace() || !window->mapped() || window->isHidden())
                continue;
            if (result.size() > 1)
                result += ",";
            result += std::format("{{\"id\":\"{}\",\"title\":\"{}\",\"address\":\"0x{:x}\"}}", SeatInput::registry()->windowIdentity(window),
                                  escapeJSONStrings(window->metadata().title()), rc<uintptr_t>(window.get()));
        }
        return result + "]";
    }

    if (args.size() == 1 || args[1] == "list") {
        std::string result = format == FORMAT_JSON ? "[" : "";
        bool        first  = true;
        for (const auto& seat : SeatInput::registry()->seats()) {
            if (!seat->active())
                continue;
            const auto monitor  = seat->monitor();
            const auto position = seat->pointer()->position();
            if (format == FORMAT_JSON) {
                if (!first)
                    result += ",";
                result += seatState(seat.get());
            } else
                result +=
                    std::format("{} output={} display={} cursor={},{}\n", seat->protocol()->seatName(), monitor ? monitor->m_name : "", seat->socketName(), position.x, position.y);
            first = false;
        }
        return format == FORMAT_JSON ? result + "]" : result;
    }
    if (args[1] == "create" && args.size() == 4)
        return SeatInput::registry()->create(args[2], State::monitorState()->query().name(args[3]).run());
    if (args[1] == "remove" && args.size() == 3)
        return SeatInput::registry()->remove(args[2]);
    if (args.size() == 4 && (args[1] == "workspace" || args[1] == "view-workspace")) {
        if (auto seat = SeatInput::registry()->forName(args[2]))
            return seat->switchWorkspace(args[3], args[1] == "view-workspace");
        return "seat not found";
    }
    if (args.size() >= 4 && args[1] == "focus") {
        const auto seat = SeatInput::registry()->forName(args[2]);
        if (!seat)
            return "seat not found";
        if (!seat->inputAllowed())
            return "seat is unavailable or session is locked";
        std::string selector = args[3];
        for (size_t i = 4; i < args.size(); ++i)
            selector += " " + args[i];
        const auto window = Desktop::viewState()->query().selector(selector).runWindow();
        if (!window || window->m_workspace != seat->workspace())
            return "window is not in seat workspace";
        seat->focusWindow(window);
        return seat->window() == window ? "ok" : "window cannot receive seat focus";
    }
    if (args.size() >= 4 && args[1] == "capture") {
        std::string path = args[3];
        for (size_t i = 4; i < args.size(); ++i)
            path += " " + args[i];
        if (args[2] == HL_SEAT_NAME) {
            const auto monitor = Desktop::focusState()->monitor();
            return g_pHyprRenderer->captureSeatWorkspace(monitor ? monitor->m_activeWorkspace : nullptr, nullptr, path);
        }
        if (auto seat = SeatInput::registry()->forName(args[2]))
            return g_pHyprRenderer->captureSeatWorkspace(seat->workspace(), seat, path);
        return "seat not found";
    }
    return "usage: seat [list|create NAME OUTPUT|remove NAME|workspace NAME WORKSPACE|focus NAME WINDOW_SELECTOR|capture NAME ABSOLUTE_PATH]";
}
