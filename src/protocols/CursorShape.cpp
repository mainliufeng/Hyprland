#include "core/Seat.hpp"
#include "CursorShape.hpp"
#include <algorithm>
#include "../helpers/CursorShapes.hpp"

CCursorShapeProtocol::CCursorShapeProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    ;
}

void CCursorShapeProtocol::onManagerResourceDestroy(wl_resource* res) {
    std::erase_if(m_managers, [res](const auto& other) { return other->resource() == res; });
}

void CCursorShapeProtocol::onDeviceResourceDestroy(wl_resource* res) {
    std::erase_if(m_devices, [this, res](const auto& other) {
        if (other->resource() != res)
            return false;
        m_pointers.erase(other.get());
        return true;
    });
}

void CCursorShapeProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_managers.emplace_back(makeUnique<CWpCursorShapeManagerV1>(client, ver, id)).get();
    RESOURCE->setOnDestroy([this](CWpCursorShapeManagerV1* p) { this->onManagerResourceDestroy(p->resource()); });

    RESOURCE->setDestroy([this](CWpCursorShapeManagerV1* pMgr) { this->onManagerResourceDestroy(pMgr->resource()); });
    RESOURCE->setGetPointer([this](CWpCursorShapeManagerV1* pMgr, uint32_t id, wl_resource* pointer) { this->onGetPointer(pMgr, id, pointer); });
    RESOURCE->setGetTabletToolV2([this](CWpCursorShapeManagerV1* pMgr, uint32_t id, wl_resource* tablet) { this->onGetTabletToolV2(pMgr, id, tablet); });
}

void CCursorShapeProtocol::onGetPointer(CWpCursorShapeManagerV1* pMgr, uint32_t id, wl_resource* pointer) {
    createCursorShapeDevice(pMgr, id, pointer);
    if (!m_devices.empty())
        m_pointers[m_devices.back().get()] = CWLPointerResource::fromResource(pointer);
}

void CCursorShapeProtocol::onGetTabletToolV2(CWpCursorShapeManagerV1* pMgr, uint32_t id, wl_resource* tablet) {
    createCursorShapeDevice(pMgr, id, tablet);
}

void CCursorShapeProtocol::createCursorShapeDevice(CWpCursorShapeManagerV1* pMgr, uint32_t id, wl_resource* resource) {
    const auto  CLIENT   = pMgr->client();
    const auto& RESOURCE = m_devices.emplace_back(makeUnique<CWpCursorShapeDeviceV1>(CLIENT, pMgr->version(), id));
    RESOURCE->setOnDestroy([this](CWpCursorShapeDeviceV1* p) { this->onDeviceResourceDestroy(p->resource()); });

    RESOURCE->setDestroy([this](CWpCursorShapeDeviceV1* p) { this->onDeviceResourceDestroy(p->resource()); });
    RESOURCE->setSetShape([this](CWpCursorShapeDeviceV1* p, uint32_t serial, wpCursorShapeDeviceV1Shape shape) { this->onSetShape(p, serial, shape); });
}

void CCursorShapeProtocol::onSetShape(CWpCursorShapeDeviceV1* pMgr, uint32_t serial, wpCursorShapeDeviceV1Shape shape) {
    if UNLIKELY (sc<uint32_t>(shape) == 0 || sc<uint32_t>(shape) >= CURSOR_SHAPE_NAMES.size()) {
        pMgr->error(WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE, "The shape is invalid");
        return;
    }

    SSetShapeEvent event;
    event.pMgr   = pMgr;
    event.serial = serial;
    if (const auto found = m_pointers.find(pMgr); found != m_pointers.end()) {
        if (!found->second || !found->second->m_owner)
            return;
        event.resource = found->second->m_owner.lock();
        event.seat     = event.resource->manager();
    }
    event.shape     = shape;
    event.shapeName = CURSOR_SHAPE_NAMES.at(shape);

    m_events.setShape.emit(event);
}
