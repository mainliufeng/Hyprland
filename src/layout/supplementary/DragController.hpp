#pragma once

#include "../target/Target.hpp"
#include "../../managers/input/InputManager.hpp"
#include "../../helpers/signal/Signal.hpp"

#include <optional>
#include <chrono>

class CSeatDesktop;

namespace Layout {
    enum eRectCorner : uint8_t;
}

namespace Layout::Supplementary {

    // DragStateController contains logic to begin and end a drag, which shouldn't be part of the layout's job. It's stuff like
    // toggling float when dragging tiled, remembering sizes, checking deltas, etc.
    class CDragStateController {
      public:
        explicit CDragStateController(CSeatDesktop* seat = nullptr);
        ~CDragStateController() = default;

        void           dragBegin(SP<ITarget> target, eMouseBindMode mode, std::optional<Layout::eRectCorner> forcedEdge = std::nullopt, bool exclusiveDeviceGrab = false);
        bool           dragEnd();

        void           mouseMove(const Vector2D& mousePos);
        eMouseBindMode mode() const;
        bool           wasDraggingWindow() const;
        bool           dragThresholdReached() const;
        void           resetDragThresholdReached();
        bool           draggingTiled() const;
        bool           exclusiveDeviceGrab() const;

        /*
            Called to try to pick up window for dragging.
            Updates drag related variables and floats window if threshold reached.
            Return true to reject
        */
        bool        updateDragWindow();

        SP<ITarget> target() const;

        struct {
            CSignalT<> motion;
            CSignalT<> ended;
        } m_events;

      private:
        Vector2D                                       mouseCoords() const;
        Vector2D                                       untransformedCoords() const;
        void                                           setCursorOverride(const std::string& shape);
        void                                           unsetCursorOverride();
        CSeatDesktop*                                  m_seat    = nullptr;
        std::chrono::high_resolution_clock::time_point m_timer   = std::chrono::high_resolution_clock::now();
        std::chrono::high_resolution_clock::time_point m_msTimer = m_timer;
        int                                            m_totalMs = 0;
        WP<ITarget>                                    m_target;

        eMouseBindMode                                 m_dragMode             = MBIND_INVALID;
        bool                                           m_wasDraggingWindow    = false;
        bool                                           m_dragThresholdReached = false;
        bool                                           m_draggingTiled        = false;
        bool                                           m_exclusiveDeviceGrab  = false;

        int                                            m_mouseMoveEventCount = 0;
        Vector2D                                       m_dragHotspot;
        Vector2D                                       m_beginDragXY;
        Vector2D                                       m_beginDragUntransformedXY;
        Vector2D                                       m_lastDragXY;
        Vector2D                                       m_beginDragPositionXY;
        Vector2D                                       m_beginDragSizeXY;
        Vector2D                                       m_draggingWindowOriginalFloatSize;
        Layout::eRectCorner                            m_grabbedCorner = sc<Layout::eRectCorner>(0) /* CORNER_NONE */;
        std::optional<Layout::eRectCorner>             m_forcedGrabbedCorner;
    };
};
