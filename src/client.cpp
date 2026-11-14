#include "capsula/client.hpp"

#include "capsula/daemon.hpp"
#include "capsula/protocol.hpp"
#include "capsula/util.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace capsula {
    namespace {

        constexpr int kConnectTimeoutMs = 5000;

        void warn(const std::string& text) {
            fprintf(stderr, "capsula: %s\n", text.c_str());
        }

        int connect_daemon(std::string& err) {
            const std::string path = socket_path();
            int fd = connect_unix(path, err);
            if(fd >= 0)
                return fd;  // успешно подключились к демону

            if(getenv("CAPSULA_NO_AUTOSTART")) {
                // демон еще не запускали и автоматический запуск запрещен
                err = "capsula daemon is not running (" + err + ")";
                return -1;
            }

            // формируем команду на запуск демона, у нас уже был выполнен один fork,
            // поэтому в команде передаём флаг foreground, чтобы не делать повторную демонизацию
            std::string runtime = runtime_dir();
            if(!ensure_dir(runtime, 0700)) {
                err = "cannot create runtime directory " + runtime + ": " + errno_string();
                return -1;
            }
            std::string self = self_exe_path();
            if(self.empty()) {
                err = "cannot determine capsula executable path";
                return -1;
            }
            std::vector<std::string> argv = {
                self, "daemon", "--foreground", "--runtime", runtime, "--socket", path,
            };
            std::string log = join_path(runtime, "daemon.log");
            std::string spawn_err;
            if(!spawn_detached(argv, log, spawn_err)) {
                err = "cannot start capsula daemon: " + spawn_err;
                return -1;
            }
            // ждём запуска демона и снова пытаемся подключиться
            if(!wait_for_socket(path, kConnectTimeoutMs)) {
                err = "capsula daemon did not start; see " + log;
                return -1;
            }
            fd = connect_unix(path, err);
            if(fd < 0)
                err = "cannot connect to capsula daemon: " + err;
            return fd;
        }

        // запись в fd сообщения согласно протоколу обмена между клиентам и демоном
        bool send_message_fd(int fd, MsgType type, uint16_t flags, const std::vector<uint8_t>& payload) {
            std::vector<uint8_t> data = encode_message(type, flags, payload);
            return write_all(fd, data.data(), data.size());
        }

        bool read_message_blocking(int fd, MessageReader& reader, Message& msg) {
            for(;;) {
                if(reader.next(msg))
                    return true;
                if(reader.failed())
                    return false;
                char buf[8192];
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if(n > 0) {
                    reader.feed(buf, static_cast<size_t>(n));
                } else if(n == 0) {
                    return false;
                } else if(errno == EINTR) {
                    continue;
                } else {
                    return false;
                }
            }
        }

        class TerminalGuard {
           public:
            ~TerminalGuard() {
                restore();
            }

            bool make_raw() {
                if(m_active)
                    return true;
                if(!isatty(STDIN_FILENO))
                    return false;
                if(tcgetattr(STDIN_FILENO, &m_saved) < 0)
                    return false;
                struct termios raw = m_saved;
                cfmakeraw(&raw);
                raw.c_cc[VMIN] = 1;
                raw.c_cc[VTIME] = 0;
                // Keep bytes that were already queued before the attach handshake
                // completed; TCSAFLUSH would silently drop piped input.
                if(tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0)
                    return false;
                m_active = true;
                return true;
            }

            void restore() {
                if(m_active) {
                    tcsetattr(STDIN_FILENO, TCSAFLUSH, &m_saved);
                    m_active = false;
                }
            }

            bool active() const {
                return m_active;
            }

           private:
            struct termios m_saved;
            bool m_active = false;
        };

        class SignalMaskGuard {
           public:
            SignalMaskGuard() {
                sigemptyset(&m_mask);
                sigaddset(&m_mask, SIGINT);
                sigaddset(&m_mask, SIGTERM);
                sigaddset(&m_mask, SIGHUP);
                sigaddset(&m_mask, SIGWINCH);
                m_active = sigprocmask(SIG_BLOCK, &m_mask, &m_old) == 0;
            }
            ~SignalMaskGuard() {
                if(m_active)
                    sigprocmask(SIG_SETMASK, &m_old, nullptr);
            }
            const sigset_t& mask() const {
                return m_mask;
            }

           private:
            sigset_t m_mask;
            sigset_t m_old;
            bool m_active = false;
        };

        bool send_resize(int fd) {
            uint16_t cols = 80;
            uint16_t rows = 24;
            read_winsize(STDIN_FILENO, cols, rows);
            Writer w;
            w.u16(cols);
            w.u16(rows);
            std::vector<uint8_t> data = encode_message(MsgType::Resize, 0, w.data());
            return write_all(fd, data.data(), data.size());
        }

        bool send_detach(int fd) {
            std::vector<uint8_t> data = encode_message(MsgType::Detach, 0, {});
            return write_all(fd, data.data(), data.size());
        }

        // Waits for RunOk/AttachOk and then proxies output/input until Exit.
        int run_attach_session(int fd, bool requested_tty) {
            MessageReader reader;
            TerminalGuard terminal;
            SignalMaskGuard signals;
            int sigfd = signalfd(-1, &signals.mask(), SFD_NONBLOCK | SFD_CLOEXEC);
            // признак присоединения к терминалу контейнерного процесса
            bool attached = false;
            bool raw = false;
            bool server_tty = requested_tty;
            bool stdin_open = true;
            bool detached = false;
            int exit_code = 1;
            bool got_exit = false;

            while(!got_exit && !detached) {
                struct pollfd pfds[3];

                pfds[0].fd = fd;
                pfds[0].events = POLLIN;
                pfds[0].revents = 0;

                // не читать из stdin до завершения рукопожатия (attach handshake):
                // терминал сначала должен перейти в raw-режим, иначе нажатый заранее Ctrl-] уйдет
                // в контейнер как обычный ввод, а не отключит клиента
                pfds[1].fd = (attached && stdin_open) ? STDIN_FILENO : -1;
                pfds[1].events = POLLIN;
                pfds[1].revents = 0;

                pfds[2].fd = sigfd;
                pfds[2].events = POLLIN;
                pfds[2].revents = 0;

                int n = poll(pfds, sigfd >= 0 ? 3 : 2, -1);
                if(n < 0) {
                    if(errno == EINTR)
                        continue;
                    warn("poll failed: " + errno_string());
                    break;
                }

                if(pfds[2].fd >= 0 && (pfds[2].revents & POLLIN)) {
                    struct signalfd_siginfo si;
                    while(read(sigfd, &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si))) {
                        if(si.ssi_signo == SIGWINCH) {
                            if(attached)
                                send_resize(fd);
                        } else if(si.ssi_signo == SIGINT || si.ssi_signo == SIGTERM || si.ssi_signo == SIGHUP) {
                            if(attached)
                                send_detach(fd);
                            exit_code = 128 + static_cast<int>(si.ssi_signo);
                            got_exit = true;
                            break;
                        }
                    }
                    if(got_exit)
                        break;
                }

                if(pfds[1].fd >= 0 && (pfds[1].revents & (POLLIN | POLLHUP))) {
                    char buf[8192];
                    ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
                    if(r > 0) {
                        const uint8_t* start = reinterpret_cast<const uint8_t*>(buf);
                        size_t len = static_cast<size_t>(r);
                        bool send_all = true;
                        if(raw) {
                            const uint8_t* quit = static_cast<const uint8_t*>(memchr(buf, 0x1d, len));
                            if(quit) {
                                size_t before = static_cast<size_t>(quit - start);
                                if(before > 0)
                                    send_message_fd(fd, MsgType::Input, 0, std::vector<uint8_t>(start, start + before));
                                send_detach(fd);
                                detached = true;
                                send_all = false;
                            }
                        }
                        if(send_all &&
                           !send_message_fd(fd, MsgType::Input, 0, std::vector<uint8_t>(start, start + len))) {
                            warn("connection to daemon lost");
                            break;
                        }
                    } else if(r == 0) {
                        send_message_fd(fd, MsgType::InputEof, 0, {});
                        stdin_open = false;
                    } else if(errno != EINTR && errno != EAGAIN) {
                        stdin_open = false;
                    }
                }

                // собыие на сокете
                if(pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                    char buf[16384];
                    ssize_t r = recv(fd, buf, sizeof(buf), 0);
                    if(r > 0) {
                        reader.feed(buf, static_cast<size_t>(r));
                    } else if(r == 0) {
                        if(!got_exit)
                            warn("daemon closed the connection");
                        break;
                    } else if(errno != EINTR && errno != EAGAIN) {
                        warn("connection to daemon lost");
                        break;
                    }

                    Message m;
                    while(reader.next(m)) {
                        MsgType type = static_cast<MsgType>(m.type);
                        if(type == MsgType::RunOk || type == MsgType::AttachOk) {
                            Reader rr(m.payload);
                            std::string id = rr.str();
                            if(type == MsgType::RunOk) {
                                std::string name = rr.str();
                                int32_t pid = rr.i32();
                                server_tty = rr.u8() != 0;
                                (void)name;
                                (void)pid;
                            } else {
                                int32_t pid = rr.i32();
                                server_tty = rr.u8() != 0;
                                (void)pid;
                            }

                            // присоединились к терминалу контейнерного процесса
                            attached = true;

                            if(server_tty && !raw) {
                                if(terminal.make_raw())
                                    raw = true;
                            }
                            send_resize(fd);

                        } else if(type == MsgType::Output) {
                            int outfd = (m.flags == kStreamStderr) ? STDERR_FILENO : STDOUT_FILENO;
                            if(!m.payload.empty() && !write_all(outfd, m.payload.data(), m.payload.size())) {
                                got_exit = true;
                                exit_code = 1;
                                break;
                            }

                        } else if(type == MsgType::Exit) {
                            int32_t code = 0;
                            int32_t term = 0;
                            decode_exit(m, code, term);
                            exit_code = code;
                            got_exit = true;
                            break;

                        } else if(type == MsgType::Error) {
                            uint32_t code = 0;
                            std::string text;
                            decode_error(m, code, text);
                            warn(text);
                            exit_code = 1;
                            got_exit = true;
                            break;
                        }
                    }

                    if(reader.failed()) {
                        warn("invalid protocol data from daemon");
                        break;
                    }
                }
            }

            if(sigfd >= 0)
                close(sigfd);
            terminal.restore();
            if(detached)
                return 0;
            if(!got_exit && exit_code == 1)
                return 1;
            return exit_code & 0xff;
        }

        struct RunOptions {
            std::string name;
            std::string rootfs;
            std::string hostname;
            std::string cwd = "/";
            std::vector<std::string> env;
            std::vector<std::string> command;
            bool tty = false;
            bool detach = false;
        };

        bool parse_run(const std::vector<std::string>& args, RunOptions& opts) {
            size_t i = 0;
            for(; i < args.size(); ++i) {
                const std::string& a = args[i];
                if(a == "--") {
                    ++i;
                    break;
                }
                if(a.empty() || a[0] != '-' || a == "-") {
                    break;
                }
                if(a == "--help" || a == "-h") {
                    printf(
                        "usage: capsula run [options] [--] COMMAND [ARGS...]\n"
                        "  -d, --detach          start in background\n"
                        "  -t, --tty             allocate a pseudo-terminal\n"
                        "      --name NAME       container name\n"
                        "      --rootfs PATH     root filesystem (required)\n"
                        "      --hostname NAME   UTS hostname\n"
                        "      --cwd PATH        working directory (default /)\n"
                        "      --env KEY=VALUE   set an environment variable\n");
                    return false;
                }
                auto value = [&](const std::string& opt, std::string& out) {
                    size_t eq = a.find('=');
                    if(eq != std::string::npos) {
                        out = a.substr(eq + 1);
                        return true;
                    }
                    if(i + 1 >= args.size()) {
                        warn("option " + opt + " requires an argument");
                        return false;
                    }
                    out = args[++i];
                    return true;
                };
                if(a == "-d" || a == "--detach") {
                    opts.detach = true;
                } else if(a == "-t" || a == "--tty") {
                    opts.tty = true;
                } else if(a.rfind("--name", 0) == 0 && (a.size() == 6 || a[6] == '=')) {
                    if(!value("--name", opts.name))
                        return false;
                } else if(a.rfind("--rootfs", 0) == 0 && (a.size() == 8 || a[8] == '=')) {
                    if(!value("--rootfs", opts.rootfs))
                        return false;
                } else if(a.rfind("--hostname", 0) == 0 && (a.size() == 10 || a[10] == '=')) {
                    if(!value("--hostname", opts.hostname))
                        return false;
                } else if(a.rfind("--cwd", 0) == 0 && (a.size() == 5 || a[5] == '=')) {
                    if(!value("--cwd", opts.cwd))
                        return false;
                } else if(a.rfind("--env", 0) == 0 && (a.size() == 5 || a[5] == '=')) {
                    std::string e;
                    if(!value("--env", e))
                        return false;
                    if(e.find('=') == std::string::npos) {
                        warn("--env expects KEY=VALUE");
                        return false;
                    }
                    opts.env.push_back(e);
                } else if(a.size() > 2 && a[0] == '-' && a[1] != '-') {
                    for(size_t k = 1; k < a.size(); ++k) {
                        if(a[k] == 'd')
                            opts.detach = true;
                        else if(a[k] == 't')
                            opts.tty = true;
                        else if(a[k] == 'i') {
                        } else {
                            warn("unknown option -" + std::string(1, a[k]));
                            return false;
                        }
                    }
                } else {
                    warn("unknown option " + a);
                    return false;
                }
            }
            opts.command.assign(args.begin() + static_cast<std::ptrdiff_t>(i), args.end());
            if(opts.command.empty()) {
                warn("run: no command specified");
                return false;
            }
            if(opts.rootfs.empty()) {
                warn("run: --rootfs is required");
                return false;
            }
            return true;
        }

        std::vector<std::string> build_environment(const RunOptions& opts) {
            std::vector<std::string> env = current_env();
            for(const auto& e : opts.env) {
                size_t eq = e.find('=');
                env_set(env, e.substr(0, eq), e.substr(eq + 1));
            }
            env_set(env, "PWD", opts.cwd);
            std::string tmp;
            if(!env_lookup(env, "HOME", tmp))
                env_set(env, "HOME", "/root");
            if(!env_lookup(env, "PATH", tmp))
                env_set(env, "PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
            if(opts.tty && !env_lookup(env, "TERM", tmp))
                env_set(env, "TERM", "xterm-256color");
            if(!opts.hostname.empty())
                env_set(env, "HOSTNAME", opts.hostname);
            env_unset(env, "CAPSULA_SOCKET");
            env_unset(env, "CAPSULA_RUNTIME_DIR");
            return env;
        }

        int command_run(const std::vector<std::string>& args) {
            RunOptions opts;
            if(!parse_run(args, opts))
                return 2;
            std::string rootfs = abs_path(opts.rootfs);
            if(!is_dir(rootfs)) {
                warn("rootfs is not a directory: " + opts.rootfs);
                return 2;
            }

            RunSpec spec;
            spec.name = opts.name;
            spec.rootfs = rootfs;
            spec.hostname = opts.hostname;
            spec.cwd = opts.cwd;
            spec.argv = opts.command;
            spec.env = build_environment(opts);
            spec.tty = opts.tty;
            spec.detach = opts.detach;
            if(isatty(STDIN_FILENO))
                read_winsize(STDIN_FILENO, spec.cols, spec.rows);

            std::string err;
            int fd = connect_daemon(err);
            if(fd < 0) {
                warn(err);
                return 1;
            }
            std::vector<uint8_t> request = encode_run(spec);
            if(!write_all(fd, request.data(), request.size())) {
                close(fd);
                warn("cannot send run request");
                return 1;
            }

            if(opts.detach) {
                set_socket_timeout(fd, 30);
                MessageReader reader;
                Message m;
                if(!read_message_blocking(fd, reader, m)) {
                    close(fd);
                    warn("daemon closed the connection");
                    return 1;
                }
                if(static_cast<MsgType>(m.type) == MsgType::RunOk) {
                    Reader r(m.payload);
                    std::string id = r.str();
                    std::string name = r.str();
                    int32_t pid = r.i32();
                    (void)name;
                    (void)pid;
                    printf("%s\n", id.c_str());
                    close(fd);
                    return 0;
                }
                if(static_cast<MsgType>(m.type) == MsgType::Error) {
                    uint32_t code = 0;
                    std::string text;
                    decode_error(m, code, text);
                    warn(text);
                } else {
                    warn("unexpected response from daemon");
                }
                close(fd);
                return 1;
            }

            int rc = run_attach_session(fd, spec.tty);
            close(fd);
            return rc;
        }

        int command_attach(const std::vector<std::string>& args) {
            std::string target;
            for(const auto& a : args) {
                if(a == "-h" || a == "--help") {
                    printf("usage: capsula attach CONTAINER\n");
                    return 0;
                }
                if(!target.empty()) {
                    warn("attach: too many arguments");
                    return 2;
                }
                target = a;
            }
            if(target.empty()) {
                warn("attach: container id, name or pid is required");
                return 2;
            }
            uint16_t cols = 80;
            uint16_t rows = 24;
            read_winsize(STDIN_FILENO, cols, rows);
            std::string err;
            int fd = connect_daemon(err);
            if(fd < 0) {
                warn(err);
                return 1;
            }
            std::vector<uint8_t> request = encode_attach_request(target, cols, rows);
            if(!write_all(fd, request.data(), request.size())) {
                close(fd);
                warn("cannot send attach request");
                return 1;
            }
            int rc = run_attach_session(fd, isatty(STDIN_FILENO));
            close(fd);
            return rc;
        }

        int command_logs(const std::vector<std::string>& args) {
            bool follow = false;
            std::string target;
            for(const auto& a : args) {
                if(a == "-f" || a == "--follow")
                    follow = true;
                else if(a == "-h" || a == "--help") {
                    printf("usage: capsula logs [-f] CONTAINER\n");
                    return 0;
                } else if(!target.empty()) {
                    warn("logs: too many arguments");
                    return 2;
                } else {
                    target = a;
                }
            }
            if(target.empty()) {
                warn("logs: container id, name or pid is required");
                return 2;
            }
            std::string err;
            int fd = connect_daemon(err);
            if(fd < 0) {
                warn(err);
                return 1;
            }
            std::vector<uint8_t> req = encode_logs_request(target, follow);
            if(!write_all(fd, req.data(), req.size())) {
                close(fd);
                warn("cannot send logs request");
                return 1;
            }
            MessageReader reader;
            Message m;
            int rc = 0;
            for(;;) {
                if(!read_message_blocking(fd, reader, m)) {
                    warn("daemon closed the connection");
                    rc = 1;
                    break;
                }
                MsgType type = static_cast<MsgType>(m.type);
                if(type == MsgType::LogData) {
                    if(!m.payload.empty() && !write_all(STDOUT_FILENO, m.payload.data(), m.payload.size())) {
                        rc = 1;
                        break;
                    }
                } else if(type == MsgType::LogEnd) {
                    break;
                } else if(type == MsgType::Error) {
                    uint32_t code = 0;
                    std::string text;
                    decode_error(m, code, text);
                    warn(text);
                    rc = 1;
                    break;
                }
            }
            close(fd);
            return rc;
        }

        std::string command_of(const ContainerInfo& info) {
            std::string cmd = info.command;
            if(cmd.size() > 60)
                cmd = cmd.substr(0, 57) + "...";
            return cmd;
        }

        int command_list(const std::vector<std::string>& args) {
            bool running_only = false;
            for(const auto& a : args) {
                if(a == "-r" || a == "--running")
                    running_only = true;
                else if(a == "-h" || a == "--help") {
                    printf("usage: capsula list [--running]\n");
                    return 0;
                } else {
                    warn("list: unknown argument " + a);
                    return 2;
                }
            }
            std::string err;
            int fd = connect_daemon(err);
            if(fd < 0) {
                warn(err);
                return 1;
            }
            set_socket_timeout(fd, 30);
            if(!send_message_fd(fd, MsgType::List, 0, {})) {
                close(fd);
                warn("cannot send list request");
                return 1;
            }
            std::vector<ContainerInfo> items;
            MessageReader reader;
            Message m;
            for(;;) {
                if(!read_message_blocking(fd, reader, m)) {
                    close(fd);
                    warn("daemon closed the connection");
                    return 1;
                }
                MsgType type = static_cast<MsgType>(m.type);
                if(type == MsgType::ListItem) {
                    ContainerInfo info;
                    if(!decode_info(m, info)) {
                        close(fd);
                        warn("invalid list response");
                        return 1;
                    }
                    items.push_back(std::move(info));
                } else if(type == MsgType::ListEnd) {
                    break;
                } else if(type == MsgType::Error) {
                    uint32_t code = 0;
                    std::string text;
                    decode_error(m, code, text);
                    close(fd);
                    warn(text);
                    return 1;
                }
            }
            close(fd);

            printf("%-10s %-7s %-9s %-5s %-3s %-14s %s\n", "ID", "PID", "STATUS", "EXIT", "TTY", "NAME", "COMMAND");
            for(const auto& info : items) {
                if(running_only && info.state == ContainerState::Exited)
                    continue;
                char exitbuf[32];
                if(info.state == ContainerState::Exited)
                    snprintf(exitbuf, sizeof(exitbuf), "%d", info.exit_code);
                else
                    snprintf(exitbuf, sizeof(exitbuf), "-");
                printf("%-10s %-7d %-9s %-5s %-3s %-14s %s\n", info.id.c_str(), static_cast<int>(info.pid),
                       state_name(info.state).c_str(), exitbuf, info.tty ? "yes" : "no",
                       info.name.empty() ? "-" : info.name.c_str(), command_of(info).c_str());
            }
            return 0;
        }

        int command_kill(const std::vector<std::string>& args) {
            int sig = SIGTERM;
            std::string target;
            for(size_t i = 0; i < args.size(); ++i) {
                const std::string& a = args[i];
                if(a == "-h" || a == "--help") {
                    printf("usage: capsula kill [-s SIGNAL] CONTAINER|PID\n");
                    return 0;
                }
                if(a == "-s" || a == "--signal") {
                    if(i + 1 >= args.size()) {
                        warn("kill: signal is required");
                        return 2;
                    }
                    if(!parse_signal(args[++i], sig)) {
                        warn("kill: invalid signal " + args[i]);
                        return 2;
                    }
                } else if(a.rfind("--signal=", 0) == 0) {
                    if(!parse_signal(a.substr(9), sig)) {
                        warn("kill: invalid signal " + a.substr(9));
                        return 2;
                    }
                } else if(a.rfind("-s", 0) == 0 && a.size() > 2) {
                    if(!parse_signal(a.substr(2), sig)) {
                        warn("kill: invalid signal " + a.substr(2));
                        return 2;
                    }
                } else if(!target.empty()) {
                    warn("kill: too many arguments");
                    return 2;
                } else {
                    target = a;
                }
            }
            if(target.empty()) {
                warn("kill: container id, name or pid is required");
                return 2;
            }
            std::string err;
            int fd = connect_daemon(err);
            if(fd < 0) {
                warn(err);
                return 1;
            }
            set_socket_timeout(fd, 30);
            std::vector<uint8_t> req = encode_kill_request(target, sig, false);
            if(!write_all(fd, req.data(), req.size())) {
                close(fd);
                warn("cannot send kill request");
                return 1;
            }
            MessageReader reader;
            Message m;
            if(!read_message_blocking(fd, reader, m)) {
                close(fd);
                warn("daemon closed the connection");
                return 1;
            }
            close(fd);
            if(static_cast<MsgType>(m.type) == MsgType::Error) {
                uint32_t code = 0;
                std::string text;
                decode_error(m, code, text);
                warn(text);
                return 1;
            }
            if(static_cast<MsgType>(m.type) != MsgType::KillOk) {
                warn("unexpected response from daemon");
                return 1;
            }
            Reader r(m.payload);
            std::string id = r.str();
            int32_t pid = r.i32();
            bool delivered = r.u8() != 0;
            int32_t exit_code = r.i32();
            if(delivered) {
                printf("signaled %s (pid %d) with SIG%s\n", id.c_str(), static_cast<int>(pid),
                       signal_name(sig).c_str());
                return 0;
            }
            printf("container %s (pid %d) is not running (exit %d)\n", id.c_str(), static_cast<int>(pid),
                   static_cast<int>(exit_code));
            return 0;
        }

        int command_daemon(const std::vector<std::string>& args) {
            DaemonOptions opts;
            bool stop = false;

            // парсим опции команды
            for(size_t i = 0; i < args.size(); ++i) {
                const std::string& a = args[i];
                auto value = [&](std::string& out) {
                    if(i + 1 >= args.size()) {
                        warn("daemon: option requires a value");
                        return false;
                    }
                    out = args[++i];
                    return true;
                };

                if(a == "-h" || a == "--help") {
                    printf(
                        "usage: capsula daemon [--foreground] [--socket PATH] [--runtime DIR] [--log FILE]\n"
                        "       capsula daemon --stop\n");
                    return 0;

                } else if(a == "--foreground") {
                    opts.foreground = true;

                } else if(a == "--stop") {
                    stop = true;

                } else if(a == "--socket") {
                    if(!value(opts.socket))
                        return 2;
                } else if(a.rfind("--socket=", 0) == 0) {
                    opts.socket = a.substr(9);

                } else if(a == "--runtime") {
                    if(!value(opts.runtime))
                        return 2;
                } else if(a.rfind("--runtime=", 0) == 0) {
                    opts.runtime = a.substr(10);

                } else if(a == "--log") {
                    if(!value(opts.log))
                        return 2;
                } else if(a.rfind("--log=", 0) == 0) {
                    opts.log = a.substr(6);

                } else {
                    warn("daemon: unknown argument " + a);
                    return 2;
                }
            }

            // выполняем команду

            // останов демона
            if(stop) {
                std::string runtime = opts.runtime.empty() ? runtime_dir() : abs_path(opts.runtime);
                std::string pidpath = join_path(runtime, "daemon.pid");
                std::string text;
                long pid = 0;
                bool killed = false;
                if(read_file(pidpath, text)) {
                    std::string trimmed = text;
                    while(!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r'))
                        trimmed.pop_back();
                    if(parse_long(trimmed, pid, 1, INT_MAX)) {
                        if(kill(static_cast<pid_t>(pid), SIGTERM) == 0) {  // шлём сигнал
                            killed = true;
                            // ждём, когда процесс завешится
                            for(int i = 0; i < 100; ++i) {
                                if(kill(static_cast<pid_t>(pid), 0) < 0 && errno == ESRCH)
                                    break;
                                struct timespec ts;
                                ts.tv_sec = 0;
                                ts.tv_nsec = 30 * 1000 * 1000;
                                nanosleep(&ts, nullptr);
                            }
                        }
                    }
                }
                if(!killed) {
                    std::string err;
                    std::string path = opts.socket.empty() ? socket_path() : abs_path(opts.socket);
                    int fd = connect_unix(path, err);
                    if(fd >= 0) {
                        set_socket_timeout(fd, 10);
                        send_message_fd(fd, MsgType::Shutdown, 0, {});
                        close(fd);
                        killed = true;
                    }
                }
                if(killed) {
                    printf("capsula daemon stopped\n");
                    return 0;
                }
                warn("capsula daemon is not running");
                return 1;
            }

            // запуск демона
            return daemon_main(opts);
        }

    }  // namespace

    int run_client_command(const std::string& command, const std::vector<std::string>& args) {
        if(command == "run")
            return command_run(args);
        if(command == "list" || command == "ps")
            return command_list(args);
        if(command == "logs")
            return command_logs(args);
        if(command == "attach")
            return command_attach(args);
        if(command == "kill")
            return command_kill(args);
        if(command == "daemon")
            return command_daemon(args);
        warn("unknown command: " + command);
        print_help();
        return 2;
    }

    void print_help() {
        printf(
            "capsula - a small namespace container runtime\n\n"
            "usage: capsula COMMAND [ARGS...]\n\n"
            "commands:\n"
            "  run       run a command in a new container\n"
            "  list      list known containers (alias: ps)\n"
            "  logs      show container logs (-f to follow)\n"
            "  attach    attach to a running container\n"
            "  kill      send a signal to a container\n"
            "  daemon    run or stop the daemon\n"
            "  version   print version\n\n"
            "environment:\n"
            "  CAPSULA_NO_AUTOSTART  clients do not start the daemon implicitly\n"
            "  CAPSULA_RUNTIME_DIR   runtime/state directory\n"
            "  CAPSULA_SOCKET        daemon socket path\n");
    }

}  // namespace capsula
