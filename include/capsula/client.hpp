#pragma once

#include <string>
#include <vector>

namespace capsula {

    int run_client_command(const std::string& command, const std::vector<std::string>& args);
    void print_help();

}  // namespace capsula
