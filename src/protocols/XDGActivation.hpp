#pragma once

#include <vector>
#include <cstdint>
#include "WaylandProtocol.hpp"
#include "xdg-activation-v1.hpp"
#include "../desktop/DesktopTypes.hpp"
#include "../helpers/time/Time.hpp"
#include <optional>
#include <unordered_map>

class CWLSeatResource;
class CSeatDesktop;
class CEventLoopTimer;

struct SActivationRoute {
    PHLWORKSPACE    workspace;
    std::string     seatSocket;
    uint64_t        generation = 0;
    uint64_t        lockEpoch  = 0;
    Time::steady_tp created;
};

class CXDGActivationToken {
  public:
    CXDGActivationToken(SP<CXdgActivationTokenV1> resource_);
    ~CXDGActivationToken();

    bool good();

  private:
    SP<CXdgActivationTokenV1> m_resource;
    SP<CWLSeatResource>       m_seat;

    uint32_t                  m_serial    = 0;
    std::string               m_appID     = "";
    bool                      m_committed = false;

    std::string               m_token = "";

    friend class CXDGActivationProtocol;
};

class CXDGActivationProtocol : public IWaylandProtocol {
  public:
    CXDGActivationProtocol(const wl_interface* iface, const int& ver, const std::string& name);
    ~CXDGActivationProtocol() override;

    virtual void                    bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id);
    std::string                     createActionToken(const std::string& context, PHLWORKSPACE workspace = nullptr);
    std::string                     registerLaunch(const std::string& context, pid_t pid);
    std::optional<SActivationRoute> routeForClient(wl_client* client);
    CSeatDesktop*                   seatForRoute(const SActivationRoute& route) const;

  private:
    void onManagerResourceDestroy(wl_resource* res);
    void destroyToken(CXDGActivationToken* pointer);
    void onGetToken(CXdgActivationV1* pMgr, uint32_t id);
    bool validRoute(const SActivationRoute& route) const;

    struct SSentToken {
        std::string      token;
        std::string      seatSocket;
        wl_client*       client = nullptr; // READ-ONLY: can be dead
        SActivationRoute route;
        std::string      context;
    };
    std::vector<SSentToken> m_sentTokens;
    struct SLaunch {
        uint64_t         started = 0;
        SActivationRoute route;
    };
    std::unordered_map<pid_t, SLaunch> m_launches;
    SP<CEventLoopTimer>                m_expiryTimer;

    //
    std::vector<UP<CXdgActivationV1>>    m_managers;
    std::vector<UP<CXDGActivationToken>> m_tokens;

    friend class CXDGActivationToken;
};

namespace PROTO {
    inline UP<CXDGActivationProtocol> activation;
};
