#pragma once
#include <optional>
#include <string>
class CSeatDesktop;
namespace SeatInput {
    CSeatDesktop* current();
    bool          physical();
    std::string   createContext(const std::string& owner = "");
    std::string   validateContext(const std::string& id, const std::string& owner = "");
    CSeatDesktop* contextSeat(const std::string& id);
    bool          contextReadonly(const std::string& id);
    class CActionScope {
      public:
        explicit CActionScope(CSeatDesktop* seat, std::optional<bool> physical = std::nullopt);
        ~CActionScope();
        CActionScope(const CActionScope&)            = delete;
        CActionScope& operator=(const CActionScope&) = delete;

      private:
        CSeatDesktop* m_previous         = nullptr;
        bool          m_previousPhysical = false;
    };
}
