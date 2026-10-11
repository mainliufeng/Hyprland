#pragma once
#include "../helpers/memory/Memory.hpp"
#include "../keybinds/Registry.hpp"
#include "../helpers/time/Time.hpp"
#include <string>
#include <map>

namespace Desktop::View {
    class CLayerSurface;
}
namespace SeatConfig {
    struct SBinding {
        std::vector<std::string> keys;
        std::string              action, argument;
        bool                     physicalOnly      = false;
        bool                     release           = false;
        bool                     viewOnly          = false;
        bool                     overrideInherited = false;
    };
    struct SOverlay {
        std::string name;
        int64_t     pid         = 0;
        bool        keyboard    = false;
        bool        localInView = false;
    };
    struct SConfiguration {
        std::string           owner, seatName, seatId, callback;
        std::vector<SBinding> bindings;
        std::vector<SOverlay> overlays;
    };
    class CConfiguration {
      public:
        std::string ensureWorkspace(const std::string& name, const std::string& identity, const std::string& workspace);
        std::string configure(const std::string& json);
        std::string renew(const std::string& owner);
        std::string remove(const std::string& owner);
        bool        live(const std::string& owner) const;
        bool        overlay(const SP<Desktop::View::CLayerSurface>& layer, const std::string& seatId, bool keyboard = false, bool localInView = false) const;

      private:
        struct SController {
            SConfiguration               config;
            Time::steady_tp              renewed;
            std::vector<Keybinds::PBind> bindings;
        };
        std::map<std::string, SController> m_controllers;
    };
    UP<CConfiguration>& manager();
}
