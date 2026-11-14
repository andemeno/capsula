#pragma once

#include <string>
#include <vector>

namespace capsula {

    struct ChildConfig {
        int sync_fd = -1;    // parent -> child "go" pipe (read side)
        int status_fd = -1;  // child -> parent startup status (CLOEXEC on exec)
        int pid_fd = -1;     // launcher -> daemon: host pid of container init
        int master_fd = -1;  // PTY master inherited from daemon, closed by child
        std::string slave_path;
        int stdin_read = -1;    // non-tty stdin pipe read side
        int stdout_write = -1;  // non-tty stdout pipe write side
        int stderr_write = -1;  // non-tty stderr pipe write side
        bool tty = false;
        std::string rootfs;
        std::string mountpoint;
        std::string hostname;
        std::string cwd = "/";
        std::vector<std::string> argv;
        std::vector<std::string> env;
    };

    // Entry point passed to clone(CLONE_NEWUSER|SIGCHLD).  The launcher maps the
    // user namespace, creates the remaining namespaces and forks the real
    // container init (PID 1 in the new PID namespace).
    int container_launcher(void* arg);

}  // namespace capsula
