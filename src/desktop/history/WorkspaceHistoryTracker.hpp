#pragma once

#include "../DesktopTypes.hpp"
#include "../../SharedDefs.hpp"
#include "../../macros.hpp"
#include "../../helpers/MiscFunctions.hpp"
#include "../../state/workspace/Target.hpp"

#include <deque>

namespace Desktop::History {
    class CWorkspaceHistoryTracker {
      public:
        explicit CWorkspaceHistoryTracker(bool observeGlobalEvents = true);
        ~CWorkspaceHistoryTracker() = default;

        CWorkspaceHistoryTracker(const CWorkspaceHistoryTracker&) = delete;
        CWorkspaceHistoryTracker(CWorkspaceHistoryTracker&)       = delete;
        CWorkspaceHistoryTracker(CWorkspaceHistoryTracker&&)      = delete;

        struct SHistoryEntry {
            PHLWORKSPACEREF           workspace;
            PHLMONITORREF             monitor;
            State::Workspace::STarget target;
        };

        const SHistoryEntry previousWorkspace(PHLWORKSPACE ws);

        const SHistoryEntry previousWorkspace(PHLWORKSPACE ws, PHLMONITOR restrict);
        void                workspaceIdentityChanged(PHLWORKSPACE workspace);
        void                track(PHLWORKSPACE workspace);

      private:
        std::deque<SHistoryEntry> m_history;

        void                      gc();
    };

    SP<CWorkspaceHistoryTracker> workspaceTracker();
};
