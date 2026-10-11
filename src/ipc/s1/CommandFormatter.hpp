#pragma once

#include "S1.hpp"
#include "../../desktop/DesktopTypes.hpp"

class CCommandFormatter {
  public:
    static std::string getWindowData(PHLWINDOW window, IPC::Socket1::eOutputFormat format);
    static std::string getWorkspaceData(PHLWORKSPACE workspace, IPC::Socket1::eOutputFormat format);
    static std::string getSolitaryBlockedReason(PHLMONITOR monitor, IPC::Socket1::eOutputFormat format);
    static std::string getDSBlockedReason(PHLMONITOR monitor, IPC::Socket1::eOutputFormat format);
    static std::string getTearingBlockedReason(PHLMONITOR monitor, IPC::Socket1::eOutputFormat format);
    static std::string getMonitorData(PHLMONITOR monitor, IPC::Socket1::eOutputFormat format);
};
