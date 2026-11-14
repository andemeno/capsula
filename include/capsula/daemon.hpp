#pragma once

#include <string>

namespace capsula {

    struct DaemonOptions {
        std::string runtime;
        std::string socket;
        std::string log;
        bool foreground = false;
    };

    // Returns a process exit code.
    int daemon_main(const DaemonOptions& options);

}  // namespace capsula
