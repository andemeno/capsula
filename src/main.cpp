#include "capsula/client.hpp"
#include "capsula/protocol.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    // не завершаем процесс при попытке записи в закрытый сокет или pipe,
    // нужно корректно обрабатывать код EPIPE
    signal(SIGPIPE, SIG_IGN);

    // собираем переданные аргументы
    std::vector<std::string> args;
    for(int i = 1; i < argc; ++i)
        args.push_back(argv[i]);

    // проверяем корректность аргументов
    if(args.empty()) {
        capsula::print_help();
        return EXIT_SUCCESS;
    }
    const std::string command = args[0];
    if(command == "-h" || command == "--help" || command == "help") {
        capsula::print_help();
        return EXIT_SUCCESS;
    }
    if(command == "version" || command == "--version") {
        printf("capsula 0.1.0 (protocol %u)\n", static_cast<unsigned>(capsula::kProtoVersion));
        return EXIT_SUCCESS;
    }

    std::vector<std::string> rest(args.begin() + 1, args.end());
    return capsula::run_client_command(command, rest);
}
