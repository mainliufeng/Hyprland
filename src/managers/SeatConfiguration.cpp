#include "SeatConfiguration.hpp"
#include "SeatDesktop.hpp"
#include "SeatActionContext.hpp"
#include "SeatPresentation.hpp"
#include "SessionLockManager.hpp"
#include "../Compositor.hpp"
#include "../protocols/core/Seat.hpp"
#include "../desktop/view/LayerSurface.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../state/WorkspaceState.hpp"
#include "../workspace/RegularWorkspace.hpp"
#include "../config/shared/actions/ConfigActions.hpp"
#include "../keybinds/Manager.hpp"
#include "../ipc/s2/S2.hpp"
#include "../helpers/MiscFunctions.hpp"
#include <glaze/glaze.hpp>
#include <algorithm>
#include <format>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>

using namespace SeatConfig;

UP<CConfiguration>& SeatConfig::manager() {
    static auto configuration = makeUnique<CConfiguration>();
    return configuration;
}

bool CConfiguration::live(const std::string& owner) const {
    const auto found = m_controllers.find(owner);
    return found != m_controllers.end() && Time::steadyNow() - found->second.renewed < std::chrono::seconds(5);
}

std::string CConfiguration::renew(const std::string& owner) {
    const auto found = m_controllers.find(owner);
    if (found == m_controllers.end() || !live(owner))
        return "configuration lease expired";
    for (const auto& bind : found->second.bindings)
        if (!Keybinds::mgr()->registry().contains(bind))
            return "configuration registry was reloaded";
    found->second.renewed = Time::steadyNow();
    return "ok";
}

std::string CConfiguration::remove(const std::string& owner) {
    if (const auto found = m_controllers.find(owner); found != m_controllers.end()) {
        for (const auto& bind : found->second.bindings)
            Keybinds::mgr()->removeBind(bind);
        m_controllers.erase(found);
    }
    return "ok";
}

bool CConfiguration::overlay(const SP<Desktop::View::CLayerSurface>& layer, const std::string& seatId, bool keyboard, bool localInView) const {
    if (!layer)
        return false;
    for (const auto& [owner, controller] : m_controllers) {
        if (!live(owner) || controller.config.seatId != seatId)
            continue;
        for (const auto& rule : controller.config.overlays)
            if (rule.pid == layer->getPID() && rule.name == layer->m_namespace && (!keyboard || rule.keyboard) && (!localInView || rule.localInView))
                return true;
    }
    return false;
}

