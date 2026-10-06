#include "../managers/SeatDesktop.hpp"
#include "PointerWarp.hpp"
#include "PointerConstraints.hpp"
#include "core/Compositor.hpp"
#include "core/Seat.hpp"
#include "../desktop/view/WLSurface.hpp"
#include "../managers/SeatManager.hpp"
#include "../pointer/PointerManager.hpp"
#include "../desktop/view/window/Window.hpp"
#include "desktop/view/LayerSurface.hpp"
#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Vector2D.hpp>

CPointerWarpProtocol::CPointerWarpProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    ;
}

void CPointerWarpProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto& RESOURCE = m_managers.emplace_back(makeUnique<CWpPointerWarpV1>(client, ver, id));

    if UNLIKELY (!RESOURCE->resource()) {
        wl_client_post_no_memory(client);
        m_managers.pop_back();
        return;
    }

    RESOURCE->setOnDestroy([this](CWpPointerWarpV1* pMgr) { destroyManager(pMgr); });
    RESOURCE->setDestroy([this](CWpPointerWarpV1* pMgr) { destroyManager(pMgr); });

    RESOURCE->setWarpPointer([](CWpPointerWarpV1* pMgr, wl_resource* surface, wl_resource* pointer, wl_fixed_t x, wl_fixed_t y, uint32_t serial) {
        const auto PSURFACE = CWLSurfaceResource::fromResource(surface);
        const auto POINTER  = CWLPointerResource::fromResource(pointer);
        if (!POINTER || !POINTER->m_owner)
            return;
        const auto PSEAT   = POINTER->m_owner.lock();
        const auto MANAGER = PSEAT->manager();
        if (MANAGER->m_state.pointerFocus != PSURFACE)
            return;

        CBox surfbox;

        auto HLSURF = Desktop::View::CWLSurface::fromResource(PSURFACE);

        if (!HLSURF)
            return;

        auto VIEW   = HLSURF->view();
        auto WINDOW = Desktop::View::CWindow::fromView(VIEW);
        if (WINDOW)
            surfbox = WINDOW->getWindowMainSurfaceBox();
        else {
            auto LAYERSURFACE = Desktop::View::CLayerSurface::fromView(VIEW);
            if (!LAYERSURFACE)
                return;

            auto BOX = LAYERSURFACE->logicalBox();
            if (!BOX.has_value())
                return;

            surfbox = BOX.value();
        }

        const auto LOCALPOS = Vector2D{wl_fixed_to_double(x), wl_fixed_to_double(y)};

        // Allow a margin of 1px on all sides
        surfbox.expand(1);
        const auto GLOBALPOS = LOCALPOS + surfbox.pos() + Vector2D{1., 1.};
        if (!surfbox.containsPoint(GLOBALPOS))
            return;

        if (!MANAGER->serialValid(PSEAT, serial, false))
            return;

        LOG(Log::DEBUG, "warped pointer to {}", GLOBALPOS);

        if (auto desktop = MANAGER->m_desktop) {
            if (!desktop->inputAllowed())
                return;
            auto position = GLOBALPOS;
            if (const auto constraint = HLSURF->constraint(MANAGER); constraint && constraint->isActive()) {
                if (constraint->isLocked())
                    return;
                position = constraint->logicConstraintRegion().closestPoint(position);
            }
            desktop->pointer()->warpTo(position);
            desktop->refocus();
            MANAGER->sendPointerFrame();
            return;
        } else
            Pointer::mgr()->warpTo(GLOBALPOS);
        MANAGER->sendPointerMotion(Time::millis(Time::steadyNow()), LOCALPOS);
        MANAGER->sendPointerFrame();
    });
}

void CPointerWarpProtocol::destroyManager(CWpPointerWarpV1* manager) {
    std::erase_if(m_managers, [&](const UP<CWpPointerWarpV1>& resource) { return resource.get() == manager; });
}
