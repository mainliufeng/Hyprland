#include "DataDevice.hpp"
#include <algorithm>
#include "../../managers/SeatManager.hpp"
#include "../../managers/SeatDesktop.hpp"
#include "../../pointer/PointerManager.hpp"
#include "../../managers/eventLoop/EventLoopManager.hpp"
#include "../../Compositor.hpp"
#include "../../render/pass/TexPassElement.hpp"
#include "Seat.hpp"
#include "Compositor.hpp"
#include "../../xwayland/XWayland.hpp"
#include "../../xwayland/Server.hpp"
#include "../../managers/input/InputManager.hpp"
#include "../../pointer/cursor/CursorShapeOverrideController.hpp"
#include "../../output/Monitor.hpp"
#include "../../render/Renderer.hpp"
#include "../../xwayland/Dnd.hpp"
#include "../../event/EventBus.hpp"
using namespace Hyprutils::OS;

CWLDataOfferResource::CWLDataOfferResource(SP<CWlDataOffer> resource_, SP<IDataSource> source_) : m_source(source_), m_resource(resource_) {
    if UNLIKELY (!good())
        return;

    m_resource->setDestroy([this](CWlDataOffer* r) { PROTO::data->destroyResource(this); });
    m_resource->setOnDestroy([this](CWlDataOffer* r) { PROTO::data->destroyResource(this); });

    m_resource->setAccept([this](CWlDataOffer* r, uint32_t serial, const char* mime) {
        if (!m_source) {
            LOG(Log::WARN, "Possible bug: Accept on an offer w/o a source");
            return;
        }

        if (m_dead) {
            LOG(Log::WARN, "Possible bug: Accept on an offer that's dead");
            return;
        }

        LOG(Log::DEBUG, "Offer {:x} accepts data from source {:x} with mime {}", (uintptr_t)this, (uintptr_t)m_source.get(), mime ? mime : "null");

        m_source->accepted(mime ? mime : "");
        m_accepted = mime;
    });

    m_resource->setReceive([this](CWlDataOffer* r, const char* mime, int fd) {
        CFileDescriptor sendFd{fd};
        if (!m_source) {
            LOG(Log::WARN, "Possible bug: Receive on an offer w/o a source");
            return;
        }

        if (m_dead) {
            LOG(Log::WARN, "Possible bug: Receive on an offer that's dead");
            return;
        }

        LOG(Log::DEBUG, "Offer {:x} asks to send data from source {:x}", (uintptr_t)this, (uintptr_t)m_source.get());

        if (!m_accepted) {
            LOG(Log::WARN, "Offer was never accepted, sending accept first");
            m_source->accepted(mime ? mime : "");
        }

        m_source->send(mime ? mime : "", std::move(sendFd));

        m_recvd = true;

        // if (source->hasDnd())
        //     PROTO::data->completeDrag(m_seat);
    });

    m_resource->setFinish([this](CWlDataOffer* r) {
        m_dead = true;
        if (!m_source || !m_recvd || !m_accepted)
            PROTO::data->abortDrag(m_seat);
        else
            PROTO::data->completeDrag(m_seat);
    });
}

CWLDataOfferResource::~CWLDataOfferResource() {
    if (!m_source || !m_source->hasDnd() || m_dead)
        return;

    m_source->sendDndFinished();
}

bool CWLDataOfferResource::good() {
    return m_resource->resource();
}

