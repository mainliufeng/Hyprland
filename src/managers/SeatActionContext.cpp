#include "SeatActionContext.hpp"
#include "SeatDesktop.hpp"
#include "SeatPresentation.hpp"
#include "SessionLockManager.hpp"
#include "../Compositor.hpp"
#include "../helpers/time/Time.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "../workspace/RegularWorkspace.hpp"
#include "../protocols/core/Seat.hpp"
#include <map>
#include <format>

static CSeatDesktop* activeSeat     = nullptr;
static bool          physicalSource = false;
struct SActionContext {
    std::string     owner, name, identity, viewOwner, viewWorkspace;
    uint64_t        generation, lockEpoch;
    Time::steady_tp created;
    bool            readonly;
};
static std::map<std::string, SActionContext> contexts;
static uint64_t                              nextContext = 0;
CSeatDesktop*                                SeatInput::current() {
    return activeSeat;
}
bool SeatInput::physical() {
    return physicalSource || !activeSeat;
}
SeatInput::CActionScope::CActionScope(CSeatDesktop* seat, std::optional<bool> physical) : m_previous(activeSeat), m_previousPhysical(physicalSource) {
    activeSeat = seat;
    if (physical)
        physicalSource = *physical;
}
SeatInput::CActionScope::~CActionScope() {
    activeSeat     = m_previous;
    physicalSource = m_previousPhysical;
}

std::string SeatInput::createContext(const std::string& owner) {
    std::erase_if(contexts, [](const auto& item) { return Time::steadyNow() - item.second.created >= std::chrono::seconds(5); });
    if (!g_pSeatDesktopRegistry || contexts.size() >= 1024 || (g_pSessionLockManager->isSessionLocked() && !g_pSessionLockManager->allowsSeatInput(current())))
        return "";
    auto       seat = current();
    const bool view = physical() && g_pSeatPresentation && g_pSeatPresentation->active();
    if (view)
        seat = g_pSeatPresentation->seat();
    SActionContext context{.owner         = owner,
                           .name          = seat ? seat->protocol()->seatName() : "main",
                           .identity      = seat ? seat->socketName() : g_pCompositor->m_instanceSignature + "-primary",
                           .viewOwner     = view ? g_pSeatPresentation->owner() : "",
                           .viewWorkspace = view ? g_pSeatPresentation->workspace()->addressableName() : "",
                           .generation    = seat ? seat->controlGeneration() : g_pSeatDesktopRegistry->primaryGeneration(),
                           .lockEpoch     = g_pSessionLockManager->lockEpoch(),
                           .created       = Time::steadyNow(),
                           .readonly      = view && !g_pSeatPresentation->controlling()};
    auto           id = std::format("{}-{}", g_pCompositor->m_instanceSignature, ++nextContext);
    contexts.emplace(id, std::move(context));
    return id;
}

std::string SeatInput::validateContext(const std::string& id, const std::string& owner) {
    const auto found = contexts.find(id);
    if (found == contexts.end() || found->second.owner != owner || Time::steadyNow() - found->second.created >= std::chrono::seconds(5))
        return "stale action context";
    const auto& context = found->second;
    auto        seat    = g_pSeatDesktopRegistry->forName(context.name);
    if (context.lockEpoch != g_pSessionLockManager->lockEpoch() || (g_pSessionLockManager->isSessionLocked() && !g_pSessionLockManager->allowsSeatInput(seat)))
        return "lock action context changed";
    if (context.name != "main" &&
        (!seat || seat->socketName() != context.identity || seat->controlGeneration() != context.generation || (!context.readonly && !seat->inputAllowed())))
        return "seat action context changed";
    if (context.name == "main" && context.generation != g_pSeatDesktopRegistry->primaryGeneration())
        return "primary action context changed";
    if (!context.viewOwner.empty() &&
        (!g_pSeatPresentation->active() || g_pSeatPresentation->owner() != context.viewOwner || g_pSeatPresentation->seat() != seat ||
         g_pSeatPresentation->controlling() == context.readonly || g_pSeatPresentation->workspace()->addressableName() != context.viewWorkspace))
        return "view action context changed";
    return std::format("{{\"valid\":true,\"name\":\"{}\",\"seatId\":\"{}\",\"generation\":\"{}\",\"mode\":\"{}\"}}", escapeJSONStrings(context.name),
                       escapeJSONStrings(context.identity), context.generation, context.readonly ? "readonly" : "control");
}

CSeatDesktop* SeatInput::contextSeat(const std::string& id) {
    const auto found = contexts.find(id);
    return found == contexts.end() ? nullptr : g_pSeatDesktopRegistry->forName(found->second.name);
}
bool SeatInput::contextReadonly(const std::string& id) {
    const auto found = contexts.find(id);
    return found == contexts.end() || found->second.readonly;
}
