#pragma once
#include "../desktop/DesktopTypes.hpp"
#include "../helpers/math/Math.hpp"
#include "../helpers/time/Time.hpp"
#include "../devices/IKeyboard.hpp"
#include "../devices/IPointer.hpp"
#include <string>
#include <vector>
#include <map>

class CSeatDesktop;
namespace Render {
    class CRenderContext;
}

// Physical output presentation is independent of either seat's active workspace.
class CSeatPresentation {
  public:
    std::string        show(const std::string& name, const std::string& identity, PHLMONITOR output, const std::string& workspace, const std::string& owner);
    std::string        status(const std::string& owner, bool renew = true);
    std::string        control(const std::string& owner, bool enabled);
    std::string        hide(const std::string& owner);
    void               clear();
    void               validate();
    bool               activeFor(PHLMONITOR output) const;
    bool               active() const;
    CSeatDesktop*      seat() const;
    PHLWORKSPACE       workspace() const;
    PHLMONITOR         output() const;
    const std::string& owner() const;
    bool               controlling() const;
    bool               overlay() const;
    uint64_t           viewEpoch() const {
        return m_viewEpoch;
    }
    bool keyboardOverlay() const;
    bool draw(Render::CRenderContext& ctx, PHLMONITOR output, const Time::steady_tp& now);
    bool motion(const IPointer::SMotionEvent& event, const Vector2D& delta);
    bool absolute(const IPointer::SMotionAbsoluteEvent& event);
    bool button(const IPointer::SButtonEvent& event);
    bool axis(const IPointer::SAxisEvent& event);
    bool key(const IKeyboard::SKeyEvent& event, SP<IKeyboard> keyboard);
    bool modifiers(SP<IKeyboard> keyboard);
    bool frame();

  private:
    void                     pointerMotion(uint32_t time);
    bool                     overlayFocus(uint32_t time);
    Vector2D                 scenePoint(const Vector2D& position) const;
    std::string              m_name, m_identity, m_owner;
    PHLMONITORREF            m_output;
    PHLWORKSPACE             m_workspace;
    Vector2D                 m_savedCursor, m_cursor;
    Time::steady_tp          m_heartbeat;
    uint64_t                 m_generation = 0, m_frames = 0, m_viewEpoch = 0;
    bool                     m_control = false, m_overlay = false;
    std::vector<uint32_t>    m_overlayKeys;
    std::map<uint32_t, bool> m_buttonOwners;
};
inline UP<CSeatPresentation> g_pSeatPresentation;
