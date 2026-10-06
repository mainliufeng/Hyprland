#pragma once

#include <vector>
#include <cstdint>
#include "WaylandProtocol.hpp"
#include "relative-pointer-unstable-v1.hpp"
#include "../helpers/math/Math.hpp"

class CSeatManager;
class CWLPointerResource;
class CRelativePointer {
  public:
    CRelativePointer(SP<CZwpRelativePointerV1> resource_, SP<CWLPointerResource> pointer);
    CSeatManager* manager() const;

    void          sendRelativeMotion(uint64_t time, const Vector2D& delta, const Vector2D& deltaUnaccel);
    void          sendRelativeMotion(uint32_t timeHi, uint32_t timeLo, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t dxUnaccel, wl_fixed_t dyUnaccel);

    bool          good();
    wl_client*    client();

  private:
    SP<CZwpRelativePointerV1> m_resource;
    SP<CWLPointerResource>    m_pointer;
    wl_client*                m_client = nullptr;
};

class CRelativePointerProtocol : public IWaylandProtocol {
  public:
    CRelativePointerProtocol(const wl_interface* iface, const int& ver, const std::string& name);

    virtual void bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id);

    void         sendRelativeMotion(uint64_t time, const Vector2D& delta, const Vector2D& deltaUnaccel, CSeatManager* seat = nullptr);

  private:
    void onManagerResourceDestroy(wl_resource* res);
    void destroyRelativePointer(CRelativePointer* pointer);
    void onGetRelativePointer(CZwpRelativePointerManagerV1* pMgr, uint32_t id, wl_resource* pointer);

    //
    std::vector<UP<CZwpRelativePointerManagerV1>> m_managers;
    std::vector<UP<CRelativePointer>>             m_relativePointers;

    friend class CRelativePointer;
};

namespace PROTO {
    inline UP<CRelativePointerProtocol> relativePointer;
};
