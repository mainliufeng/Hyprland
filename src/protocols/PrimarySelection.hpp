#pragma once

#include <vector>
#include <cstdint>
#include "WaylandProtocol.hpp"
#include "primary-selection-unstable-v1.hpp"
#include "types/DataDevice.hpp"
#include <hyprutils/os/FileDescriptor.hpp>

class CPrimarySelectionOffer;
class CPrimarySelectionSource;
class CWLSeatResource;
class CSeatManager;

class CPrimarySelectionDevice;
class CPrimarySelectionManager;

class CPrimarySelectionOffer {
  public:
    CPrimarySelectionOffer(SP<CZwpPrimarySelectionOfferV1> resource_, SP<IDataSource> source_);

    bool            good();
    void            sendData();

    CSeatManager*   m_seat = nullptr;
    bool            m_dead = false;

    WP<IDataSource> m_source;

  private:
    SP<CZwpPrimarySelectionOfferV1> m_resource;

    friend class CPrimarySelectionDevice;
};

class CPrimarySelectionSource : public IDataSource {
  public:
    CPrimarySelectionSource(SP<CZwpPrimarySelectionSourceV1> resource_, SP<CPrimarySelectionDevice> device_);
    ~CPrimarySelectionSource();

    static SP<CPrimarySelectionSource> fromResource(wl_resource*);

    bool                               good();

    virtual std::vector<std::string>   mimes();
    virtual void                       send(const std::string& mime, Hyprutils::OS::CFileDescriptor fd);
    virtual void                       accepted(const std::string& mime);
    virtual void                       cancelled();
    virtual void                       error(uint32_t code, const std::string& msg);

    std::vector<std::string>           m_mimeTypes;
    WP<CPrimarySelectionSource>        m_self;
    WP<CPrimarySelectionDevice>        m_device;

  private:
    SP<CZwpPrimarySelectionSourceV1> m_resource;
};

class CPrimarySelectionDevice {
  public:
    CPrimarySelectionDevice(SP<CZwpPrimarySelectionDeviceV1> resource_, SP<CWLSeatResource> seat);
    CSeatManager*               manager() const;

    bool                        good();
    wl_client*                  client();

    void                        sendDataOffer(SP<CPrimarySelectionOffer> offer);
    void                        sendSelection(SP<CPrimarySelectionOffer> selection);

    WP<CPrimarySelectionDevice> m_self;

  private:
    CHyprSignalListener              m_focusListener;
    SP<CWLSeatResource>              m_seat;
    SP<CZwpPrimarySelectionDeviceV1> m_resource;
    wl_client*                       m_client = nullptr;

    friend class CPrimarySelectionProtocol;
};

class CPrimarySelectionManager {
  public:
    CPrimarySelectionManager(SP<CZwpPrimarySelectionDeviceManagerV1> resource_);

    bool                                     good();

    WP<CPrimarySelectionDevice>              m_device;
    std::vector<WP<CPrimarySelectionSource>> m_sources;

  private:
    SP<CZwpPrimarySelectionDeviceManagerV1> m_resource;
};

class CPrimarySelectionProtocol : public IWaylandProtocol {
  public:
    CPrimarySelectionProtocol(const wl_interface* iface, const int& ver, const std::string& name);

    virtual void bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id);

  private:
    void destroyResource(CPrimarySelectionManager* resource);
    void destroyResource(CPrimarySelectionDevice* resource);
    void destroyResource(CPrimarySelectionSource* resource);
    void destroyResource(CPrimarySelectionOffer* resource);

    //
    std::vector<SP<CPrimarySelectionManager>> m_managers;
    std::vector<SP<CPrimarySelectionDevice>>  m_devices;
    std::vector<SP<CPrimarySelectionSource>>  m_sources;
    std::vector<SP<CPrimarySelectionOffer>>   m_offers;

    //
    void setSelection(SP<IDataSource> source, CSeatManager* seat = nullptr);
    void sendSelectionToDevice(SP<CPrimarySelectionDevice> dev, SP<IDataSource> sel);
    void updateSelection(CSeatManager* seat = nullptr);
    void onPointerFocus(CSeatManager* seat = nullptr);

    //
    SP<CPrimarySelectionDevice> dataDeviceForClient(wl_client*, CSeatManager* seat = nullptr);

    friend class CPrimarySelectionManager;
    friend class CPrimarySelectionDevice;
    friend class CPrimarySelectionSource;
    friend class CPrimarySelectionOffer;
    friend class CSeatManager;

    struct {
        CHyprSignalListener onPointerFocusChange;
    } m_listeners;
};

namespace PROTO {
    inline UP<CPrimarySelectionProtocol> primarySelection;
};