void CWLDataOfferResource::sendData() {
    if (!m_source)
        return;

    const auto SOURCEACTIONS = m_source->actions();

    if (m_resource->version() >= 3 && SOURCEACTIONS > 0) {
        m_resource->sendSourceActions(SOURCEACTIONS);
        if (SOURCEACTIONS & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
            m_resource->sendAction(WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
        else if (SOURCEACTIONS & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
            m_resource->sendAction(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        else {
            LOG(Log::ERR, "Client bug? dnd source has no action move or copy. Sending move, f this.");
            m_resource->sendAction(WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
        }
    }

    for (auto const& m : m_source->mimes()) {
        LOG(Log::DEBUG, " | offer {:x} supports mime {}", (uintptr_t)this, m);
        m_resource->sendOffer(m.c_str());
    }
}

eDataSourceType CWLDataOfferResource::type() {
    return DATA_SOURCE_TYPE_WAYLAND;
}

SP<CWLDataOfferResource> CWLDataOfferResource::getWayland() {
    return m_self.lock();
}

SP<CX11DataOffer> CWLDataOfferResource::getX11() {
    return nullptr;
}

SP<IDataSource> CWLDataOfferResource::getSource() {
    return m_source.lock();
}

CWLDataSourceResource::CWLDataSourceResource(SP<CWlDataSource> resource_, SP<CWLDataDeviceResource> device_) : m_device(device_), m_resource(resource_) {
    if UNLIKELY (!good())
        return;

    m_resource->setData(this);

    m_resource->setDestroy([this](CWlDataSource* r) {
        m_events.destroy.emit();
        PROTO::data->onDestroyDataSource(m_self);
        PROTO::data->destroyResource(this);
    });
    m_resource->setOnDestroy([this](CWlDataSource* r) {
        m_events.destroy.emit();
        PROTO::data->onDestroyDataSource(m_self);
        PROTO::data->destroyResource(this);
    });

    m_resource->setOffer([this](CWlDataSource* r, const char* mime) { m_mimeTypes.emplace_back(mime); });
    m_resource->setSetActions([this](CWlDataSource* r, uint32_t a) {
        LOG(Log::DEBUG, "DataSource {:x} actions {}", (uintptr_t)this, a);
        m_supportedActions = a;
    });
}

CWLDataSourceResource::~CWLDataSourceResource() {
    m_events.destroy.emit();
    PROTO::data->onDestroyDataSource(m_self);
}

SP<CWLDataSourceResource> CWLDataSourceResource::fromResource(wl_resource* res) {
    auto data = sc<CWLDataSourceResource*>(sc<CWlDataSource*>(wl_resource_get_user_data(res))->data());
    return data ? data->m_self.lock() : nullptr;
}

bool CWLDataSourceResource::good() {
    return m_resource->resource();
}

void CWLDataSourceResource::accepted(const std::string& mime) {
    if (mime.empty()) {
        m_resource->sendTarget(nullptr);
        return;
    }

    if (std::ranges::find(m_mimeTypes, mime) == m_mimeTypes.end()) {
        LOG(Log::ERR, "Compositor/App bug: CWLDataSourceResource::sendAccepted with non-existent mime");
        return;
    }

    m_resource->sendTarget(mime.c_str());
}

std::vector<std::string> CWLDataSourceResource::mimes() {
    return m_mimeTypes;
}

void CWLDataSourceResource::send(const std::string& mime, CFileDescriptor fd) {
    if (std::ranges::find(m_mimeTypes, mime) == m_mimeTypes.end()) {
        LOG(Log::ERR, "Compositor/App bug: CWLDataSourceResource::sendAskSend with non-existent mime");
        return;
    }

    m_resource->sendSend(mime.c_str(), fd.get());
}

void CWLDataSourceResource::cancelled() {
    m_resource->sendCancelled();
}

bool CWLDataSourceResource::hasDnd() {
    return m_dnd;
}

bool CWLDataSourceResource::dndDone() {
    return m_dndSuccess;
}

void CWLDataSourceResource::error(uint32_t code, const std::string& msg) {
    m_resource->error(code, msg);
}

void CWLDataSourceResource::sendDndDropPerformed() {
    if (m_resource->version() < 3)
        return;
    m_resource->sendDndDropPerformed();
    m_dropped = true;
}

void CWLDataSourceResource::sendDndFinished() {
    if (m_resource->version() < 3)
        return;
    m_resource->sendDndFinished();
}

void CWLDataSourceResource::sendDndAction(wl_data_device_manager_dnd_action a) {
    if (m_resource->version() < 3)
        return;
    m_resource->sendAction(a);
}

uint32_t CWLDataSourceResource::actions() {
    return m_supportedActions;
}

eDataSourceType CWLDataSourceResource::type() {
    return DATA_SOURCE_TYPE_WAYLAND;
}

CWLDataDeviceResource::CWLDataDeviceResource(SP<CWlDataDevice> resource_, SP<CWLSeatResource> seat) : m_seat(seat), m_resource(resource_) {
    if UNLIKELY (!good())
        return;

    m_resource->setRelease([this](CWlDataDevice* r) { PROTO::data->destroyResource(this); });
    m_resource->setOnDestroy([this](CWlDataDevice* r) { PROTO::data->destroyResource(this); });

    m_client = m_resource->client();
    if (manager() != g_pSeatManager.get())
        m_focusListener = manager()->m_events.keyboardFocusChange.listen([this] { PROTO::data->onKeyboardFocus(manager()); });

    m_resource->setSetSelection([this](CWlDataDevice* r, wl_resource* sourceR, uint32_t serial) {
        auto source = sourceR ? CWLDataSourceResource::fromResource(sourceR) : CSharedPointer<CWLDataSourceResource>{};
        if (!source) {
            LOG(Log::DEBUG, "Reset selection received");
            manager()->setCurrentSelection(nullptr);
            return;
        }

        if (source && source->m_used)
            LOG(Log::WARN, "setSelection on a used resource. By protocol, this is a violation, but firefox et al insist on doing this.");

        source->m_device = m_self;
        source->markUsed();

        manager()->setCurrentSelection(source);
    });

    m_resource->setStartDrag([this](CWlDataDevice* r, wl_resource* sourceR, wl_resource* origin, wl_resource* icon, uint32_t serial) {
        auto source = sourceR ? CWLDataSourceResource::fromResource(sourceR) : nullptr;
        if (!source) {
            LOG(Log::ERR, "No source in drag");
            return;
        }

        if (source && source->m_used)
            LOG(Log::WARN, "setSelection on a used resource. By protocol, this is a violation, but firefox et al insist on doing this.");

        source->markUsed();

        const auto originSurface = CWLSurfaceResource::fromResource(origin);
        if (!manager()->serialValid(m_seat, serial, false) || manager()->m_state.pointerFocus != originSurface)
            return;
        source->m_device = m_self;
        source->m_dnd    = true;

        PROTO::data->initiateDrag(source, icon ? CWLSurfaceResource::fromResource(icon) : nullptr, originSurface, manager());
    });
}

bool CWLDataDeviceResource::good() {
    return m_resource->resource();
}

wl_client* CWLDataDeviceResource::client() {
    return m_client;
}

void CWLDataDeviceResource::sendDataOffer(SP<IDataOffer> offer) {
    if (!offer)
        m_resource->sendDataOfferRaw(nullptr);
    else if (const auto WL = offer->getWayland(); WL)
        m_resource->sendDataOffer(WL->m_resource.get());
    //FIXME: X11
}

void CWLDataDeviceResource::sendEnter(uint32_t serial, SP<CWLSurfaceResource> surf, const Vector2D& local, SP<IDataOffer> offer) {
    if (const auto WL = offer->getWayland(); WL)
        m_resource->sendEnterRaw(serial, surf->getResource()->resource(), wl_fixed_from_double(local.x), wl_fixed_from_double(local.y), WL->m_resource->resource());

    m_entered = surf;

    // FIXME: X11
}

void CWLDataDeviceResource::sendLeave() {
    if (!m_entered)
        return;

    m_entered.reset();
    m_resource->sendLeave();
}

void CWLDataDeviceResource::sendMotion(uint32_t timeMs, const Vector2D& local) {
    m_resource->sendMotion(timeMs, wl_fixed_from_double(local.x), wl_fixed_from_double(local.y));
}

void CWLDataDeviceResource::sendDrop() {
    m_resource->sendDrop();
}

void CWLDataDeviceResource::sendSelection(SP<IDataOffer> offer) {
    if (!offer)
        m_resource->sendSelectionRaw(nullptr);
    else if (const auto WL = offer->getWayland(); WL)
        m_resource->sendSelection(WL->m_resource.get());
}

eDataSourceType CWLDataDeviceResource::type() {
    return DATA_SOURCE_TYPE_WAYLAND;
}

SP<CWLDataDeviceResource> CWLDataDeviceResource::getWayland() {
    return m_self.lock();
}

SP<CX11DataDevice> CWLDataDeviceResource::getX11() {
    return nullptr;
}

CWLDataDeviceManagerResource::CWLDataDeviceManagerResource(SP<CWlDataDeviceManager> resource_) : m_resource(resource_) {
    if UNLIKELY (!good())
        return;

    m_resource->setOnDestroy([this](CWlDataDeviceManager* r) { PROTO::data->destroyResource(this); });

    m_resource->setCreateDataSource([this](CWlDataDeviceManager* r, uint32_t id) {
        std::erase_if(m_sources, [](const auto& e) { return e.expired(); });

        const auto RESOURCE = PROTO::data->m_sources.emplace_back(makeShared<CWLDataSourceResource>(makeShared<CWlDataSource>(r->client(), r->version(), id), m_device.lock()));

        if UNLIKELY (!RESOURCE->good()) {
            r->noMemory();
            PROTO::data->m_sources.pop_back();
            return;
        }

        if (!m_device)
            LOG(Log::WARN, "New data source before a device was created");

        RESOURCE->m_self = RESOURCE;

        m_sources.emplace_back(RESOURCE);

        LOG(Log::DEBUG, "New data source bound at {:x}", (uintptr_t)RESOURCE.get());
    });

    m_resource->setGetDataDevice([this](CWlDataDeviceManager* r, uint32_t id, wl_resource* seat) {
        const auto RESOURCE =
            PROTO::data->m_devices.emplace_back(makeShared<CWLDataDeviceResource>(makeShared<CWlDataDevice>(r->client(), r->version(), id), CWLSeatResource::fromResource(seat)));

        if UNLIKELY (!RESOURCE->good()) {
            r->noMemory();
            PROTO::data->m_devices.pop_back();
            return;
        }

        RESOURCE->m_self = RESOURCE;
        m_device         = RESOURCE;

        for (auto const& s : m_sources) {
            if (!s)
                continue;
            s->m_device = RESOURCE;
        }

        LOG(Log::DEBUG, "New data device bound at {:x}", (uintptr_t)RESOURCE.get());
    });
}

bool CWLDataDeviceManagerResource::good() {
    return m_resource->resource();
}

CWLDataDeviceProtocol::CWLDataDeviceProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    g_pEventLoopManager->doLater([this]() {
        m_listeners.onKeyboardFocusChange   = g_pSeatManager->m_events.keyboardFocusChange.listen([this] { onKeyboardFocus(); });
        m_listeners.onDndPointerFocusChange = g_pSeatManager->m_events.dndPointerFocusChange.listen([this] { onDndPointerFocus(); });
    });
}

void CWLDataDeviceProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_managers.emplace_back(makeShared<CWLDataDeviceManagerResource>(makeShared<CWlDataDeviceManager>(client, ver, id)));

    if UNLIKELY (!RESOURCE->good()) {
        wl_client_post_no_memory(client);
        m_managers.pop_back();
        return;
    }

    LOG(Log::DEBUG, "New datamgr resource bound at {:x}", (uintptr_t)RESOURCE.get());
}

void CWLDataDeviceProtocol::destroyResource(CWLDataDeviceManagerResource* seat) {
    std::erase_if(m_managers, [&](const auto& other) { return other.get() == seat; });
}

void CWLDataDeviceProtocol::destroyResource(CWLDataDeviceResource* resource) {
    std::erase_if(m_devices, [&](const auto& other) { return other.get() == resource; });
}

void CWLDataDeviceProtocol::destroyResource(CWLDataSourceResource* resource) {
    std::erase_if(m_sources, [&](const auto& other) { return other.get() == resource; });
}

void CWLDataDeviceProtocol::destroyResource(CWLDataOfferResource* resource) {
    std::erase_if(m_offers, [&](const auto& other) { return other.get() == resource; });
}

SP<IDataDevice> CWLDataDeviceProtocol::dataDeviceForClient(wl_client* c, CSeatManager* seat) {
    seat = seat ? seat : g_pSeatManager.get();
#ifndef NO_XWAYLAND
    if (seat == g_pSeatManager.get() && g_pXWayland && g_pXWayland->m_server && c == g_pXWayland->m_server->m_xwaylandClient)
        return g_pXWayland->m_wm->getDataDevice();
#endif

    auto it = std::ranges::find_if(m_devices, [c, seat](const auto& e) { return e->client() == c && e->manager() == seat; });
    if (it == m_devices.end())
        return nullptr;
    return *it;
}

void CWLDataDeviceProtocol::sendSelectionToDevice(SP<IDataDevice> dev, SP<IDataSource> sel) {
    if (!sel) {
        dev->sendSelection(nullptr);
        return;
    }

    SP<IDataOffer> offer;

    if (const auto WL = dev->getWayland(); WL) {
        const auto OFFER = m_offers.emplace_back(makeShared<CWLDataOfferResource>(makeShared<CWlDataOffer>(WL->m_resource->client(), WL->m_resource->version(), 0), sel));
        if UNLIKELY (!OFFER->good()) {
            WL->m_resource->noMemory();
            m_offers.pop_back();
            return;
        }
        OFFER->m_seat   = WL->manager();
        OFFER->m_source = sel;
        OFFER->m_self   = OFFER;
        offer           = OFFER;
    }
#ifndef NO_XWAYLAND
    else if (const auto X11 = dev->getX11(); X11)
        offer = g_pXWayland->m_wm->createX11DataOffer(g_pSeatManager->m_state.keyboardFocus.lock(), sel);
#endif

    if UNLIKELY (!offer) {
        LOG(Log::ERR, "No offer could be created in sendSelectionToDevice");
        return;
    }

    LOG(Log::DEBUG, "New {} offer {:x} for data source {:x}", offer->type() == DATA_SOURCE_TYPE_WAYLAND ? "wayland" : "X11", (uintptr_t)offer.get(), (uintptr_t)sel.get());

    dev->sendDataOffer(offer);
    if (const auto WL = offer->getWayland(); WL)
        WL->sendData();
    dev->sendSelection(offer);
}

void CWLDataDeviceProtocol::onDestroyDataSource(WP<CWLDataSourceResource> source) {
    for (auto& [seat, drag] : m_drags) {
        if (drag.currentSource == source)
            abortDrag(seat);
    }
}

void CWLDataDeviceProtocol::setSelection(SP<IDataSource> source, CSeatManager* seat) {
    seat = seat ? seat : g_pSeatManager.get();
    for (auto const& o : m_offers) {
        if (o->m_seat != seat)
            continue;
        if (o->m_source && o->m_source->hasDnd())
            continue;
        o->m_dead = true;
    }

    if (!source) {
        LOG(Log::DEBUG, "resetting selection");

        if (!seat->m_state.keyboardFocusResource)
            return;

        auto DESTDEVICE = dataDeviceForClient(seat->m_state.keyboardFocusResource->client(), seat);
        if (DESTDEVICE && DESTDEVICE->type() == DATA_SOURCE_TYPE_WAYLAND)
            sendSelectionToDevice(DESTDEVICE, nullptr);

        return;
    }

    LOG(Log::DEBUG, "New selection for data source {:x}", (uintptr_t)source.get());

    if (!seat->m_state.keyboardFocusResource)
        return;

    auto DESTDEVICE = dataDeviceForClient(seat->m_state.keyboardFocusResource->client(), seat);

    if (!DESTDEVICE) {
        LOG(Log::DEBUG, "CWLDataDeviceProtocol::setSelection: cannot send selection to a client without a data_device");
        return;
    }

    if (DESTDEVICE->type() != DATA_SOURCE_TYPE_WAYLAND) {
        LOG(Log::DEBUG, "CWLDataDeviceProtocol::setSelection: ignoring X11 data device");
        return;
    }

    sendSelectionToDevice(DESTDEVICE, source);
}

void CWLDataDeviceProtocol::updateSelection(CSeatManager* seat) {
    seat = seat ? seat : g_pSeatManager.get();
    if (!seat->m_state.keyboardFocusResource)
        return;

    auto DESTDEVICE = dataDeviceForClient(seat->m_state.keyboardFocusResource->client(), seat);

    if (!DESTDEVICE) {
        LOG(Log::DEBUG, "CWLDataDeviceProtocol::onKeyboardFocus: cannot send selection to a client without a data_device");
        return;
    }

    sendSelectionToDevice(DESTDEVICE, seat->m_selection.currentSelection.lock());
}

void CWLDataDeviceProtocol::onKeyboardFocus(CSeatManager* seat) {
    seat = seat ? seat : g_pSeatManager.get();
    for (auto const& o : m_offers) {
        if (o->m_seat != seat)
            continue;
        if (o->m_source && o->m_source->hasDnd())
            continue;
        o->m_dead = true;
    }

    updateSelection(seat);
}

void CWLDataDeviceProtocol::onDndPointerFocus(CSeatManager* seat) {
    seat       = seat ? seat : g_pSeatManager.get();
    auto& drag = m_drags[seat];
    for (auto const& o : m_offers) {
        if (o->m_seat != seat)
            continue;
        if (o->m_source && !o->m_source->hasDnd())
            continue;
        o->m_dead = true;
    }

    updateDrag(seat);
}

void CWLDataDeviceProtocol::initiateDrag(WP<CWLDataSourceResource> currentSource, SP<CWLSurfaceResource> dragSurface, SP<CWLSurfaceResource> origin, CSeatManager* seat) {
    seat       = seat ? seat : g_pSeatManager.get();
    auto& drag = m_drags[seat];

    if (drag.currentSource) {
        LOG(Log::WARN, "New drag started while old drag still active??");
        abortDrag(seat);
    }

    if (!seat->m_desktop) {
        Pointer::Cursor::overrideController->setOverride("grabbing", Pointer::Cursor::CURSOR_OVERRIDE_DND);
        drag.overriddenCursor = true;
    }

    // For touch-initiated drags, anchor the drag icon to the touch point
    // right away. Until the first touch motion arrives it would otherwise
    // be rendered at the (possibly far away) mouse cursor position.
    if (!seat->m_desktop && g_pInputManager->m_lastInputTouch)
        drag.touchPos = g_pInputManager->m_touchData.lastTouchPos;

    LOG(Log::DEBUG, "initiateDrag: source {:x}, surface: {:x}, origin: {:x}", (uintptr_t)currentSource.get(), (uintptr_t)dragSurface.get(), (uintptr_t)origin.get());

    currentSource->m_used = true;

    drag.currentSource = currentSource;
    drag.originSurface = origin;
    drag.dndSurface    = dragSurface;
    if (dragSurface) {
        drag.dndSurfaceDestroy = dragSurface->m_events.destroy.listen([this, seat] { abortDrag(seat); });
        drag.dndSurfaceCommit  = dragSurface->m_events.commit.listen([this, seat] {
            auto& drag = m_drags[seat];
            if (drag.dndSurface->m_current.texture && !drag.dndSurface->m_mapped) {
                drag.dndSurface->map();
                return;
            }

            if (!drag.dndSurface->m_current.texture && drag.dndSurface->m_mapped) {
                drag.dndSurface->unmap();
                return;
            }
        });
    }

    if (!seat->m_desktop) {
        drag.mouseButton = Event::bus()->m_events.input.mouse.button.listen([this, seat](IPointer::SButtonEvent e, Event::SCallbackInfo&) {
            auto& drag = m_drags[seat];
            if (e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
                LOG(Log::DEBUG, "Dropping drag on mouseUp");
                dropDrag(seat);
            }
        });

        drag.touchUp = Event::bus()->m_events.input.touch.up.listen([this, seat](ITouch::SUpEvent e, Event::SCallbackInfo&) {
            auto& drag = m_drags[seat];
            LOG(Log::DEBUG, "Dropping drag on touchUp");
            dropDrag(seat);
        });

        drag.tabletTip = Event::bus()->m_events.input.tablet.tip.listen([this, seat](CTablet::STipEvent e, Event::SCallbackInfo&) {
            auto& drag = m_drags[seat];
            if (!e.in) {
                LOG(Log::DEBUG, "Dropping drag on tablet tipUp");
                dropDrag(seat);
            }
        });

        drag.mouseMove = Event::bus()->m_events.input.mouse.move.listen([this, seat](Vector2D pos, Event::SCallbackInfo&) {
            auto& drag = m_drags[seat];
            if (drag.focusedDevice && seat->m_state.dndPointerFocus) {
                auto surf = Desktop::View::CWLSurface::fromResource(seat->m_state.dndPointerFocus.lock());

                if (!surf)
                    return;

                const auto box = surf->getSurfaceBoxGlobal();

                if (!box.has_value())
                    return;

                drag.focusedDevice->sendMotion(Time::millis(Time::steadyNow()), pos - box->pos());
                LOG(Log::DEBUG, "Drag motion {}", pos - box->pos());
            }
        });

        drag.touchMove = Event::bus()->m_events.input.touch.motion.listen([this, seat](ITouch::SMotionEvent e, Event::SCallbackInfo&) {
            auto& drag = m_drags[seat];
            // Mirror the mouse drag path: the input layer (CInputManager::onTouchMove)
            // refocuses dndPointerFocus from the global touch position; here we just
            // send surface-local motion against the currently focused dnd surface.
            if (drag.focusedDevice && seat->m_state.dndPointerFocus) {
                auto surf = Desktop::View::CWLSurface::fromResource(seat->m_state.dndPointerFocus.lock());

                if (!surf)
                    return;

                const auto box = surf->getSurfaceBoxGlobal();

                if (!box.has_value())
                    return;

                const auto POS = g_pInputManager->m_touchData.lastTouchPos;

                drag.touchPos = POS;

                drag.focusedDevice->sendMotion(e.timeMs, POS - box->pos());
                LOG(Log::DEBUG, "Drag motion {}", POS - box->pos());
            }
        });
    }
    // unfocus the pointer from the surface, this is part of """standard""" wayland procedure and gtk will freak out if this isn't happening.
    // BTW, the spec does NOT require this explicitly...
    // Fuck you gtk.
    const auto LASTDNDFOCUS = seat->m_state.dndPointerFocus;
    seat->setPointerFocus(nullptr, {});
    seat->m_state.dndPointerFocus = LASTDNDFOCUS;

    // make a new offer, etc
    updateDrag(seat);
}

void CWLDataDeviceProtocol::updateDrag(CSeatManager* seat) {
    seat       = seat ? seat : g_pSeatManager.get();
    auto& drag = m_drags[seat];
    if (!dndActive(seat))
        return;

    if (drag.focusedDevice)
        drag.focusedDevice->sendLeave();

    auto surface = seat->m_state.dndPointerFocus.lock();
    if (!surface)
        return;

    drag.focusedDevice = dataDeviceForClient(surface->client(), seat);

    if (!drag.focusedDevice)
        return;

    SP<IDataOffer> offer;

    if (const auto WL = drag.focusedDevice->getWayland(); WL) {
        const auto OFFER =
            m_offers.emplace_back(makeShared<CWLDataOfferResource>(makeShared<CWlDataOffer>(WL->m_resource->client(), WL->m_resource->version(), 0), drag.currentSource.lock()));
        if (!OFFER->good()) {
            WL->m_resource->noMemory();
            m_offers.pop_back();
            return;
        }
        OFFER->m_seat   = seat;
        OFFER->m_source = drag.currentSource;
        OFFER->m_self   = OFFER;
        offer           = OFFER;
    }
#ifndef NO_XWAYLAND
    else if (const auto X11 = drag.focusedDevice->getX11(); X11)
        offer = g_pXWayland->m_wm->createX11DataOffer(seat->m_state.keyboardFocus.lock(), drag.currentSource.lock());
#endif

    if (!offer) {
        LOG(Log::ERR, "No offer could be created in updateDrag");
        return;
    }

    LOG(Log::DEBUG, "New {} dnd offer {:x} for data source {:x}", offer->type() == DATA_SOURCE_TYPE_WAYLAND ? "wayland" : "X11", (uintptr_t)offer.get(),
        (uintptr_t)drag.currentSource.get());

    drag.focusedDevice->sendDataOffer(offer);
    if (const auto WL = offer->getWayland(); WL)
        WL->sendData();
    drag.focusedDevice->sendEnter(wl_display_next_serial(g_pCompositor->m_wlDisplay), surface, surface->m_current.size / 2.F, offer);
}

void CWLDataDeviceProtocol::cleanupDndState(bool resetDevice, bool resetSource, bool simulateInput, CSeatManager* seat) {
    seat       = seat ? seat : g_pSeatManager.get();
    auto& drag = m_drags[seat];
    drag.dndSurface.reset();
    drag.dndSurfaceCommit.reset();
    drag.dndSurfaceDestroy.reset();
    drag.mouseButton.reset();
    drag.mouseMove.reset();
    drag.touchUp.reset();
    drag.touchMove.reset();
    drag.touchPos.reset();
    drag.tabletTip.reset();

    if (resetDevice)
        drag.focusedDevice.reset();
    if (resetSource)
        drag.currentSource.reset();

    if (simulateInput) {
        if (seat->m_desktop)
            seat->m_desktop->refocus();
        else
            g_pInputManager->simulateMouseMovement();
        seat->resendEnterEvents();
    }
}

void CWLDataDeviceProtocol::dropDrag(CSeatManager* seat) {
    seat       = seat ? seat : g_pSeatManager.get();
    auto& drag = m_drags[seat];
    if (!drag.focusedDevice || !drag.currentSource) {
        if (drag.currentSource)
            abortDrag(seat);
        return;
    }

    if (!wasDragSuccessful(seat)) {
        abortDrag(seat);
        return;
    }

    drag.focusedDevice->sendDrop();

#ifndef NO_XWAYLAND
    if (drag.focusedDevice->getX11()) {
        drag.focusedDevice->sendLeave();
        if (drag.overriddenCursor)
            Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_DND);
        drag.overriddenCursor = false;
        cleanupDndState(true, true, true, seat);
        return;
    }
#endif

    drag.focusedDevice->sendLeave();
    if (drag.overriddenCursor)
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_DND);
    drag.overriddenCursor = false;
    cleanupDndState(false, false, false, seat);
}

bool CWLDataDeviceProtocol::wasDragSuccessful(CSeatManager* seat) {
    seat       = seat ? seat : g_pSeatManager.get();
    auto& drag = m_drags[seat];
    if (!drag.currentSource)
        return false;

    for (auto const& o : m_offers) {
        if (o->m_dead || o->m_source != drag.currentSource)
            continue;

        if (o->m_recvd || o->m_accepted)
            return true;
    }

#ifndef NO_XWAYLAND
    if (drag.focusedDevice->getX11())
        return true;
#endif

    return false;
}

void CWLDataDeviceProtocol::completeDrag(CSeatManager* seat) {
    seat       = seat ? seat : g_pSeatManager.get();
    auto& drag = m_drags[seat];
    if (!drag.focusedDevice && !drag.currentSource)
        return;

    if (drag.currentSource) {
        drag.currentSource->sendDndDropPerformed();
        drag.currentSource->sendDndFinished();
    }

    cleanupDndState(true, true, true, seat);
}

void CWLDataDeviceProtocol::abortDrag(CSeatManager* seat) {
    seat       = seat ? seat : g_pSeatManager.get();
    auto& drag = m_drags[seat];
    cleanupDndState(false, false, false, seat);

    if (drag.overriddenCursor)
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_DND);
    drag.overriddenCursor = false;

    if (!drag.focusedDevice && !drag.currentSource)
        return;

    if (drag.focusedDevice) {
#ifndef NO_XWAYLAND
        if (auto x11Device = drag.focusedDevice->getX11(); x11Device)
            x11Device->forceCleanupDnd();
#endif
        drag.focusedDevice->sendLeave();
    }

    if (drag.currentSource)
        drag.currentSource->cancelled();

    drag.focusedDevice.reset();
    drag.currentSource.reset();

    if (seat->m_desktop)
        seat->m_desktop->refocus();
    else
        g_pInputManager->simulateMouseMovement();
    seat->resendEnterEvents();
}

