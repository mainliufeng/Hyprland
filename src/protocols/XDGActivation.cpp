#include "XDGActivation.hpp"
#include "../managers/TokenManager.hpp"
#include "../Compositor.hpp"
#include "../managers/SeatManager.hpp"
#include "../managers/SeatDesktop.hpp"
#include "core/Seat.hpp"
#include "core/Compositor.hpp"
#include "../managers/SeatActionContext.hpp"
#include "../managers/SessionLockManager.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../desktop/view/window/Window.hpp"
#include "../workspace/RegularWorkspace.hpp"
#include "../output/Monitor.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

static uint64_t processStart(pid_t pid) {
    std::ifstream file(std::format("/proc/{}/stat", pid));
    std::string   line;
    std::getline(file, line);
    const auto end = line.rfind(") ");
    if (end == std::string::npos)
        return 0;
    std::istringstream fields(line.substr(end + 2));
    std::string        value;
    for (size_t i = 0; i < 20; ++i) {
        if (!(fields >> value))
            return 0;
    }
    try {
        return std::stoull(value);
    } catch (...) { return 0; }
}

CXDGActivationToken::CXDGActivationToken(SP<CXdgActivationTokenV1> resource_) : m_resource(resource_) {
    if UNLIKELY (!resource_->resource())
        return;

    m_resource->setDestroy([this](CXdgActivationTokenV1* r) { PROTO::activation->destroyToken(this); });
    m_resource->setOnDestroy([this](CXdgActivationTokenV1* r) { PROTO::activation->destroyToken(this); });

    m_resource->setSetSerial([this](CXdgActivationTokenV1* r, uint32_t serial_, wl_resource* seat) {
        m_serial = serial_;
        m_seat   = CWLSeatResource::fromResource(seat);
    });

    m_resource->setSetAppId([this](CXdgActivationTokenV1* r, const char* appid) { m_appID = appid; });

    m_resource->setCommit([this](CXdgActivationTokenV1* r) {
        auto rej = [this] {
            // is this fucking correct? Or should we just never send done???
            m_resource->sendDone("");
        };

        // TODO: should we send a protocol error of already_used here
        // if it was used? the protocol spec doesn't say _when_ it should be sent...
        if UNLIKELY (m_committed) {
            LOG(Log::WARN, "possible protocol error, two commits from one token, rejecting");
            rej();
            return;
        }

        m_committed = true;

        if UNLIKELY (!m_serial || !m_seat || m_seat->client() != m_resource->client() || !m_seat->manager()->serialValid(m_seat, m_serial)) {
            LOG(Log::WARN, "invalid serial {} for activation token, rejecting", m_serial);
            rej();
            return;
        }

        // send done with a new token
        m_token = g_pTokenManager->registerNewToken({}, std::chrono::seconds(30));

        LOG(Log::DEBUG, "assigned new xdg-activation token {}", m_token);

        m_resource->sendDone(m_token.c_str());

        const auto       desktop = m_seat->manager()->m_desktop;
        const auto       monitor = Desktop::focusState()->monitor();
        SActivationRoute route{.workspace  = desktop ? desktop->workspace() :
                                   monitor           ? monitor->getCurrentWorkspace() :
                                                       nullptr,
                               .seatSocket = desktop ? desktop->socketName() : "",
                               .generation = desktop ? desktop->controlGeneration() : SeatInput::registry()->primaryGeneration(),
                               .lockEpoch  = g_pSessionLockManager->lockEpoch(),
                               .created    = Time::steadyNow()};
        PROTO::activation->m_sentTokens.push_back({m_token, route.seatSocket, m_resource->client(), route, ""});

        auto count = std::ranges::count_if(PROTO::activation->m_sentTokens, [this](const auto& other) { return other.client == m_resource->client(); });

        if UNLIKELY (count > 10) {
            // remove first token. Too many, dear app.
            for (auto i = PROTO::activation->m_sentTokens.begin(); i != PROTO::activation->m_sentTokens.end(); ++i) {
                if (i->client == m_resource->client()) {
                    PROTO::activation->m_sentTokens.erase(i);
                    break;
                }
            }
        }
    });
}

CXDGActivationToken::~CXDGActivationToken() {
    if (m_committed)
        g_pTokenManager->removeToken(g_pTokenManager->getToken(m_token));
}

bool CXDGActivationToken::good() {
    return m_resource->resource();
}

CXDGActivationProtocol::CXDGActivationProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    // Accepted empty workspaces survive until their launch maps. Expired
    // routes release them even when no subsequent user action arrives.
    m_expiryTimer = makeShared<CEventLoopTimer>(
        std::chrono::seconds(1),
        [this](SP<CEventLoopTimer> timer, void*) {
            std::erase_if(m_sentTokens, [this](const auto& item) { return !validRoute(item.route); });
            std::erase_if(m_launches, [this](const auto& item) { return !validRoute(item.second.route); });
            timer->updateTimeout(std::chrono::seconds(1));
        },
        nullptr);
    g_pEventLoopManager->addTimer(m_expiryTimer);
}

CXDGActivationProtocol::~CXDGActivationProtocol() {
    m_expiryTimer->cancel();
}

void CXDGActivationProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_managers.emplace_back(makeUnique<CXdgActivationV1>(client, ver, id)).get();
    RESOURCE->setOnDestroy([this](CXdgActivationV1* p) { this->onManagerResourceDestroy(p->resource()); });

    RESOURCE->setDestroy([this](CXdgActivationV1* pMgr) { this->onManagerResourceDestroy(pMgr->resource()); });
    RESOURCE->setGetActivationToken([this](CXdgActivationV1* pMgr, uint32_t id) { this->onGetToken(pMgr, id); });
    RESOURCE->setActivate([this](CXdgActivationV1* pMgr, const char* token, wl_resource* surface) {
        auto TOKEN = std::ranges::find_if(m_sentTokens, [token](const auto& t) { return t.token == token; });

        if UNLIKELY (TOKEN == m_sentTokens.end()) {
            LOG(Log::WARN, "activate event for non-existent token {}??", token);
            return;
        }

        if (!validRoute(TOKEN->route)) {
            m_sentTokens.erase(TOKEN);
            return;
        }
        const auto route   = TOKEN->route;
        auto*      desktop = seatForRoute(route);

        // remove token. It's been now spent.
        m_sentTokens.erase(TOKEN);

        SP<CWLSurfaceResource> surf    = CWLSurfaceResource::fromResource(surface);
        const auto             PWINDOW = Desktop::viewState()->query().type(Desktop::View::VIEW_TYPE_WINDOW).surface(surf).runWindow();

        if UNLIKELY (!PWINDOW) {
            LOG(Log::WARN, "activate event for non-window or gone surface with token {}, ignoring", token);
            return;
        }

        PWINDOW->activateFromToken(route.workspace, desktop);
    });
}

CSeatDesktop* CXDGActivationProtocol::seatForRoute(const SActivationRoute& route) const {
    for (const auto& seat : SeatInput::registry()->seats()) {
        if (seat->active() && seat->socketName() == route.seatSocket)
            return seat.get();
    }
    return nullptr;
}

bool CXDGActivationProtocol::validRoute(const SActivationRoute& route) const {
    if (!route.workspace || !route.workspace->m_monitor || !route.workspace->m_monitor->m_enabled || Time::steadyNow() - route.created >= std::chrono::seconds(30) ||
        route.lockEpoch != g_pSessionLockManager->lockEpoch())
        return false;
    const auto seat = seatForRoute(route);
    if (!route.seatSocket.empty())
        return seat && seat->inputAllowed() && seat->controlGeneration() == route.generation;
    return !g_pSessionLockManager->isSessionLocked() && SeatInput::registry()->primaryGeneration() == route.generation;
}

std::string CXDGActivationProtocol::createActionToken(const std::string& context, PHLWORKSPACE workspace) {
    if (!SeatInput::validateContext(context).starts_with("{") || SeatInput::contextReadonly(context))
        return "";
    if (!workspace)
        workspace = SeatInput::contextWorkspace(context);
    const auto seat = SeatInput::contextSeat(context);
    if (!workspace)
        return "";
    std::erase_if(m_sentTokens, [this](const auto& token) { return !validRoute(token.route); });
    if (m_sentTokens.size() >= 1024)
        return "";
    const auto       token = g_pTokenManager->getRandomUUID();
    SActivationRoute route{.workspace  = workspace,
                           .seatSocket = seat ? seat->socketName() : "",
                           .generation = seat ? seat->controlGeneration() : SeatInput::registry()->primaryGeneration(),
                           .lockEpoch  = g_pSessionLockManager->lockEpoch(),
                           .created    = Time::steadyNow()};
    m_sentTokens.push_back({token, route.seatSocket, nullptr, route, context});
    return token;
}

std::string CXDGActivationProtocol::registerLaunch(const std::string& actionToken, pid_t pid) {
    const auto token   = std::ranges::find_if(m_sentTokens, [&](const auto& item) { return item.token == actionToken && validRoute(item.route); });
    const auto started = processStart(pid);
    if (token == m_sentTokens.end() || !started)
        return "launch context unavailable";
    std::erase_if(m_launches, [this](const auto& item) { return !validRoute(item.second.route); });
    if (m_launches.size() >= 1024)
        return "launch context limit reached";
    m_launches[pid] = {started, token->route};
    return "ok";
}

std::optional<SActivationRoute> CXDGActivationProtocol::routeForClient(wl_client* client) {
    pid_t pid = 0;
    wl_client_get_credentials(client, &pid, nullptr, nullptr);
    // Follow process identity, never titles, app names, command lines or env.
    // Descendants created by shell/terminal launchers retain the same route.
    for (size_t depth = 0; pid > 1 && depth < 32; ++depth) {
        const auto found = m_launches.find(pid);
        if (found != m_launches.end() && validRoute(found->second.route) && found->second.started == processStart(pid))
            return found->second.route;
        pid = getPPIDof(pid);
    }
    return std::nullopt;
}

void CXDGActivationProtocol::onManagerResourceDestroy(wl_resource* res) {
    std::erase_if(m_managers, [&](const auto& other) { return other->resource() == res; });
}

void CXDGActivationProtocol::destroyToken(CXDGActivationToken* token) {
    std::erase_if(m_tokens, [&](const auto& other) { return other.get() == token; });
}

void CXDGActivationProtocol::onGetToken(CXdgActivationV1* pMgr, uint32_t id) {
    const auto CLIENT   = pMgr->client();
    const auto RESOURCE = m_tokens.emplace_back(makeUnique<CXDGActivationToken>(makeShared<CXdgActivationTokenV1>(CLIENT, pMgr->version(), id))).get();

    if UNLIKELY (!RESOURCE->good()) {
        pMgr->noMemory();
        m_tokens.pop_back();
        return;
    }
}
