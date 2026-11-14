#pragma once

#include <sys/types.h>
#include <cstdint>
#include <string>
#include <vector>

namespace capsula {

    std::string errno_string(int err);
    std::string errno_string();

    bool set_cloexec(int fd);  // FIXME: no used
    bool set_nonblocking(int fd, bool enabled);
    bool write_all(int fd, const void* data, size_t len);
    bool write_all(int fd, const std::string& s);
    bool write_file(const std::string& path, const std::string& content, int mode = 0600);
    bool read_file(const std::string& path, std::string& out);

    std::string self_exe_path();
    std::string runtime_dir();
    std::string socket_path();
    std::string abs_path(const std::string& path);
    std::string join_path(const std::string& a, const std::string& b);
    std::string base_name(const std::string& path);

    bool ensure_dir(const std::string& path, int mode = 0700);
    bool is_dir(const std::string& path);

    uint64_t now_ms();
    uint64_t random_u64();
    std::string random_hex(size_t chars);

    bool parse_long(const std::string& s, long& out, long min, long max);
    bool parse_signal(const std::string& s, int& sig);
    std::string signal_name(int sig);

    std::vector<std::string> current_env();
    bool env_lookup(const std::vector<std::string>& env, const std::string& key, std::string& value);
    void env_set(std::vector<std::string>& env, const std::string& key, const std::string& value);
    void env_unset(std::vector<std::string>& env, const std::string& key);

    std::string join_words(const std::vector<std::string>& words);
    std::string shell_quote(const std::string& s);

    int connect_unix(const std::string& path, std::string& err);
    int listen_unix(const std::string& path, std::string& err);
    bool set_socket_timeout(int fd, int seconds);

    bool read_winsize(int fd, uint16_t& cols, uint16_t& rows);
    bool set_winsize(int fd, uint16_t cols, uint16_t rows);

    bool daemonize(const std::string& log_path);
    bool spawn_detached(const std::vector<std::string>& argv, const std::string& log_path, std::string& err);
    bool wait_for_socket(const std::string& path, int timeout_ms);

    bool kill_process(pid_t pid, int sig);

}  // namespace capsula