void CWLDataDeviceProtocol::renderDND(Render::CRenderContext& ctx, PHLMONITOR pMonitor, const Time::steady_tp& when) {
    for (auto& [seat, drag] : m_drags) {
        if (ctx.m_sceneSeat ?
                seat->m_desktop != ctx.m_sceneSeat :
                (seat->m_desktop && (!seat->m_desktop->inputAllowed() || seat->m_desktop->monitor() != pMonitor || seat->m_desktop->workspace() != pMonitor->m_activeWorkspace)))
            continue;
        if (!drag.dndSurface || !drag.dndSurface->m_current.texture)
            continue;

        const auto POS = drag.touchPos.value_or(seat->m_desktop ? seat->m_desktop->pointer()->position() : g_pInputManager->getMouseCoordsInternal());

        Vector2D   surfacePos = POS;

        surfacePos += drag.dndSurface->m_current.offset;

        CBox                         box = CBox{surfacePos, drag.dndSurface->m_current.size}.translate(-pMonitor->m_position).scale(pMonitor->m_scale).round();

        CTexPassElement::SRenderData data;
        data.tex = drag.dndSurface->m_current.texture;
        data.box = box;
        g_pHyprRenderer->addPassElement(ctx, makeUnique<CTexPassElement>(std::move(data)));

        if (!ctx.readOnlyEffects()) {
            CBox damageBox = CBox{surfacePos, drag.dndSurface->m_current.size}.expand(5);
            g_pHyprRenderer->damageBox(damageBox);
            drag.dndSurface->frame(when);
        }
    }
}

