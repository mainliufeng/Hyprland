#include "SeatActionContext.hpp"
static CSeatDesktop* activeSeat = nullptr;
CSeatDesktop*        SeatInput::current() {
    return activeSeat;
}
SeatInput::CActionScope::CActionScope(CSeatDesktop* seat) : m_previous(activeSeat) {
    activeSeat = seat;
}
SeatInput::CActionScope::~CActionScope() {
    activeSeat = m_previous;
}
