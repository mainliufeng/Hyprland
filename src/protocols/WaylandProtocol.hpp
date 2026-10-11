#pragma once

#include "../defines.hpp"
#include "../helpers/memory/Memory.hpp"

#include <functional>

#define RESOURCE_OR_BAIL(resname)                                                                                                                                                  \
    const auto resname = (CWaylandResource*)wl_resource_get_user_data(resource);                                                                                                   \
    if (!resname)                                                                                                                                                                  \
        return;

#define PROTO NProtocols

class IWaylandProtocol;
struct SIWaylandProtocolDestroyWrapper {
    wl_listener       listener;
    IWaylandProtocol* parent = nullptr;
};

class IWaylandProtocol {
  public:
    IWaylandProtocol(const wl_interface* iface, const int& ver, const std::string& name, PHLMONITOR outputMonitor = nullptr);
    virtual ~IWaylandProtocol();

    virtual void                    onDisplayDestroy();
    virtual void                    removeGlobal();
    virtual wl_global*              getGlobal();
    PHLMONITOR                      outputMonitor() const;

    virtual void                    bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) = 0;

    SIWaylandProtocolDestroyWrapper m_liDisplayDestroy;

  private:
    std::string   m_name;
    PHLMONITORREF m_outputMonitor;
    wl_global*    m_global = nullptr;
};