std::string CConfiguration::configure(const std::string& json) {
    if (json.size() > 65536 || g_pSessionLockManager->isSessionLocked())
        return "configuration unavailable";
    SConfiguration config;
    if (const auto error = glz::read_json(config, json); error)
        return "invalid configuration JSON";
    auto seat = g_pSeatDesktopRegistry->forName(config.seatName);
    if (!seat || seat->socketName() != config.seatId || config.owner.size() < 32 || config.owner.size() > 128 || config.bindings.size() > 128 || config.overlays.size() > 128)
        return "invalid configuration identity or limits";
    if (!config.callback.empty() &&
        (config.callback.size() >= sizeof(sockaddr_un::sun_path) ||
         !config.callback.starts_with(std::string{getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/nonexistent"} + "/")))
        return "invalid controller callback";
    for (const auto& rule : config.overlays)
        if (rule.pid <= 0 || rule.name.empty() || rule.name.size() > 256)
            return "invalid surface routing rule";
    if (!m_controllers.contains(config.owner) && m_controllers.size() >= 64) {
        std::vector<std::string> expired;
        for (const auto& [owner, controller] : m_controllers)
            if (!live(owner))
                expired.push_back(owner);
        for (const auto& owner : expired)
            remove(owner);
        if (m_controllers.size() >= 64)
            return "controller configuration limit reached";
    }
    std::vector<Keybinds::CBind> definitions;
    std::vector<std::string>     conflicts;
    for (const auto& definition : config.bindings) {
        if (definition.keys.empty() || definition.keys.size() > 8 || definition.argument.size() > 4096 ||
            (definition.action != "notify" && definition.action != "workspace" && definition.action != "move" && definition.action != "dismiss"))
            return "invalid binding action";
        if (definition.viewOnly && definition.action == "move")
            return "readonly bindings cannot mutate windows";
        auto callback = [definition, owner = config.owner, name = config.seatName, identity = config.seatId, callback = config.callback]() -> Keybinds::SBindResult {
            auto current = g_pSeatDesktopRegistry->forName(name);
            if (!manager()->live(owner) || !current || current->socketName() != identity)
                return {.success = false, .error = "stale binding context"};
            const bool viewing = !SeatInput::current() && g_pSeatPresentation->active() && !g_pSeatPresentation->controlling() && g_pSeatPresentation->seat() == current;
            if (definition.viewOnly != viewing || (g_pSessionLockManager->isSessionLocked() && !g_pSessionLockManager->allowsSeatInput(current)))
                return {.success = false, .error = "binding context changed"};
            if (definition.action == "notify") {
                const auto context = SeatInput::createContext(owner);
                if (context.empty() || callback.empty())
                    return {.success = false, .error = "controller context unavailable"};
                const auto event = std::format(
                    "{{\"actionId\":\"{}\",\"owner\":\"{}\",\"seat\":\"{}\",\"seatId\":\"{}\",\"generation\":\"{}\",\"viewOwner\":\"{}\",\"mode\":\"{}\",\"action\":\"{}\"}}",
                    escapeJSONStrings(context), escapeJSONStrings(owner), escapeJSONStrings(name), escapeJSONStrings(identity), current->controlGeneration(),
                    viewing ? escapeJSONStrings(g_pSeatPresentation->owner()) : "", viewing ? "readonly" : "control", escapeJSONStrings(definition.argument));
                const auto fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
                if (fd < 0)
                    return {.success = false, .error = "controller socket unavailable"};
                sockaddr_un address{};
                address.sun_family = AF_UNIX;
                std::memcpy(address.sun_path, callback.c_str(), callback.size() + 1);
                const auto connected = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
                const auto message   = std::format("{{\"id\":\"{}\",\"method\":\"controller-action\",\"params\":{}}}\n", escapeJSONStrings(context), event);
                const auto sent      = connected == 0 ? send(fd, message.data(), message.size(), MSG_NOSIGNAL) : -1;
                close(fd);
                if (sent != static_cast<ssize_t>(message.size()))
                    return {.success = false, .error = "controller event delivery failed"};
                return {.passEvent = false};
            }
            if (definition.action == "dismiss") {
                if (viewing)
                    g_pSeatPresentation->clear();
                return {.passEvent = false};
            }
            if (viewing) {
                const auto ensured = manager()->ensureWorkspace(name, identity, definition.argument);
                if (ensured != "ok")
                    return {.success = false, .error = ensured};
                const auto result = g_pSeatPresentation->show(name, identity, g_pSeatPresentation->output(), definition.argument, g_pSeatPresentation->owner());
                return {.success = result.starts_with("{"), .error = result.starts_with("{") ? "" : result};
            }
            if (!current->inputAllowed())
                return {.success = false, .error = "seat input unavailable"};
            const auto result =
                definition.action == "workspace" ? Config::Actions::changeWorkspace(definition.argument) : Config::Actions::moveToWorkspace(definition.argument, true);
            return {.success = bool(result), .error = result ? "" : result.error().message};
        };
        std::string display;
        for (const auto& key : definition.keys) {
            if (!display.empty())
                display += " + ";
            display += key;
        }
        Keybinds::SExtraBindArgs args{.metadata = {.displayKey        = display,
                                                   .description       = "Managed seat binding",
                                                   .handler           = definition.action,
                                                   .argument          = definition.argument,
                                                   .seatIdentity      = config.seatId,
                                                   .controller        = config.owner,
                                                   .viewOnly          = definition.viewOnly,
                                                   .overrideInherited = definition.overrideInherited}};
        auto                     bind = Keybinds::CBind::make(std::vector<std::string>{definition.keys}, 0, std::move(callback), std::move(args));
        if (!bind)
            return "invalid binding: " + bind.error();
        for (const auto& existing : definitions)
            if (existing.metadata().viewOnly == definition.viewOnly && existing.modifierMask() == bind->modifierMask() && std::ranges::equal(existing.keyNames(), bind->keyNames()))
                return "duplicate binding in configuration: " + display;
        for (const auto& existing : Keybinds::mgr()->registry().binds()) {
            if (existing->modifierMask() != bind->modifierMask() || existing->keyNames().size() != bind->keyNames().size())
                continue;
            if (!existing->metadata().controller.empty()) {
                const auto& metadata = existing->metadata();
                if (metadata.controller != config.owner && live(metadata.controller) && metadata.seatIdentity == config.seatId && metadata.viewOnly == definition.viewOnly &&
                    std::ranges::equal(existing->keyNames(), bind->keyNames()))
                    return "binding conflicts with another controller: " + display;
                continue;
            }
            if (std::ranges::equal(existing->keyNames(), bind->keyNames())) {
                conflicts.push_back(display);
                if (!definition.overrideInherited)
                    return "binding conflicts with existing config: " + display;
            }
        }
        definitions.emplace_back(std::move(*bind));
    }
    remove(config.owner);
    auto& controller   = m_controllers[config.owner];
    controller.config  = std::move(config);
    controller.renewed = Time::steadyNow();
    for (auto& definition : definitions)
        controller.bindings.push_back(Keybinds::mgr()->addBind(std::move(definition)));
    std::string encoded;
    glz::write_json(conflicts, encoded);
    return "{\"configured\":true,\"inheritedOverrides\":" + encoded + "}";
}

std::string CConfiguration::ensureWorkspace(const std::string& name, const std::string& identity, const std::string& workspace) {
    const auto seat = g_pSeatDesktopRegistry->forName(name);
    if (!seat || seat->socketName() != identity || g_pSessionLockManager->isSessionLocked())
        return "workspace management unavailable";
    return seat->reserveWorkspace(workspace);
}
