#pragma once

#include "S1.hpp"

namespace IPC::Socket1 {
    void        registerBuiltinCommands(CSocket1& socket);
    void        refreshState();
    std::string dispatchRequest(eOutputFormat format, std::string request);
}
