#pragma once
class CSeatDesktop;
namespace SeatInput {
    CSeatDesktop* current();
    class CActionScope {
      public:
        explicit CActionScope(CSeatDesktop* seat);
        ~CActionScope();
        CActionScope(const CActionScope&)            = delete;
        CActionScope& operator=(const CActionScope&) = delete;

      private:
        CSeatDesktop* m_previous = nullptr;
    };
}