bool CWLDataDeviceProtocol::dndActive(CSeatManager* seat) {
    seat             = seat ? seat : g_pSeatManager.get();
    const auto found = m_drags.find(seat);
    return found != m_drags.end() && !!found->second.currentSource;
}

void CWLDataDeviceProtocol::abortDndIfPresent(CSeatManager* seat) {
    if (!dndActive(seat))
        return;
    abortDrag(seat);
}

CSeatManager* CWLDataDeviceResource::manager() const {
    return m_seat->manager();
}

void CWLDataDeviceProtocol::dragMotion(CSeatManager* seat, const Vector2D& position, uint32_t timeMs) {
    if (!dndActive(seat))
        return;
    auto&      drag    = m_drags[seat];
    const auto surface = Desktop::View::CWLSurface::fromResource(seat->m_state.dndPointerFocus.lock());
    LOG(Log::DEBUG, "DND motion for seat {:x}, device {:x}, surface {:x}, at {}", (uintptr_t)seat, (uintptr_t)drag.focusedDevice.get(), (uintptr_t)surface.get(), position);
    if (!drag.focusedDevice || !surface)
        return;
    const auto box = surface->getSurfaceBoxGlobal();
    if (box)
        drag.focusedDevice->sendMotion(timeMs, position - box->pos());
}
void CWLDataDeviceProtocol::dragButton(CSeatManager* seat, uint32_t state) {
    if (dndActive(seat) && state == WL_POINTER_BUTTON_STATE_RELEASED) {
        LOG(Log::DEBUG, "Dropping DND for seat {:x}", (uintptr_t)seat);
        dropDrag(seat);
    }
}

void CWLDataDeviceProtocol::forgetSeat(CSeatManager* seat) {
    abortDndIfPresent(seat);
    m_drags.erase(seat);
}
