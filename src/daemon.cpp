#include "capsula/daemon.hpp"

#include "capsula/child.hpp"
#include "capsula/protocol.hpp"
#include "capsula/util.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <utility>

namespace capsula {
    namespace {

        constexpr size_t kMaxOutputQueue = 64u * 1024u * 1024u;
        constexpr size_t kMaxInputQueue = 4u * 1024u * 1024u;
        constexpr int kKillGraceMs = 5000;

        bool write_proc_file(const std::string& path, const std::string& value) {
            int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
            if(fd < 0)
                return false;
            bool ok = write_all(fd, value);
            close(fd);
            return ok;
        }

        bool setup_userns_maps(pid_t pid, std::string& err) {
            char path[128];
            snprintf(path, sizeof(path), "/proc/%d/setgroups", static_cast<int>(pid));
            if(!write_proc_file(path, "deny\n") && errno != ENOENT && errno != EPERM) {
                err = "cannot write setgroups: " + errno_string();
                return false;
            }

            char buf[128];
            snprintf(path, sizeof(path), "/proc/%d/gid_map", static_cast<int>(pid));
            snprintf(buf, sizeof(buf), "0 %u 1\n", static_cast<unsigned>(getegid()));
            if(!write_proc_file(path, buf)) {
                err = "cannot write gid_map: " + errno_string();
                return false;
            }

            snprintf(path, sizeof(path), "/proc/%d/uid_map", static_cast<int>(pid));
            snprintf(buf, sizeof(buf), "0 %u 1\n", static_cast<unsigned>(geteuid()));
            if(!write_proc_file(path, buf)) {
                err = "cannot write uid_map: " + errno_string();
                return false;
            }
            return true;
        }

        bool valid_name(const std::string& name) {
            if(name.empty() || name.size() > 64)
                return false;
            for(char c : name) {
                if(!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.'))
                    return false;
            }
            return true;
        }

        bool all_digits(const std::string& s) {
            if(s.empty())
                return false;
            for(char c : s) {
                if(c < '0' || c > '9')
                    return false;
            }
            return true;
        }

        int decode_wait_status(int status, int& term_sig) {
            term_sig = 0;
            if(WIFEXITED(status))
                return WEXITSTATUS(status);
            if(WIFSIGNALED(status)) {
                term_sig = WTERMSIG(status);
                return 128 + term_sig;
            }
            return 255;
        }

        std::string describe_status(const std::string& raw) {
            // Child failure pipe format: stage:errno:message
            size_t a = raw.find(':');
            if(a == std::string::npos)
                return raw;
            size_t b = raw.find(':', a + 1);
            if(b == std::string::npos)
                return raw;
            std::string stage = raw.substr(0, a);
            std::string message = raw.substr(b + 1);
            return "capsula: " + stage + ": " + message;
        }

        struct Client {
            int fd = -1;
            MessageReader reader;
            std::vector<uint8_t> out;
            size_t out_off = 0;

            enum class Mode { Control, Attached, Logs };
            Mode mode = Mode::Control;
            std::string target;
            bool follow = false;
        };

        enum class FdRole { Master, Stdout, Stderr, Stdin, Status };

        struct Container {
            std::string id;
            std::string name;
            std::string rootfs;
            std::string mountpoint;
            std::string hostname;
            std::string cwd;
            std::string command;
            std::string log_path;
            std::vector<std::string> argv;
            std::vector<std::string> env;

            bool tty = false;
            bool detach = false;
            pid_t pid = -1;
            pid_t launcher_pid = -1;
            bool launcher_reaped = false;
            ContainerState state = ContainerState::Starting;
            int32_t exit_code = -1;
            int32_t term_signal = 0;
            uint64_t created_ms = 0;

            int master_fd = -1;
            int stdout_fd = -1;
            int stderr_fd = -1;
            int status_fd = -1;
            int input_fd = -1;

            bool master_eof = false;
            bool stdout_eof = false;
            bool stderr_eof = false;
            bool status_eof = false;
            bool child_reaped = false;
            bool exit_sent = false;

            int log_fd = -1;
            std::string status_buf;
            std::string startup_error;

            std::vector<uint8_t> input_buf;
            size_t input_off = 0;
            bool input_close_after = false;

            int64_t kill_deadline_ms = -1;
            bool kill_pending = false;
        };

        class Daemon {
           public:
            explicit Daemon(DaemonOptions opts) : m_opts(std::move(opts)) {
            }
            ~Daemon();

            // 0: ok, 1: error, 2: another instance owns the lock
            int run();

           private:
            bool setup(std::string& err);
            void cleanup();
            void loop();
            int compute_timeout() const;
            void check_kill_deadlines();

            void handle_accept();
            void handle_signal();
            void handle_client(Client& c, short revents);
            bool process_messages(Client& c);
            bool dispatch(Client& c, const Message& m);
            void handle_run(Client& c, const Message& m);
            void handle_list(Client& c);
            void handle_logs(Client& c, const Message& m);
            void handle_attach(Client& c, const Message& m);
            void handle_kill(Client& c, const Message& m);

            void handle_container_fd(Container& ct, FdRole role, short revents);
            void read_output(Container& ct, FdRole role);
            void read_status(Container& ct);
            void flush_input(Container& ct);
            void maybe_finalize(Container& ct);
            void close_output_fd(Container& ct, FdRole role);
            void reap_children();

            Container* find_container(const std::string& target);
            Container* find_container_by_id(const std::string& id);
            std::string unique_id();
            std::string unique_mountpoint(const std::string& id);
            std::string make_log_dir(const std::string& id);

            void queue_bytes(Client& c, const std::vector<uint8_t>& data);
            void send_msg(Client& c, MsgType type, uint16_t flags, const std::vector<uint8_t>& payload);
            void send_error(Client& c, uint32_t code, const std::string& text);
            void close_client(Client& c);
            void broadcast_output(Container& ct, uint16_t stream, const std::vector<uint8_t>& data);
            void broadcast_log(Container& ct, const std::vector<uint8_t>& data);
            void send_exit_to_attached(Container& ct);
            bool queue_input(Container& ct, const void* data, size_t len);

            DaemonOptions m_opts;
            std::string m_runtime;
            std::string m_socket;
            std::string m_pid_path;
            std::string m_lock_path;
            int m_listen_fd = -1;
            int m_sig_fd = -1;
            int m_lock_fd = -1;
            bool m_shutdown = false;
            bool m_already_running = false;
            bool m_owns_lock = false;
            std::vector<std::unique_ptr<Client>> m_clients;
            std::vector<std::unique_ptr<Container>> m_containers;
        };

        bool Daemon::setup(std::string& err) {
            m_runtime = m_opts.runtime.empty() ? runtime_dir() : abs_path(m_opts.runtime);
            m_socket = m_opts.socket.empty() ? socket_path() : abs_path(m_opts.socket);

            struct stat st;
            if(lstat(m_runtime.c_str(), &st) == 0) {
                if(!S_ISDIR(st.st_mode)) {
                    err = "runtime path is not a directory: " + m_runtime;
                    return false;
                }
                if(st.st_uid != geteuid()) {
                    err = "runtime directory is owned by another user: " + m_runtime;
                    return false;
                }
                chmod(m_runtime.c_str(), 0700);
            } else if(errno == ENOENT) {
                if(!ensure_dir(m_runtime, 0700)) {
                    err = "cannot create runtime directory " + m_runtime + ": " + errno_string();
                    return false;
                }
            } else {
                err = "cannot stat runtime directory: " + errno_string();
                return false;
            }

            size_t slash = m_socket.find_last_of('/');
            std::string sockdir = slash == std::string::npos ? std::string(".") : m_socket.substr(0, slash);
            if(!sockdir.empty() && !ensure_dir(sockdir, 0700)) {
                err = "cannot create socket directory " + sockdir + ": " + errno_string();
                return false;
            }

            m_lock_path = join_path(m_runtime, "daemon.lock");
            m_lock_fd = open(m_lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
            if(m_lock_fd < 0) {
                err = "cannot open lock file: " + errno_string();
                return false;
            }
            if(flock(m_lock_fd, LOCK_EX | LOCK_NB) < 0) {
                if(errno == EWOULDBLOCK) {
                    m_already_running = true;
                    err = "another capsula daemon is already running";
                } else {
                    err = "cannot lock daemon.lock: " + errno_string();
                }
                return false;
            }
            m_owns_lock = true;

            if(!ensure_dir(join_path(m_runtime, "containers"), 0700) ||
               !ensure_dir(join_path(m_runtime, "roots"), 0700)) {
                err = "cannot create daemon directories: " + errno_string();
                return false;
            }

            m_listen_fd = listen_unix(m_socket, err);
            if(m_listen_fd < 0)
                return false;
            if(!set_nonblocking(m_listen_fd, true)) {
                err = "cannot set listener non-blocking: " + errno_string();
                return false;
            }

            m_pid_path = join_path(m_runtime, "daemon.pid");
            char pidbuf[32];
            snprintf(pidbuf, sizeof(pidbuf), "%d\n", static_cast<int>(getpid()));
            if(!write_file(m_pid_path, pidbuf, 0600)) {
                err = "cannot write pid file: " + errno_string();
                return false;
            }

            signal(SIGPIPE, SIG_IGN);
            if(prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
                err = "cannot become child subreaper: " + errno_string();
                return false;
            }

            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGCHLD);
            sigaddset(&mask, SIGTERM);
            sigaddset(&mask, SIGINT);
            sigaddset(&mask, SIGHUP);
            sigaddset(&mask, SIGQUIT);
            if(sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
                err = "cannot block signals: " + errno_string();
                return false;
            }
            m_sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
            if(m_sig_fd < 0) {
                err = "cannot create signalfd: " + errno_string();
                return false;
            }
            return true;
        }

        void Daemon::cleanup() {
            for(auto& ct : m_containers) {
                if(ct->pid > 0 && !ct->child_reaped && ct->state != ContainerState::Exited)
                    kill(ct->pid, SIGKILL);
                if(ct->launcher_pid > 0 && !ct->launcher_reaped && ct->launcher_pid != ct->pid)
                    kill(ct->launcher_pid, SIGKILL);
                const int old_master = ct->master_fd;
                if(ct->master_fd >= 0) {
                    close(ct->master_fd);
                    ct->master_fd = -1;
                }
                if(ct->stdout_fd >= 0) {
                    close(ct->stdout_fd);
                    ct->stdout_fd = -1;
                }
                if(ct->stderr_fd >= 0) {
                    close(ct->stderr_fd);
                    ct->stderr_fd = -1;
                }
                if(ct->status_fd >= 0) {
                    close(ct->status_fd);
                    ct->status_fd = -1;
                }
                if(ct->input_fd >= 0 && ct->input_fd != old_master)
                    close(ct->input_fd);
                ct->input_fd = -1;
                if(ct->log_fd >= 0) {
                    close(ct->log_fd);
                    ct->log_fd = -1;
                }
            }
            for(int i = 0; i < 100; ++i) {
                bool any = false;
                for(auto& ct : m_containers) {
                    if(ct->pid > 0 && !ct->child_reaped) {
                        int status = 0;
                        pid_t r = waitpid(ct->pid, &status, WNOHANG);
                        if(r == ct->pid)
                            ct->child_reaped = true;
                        else if(r < 0 && errno == ECHILD)
                            ct->child_reaped = true;
                        else
                            any = true;
                    }
                }
                if(!any)
                    break;
                struct timespec ts;
                ts.tv_sec = 0;
                ts.tv_nsec = 10 * 1000 * 1000;
                nanosleep(&ts, nullptr);
            }
            for(auto& c : m_clients) {
                if(c->fd >= 0)
                    close(c->fd);
                c->fd = -1;
            }
            m_clients.clear();
            if(m_listen_fd >= 0)
                close(m_listen_fd);
            if(m_sig_fd >= 0)
                close(m_sig_fd);
            if(m_owns_lock) {
                if(!m_socket.empty())
                    unlink(m_socket.c_str());
                if(!m_pid_path.empty())
                    unlink(m_pid_path.c_str());
            }
            if(m_lock_fd >= 0) {
                flock(m_lock_fd, LOCK_UN);
                close(m_lock_fd);
            }
            m_owns_lock = false;
            m_listen_fd = m_sig_fd = m_lock_fd = -1;
        }

        Daemon::~Daemon() {
            cleanup();
        }

        int Daemon::run() {
            std::string err;
            if(!setup(err)) {
                if(m_already_running)
                    fprintf(stderr, "capsula: %s\n", err.c_str());
                else
                    fprintf(stderr, "capsula: daemon setup failed: %s\n", err.c_str());
                return m_already_running ? 0 : 1;
            }
            fprintf(stderr, "capsula: daemon listening on %s (pid %d)\n", m_socket.c_str(), static_cast<int>(getpid()));
            fflush(stderr);
            loop();
            cleanup();
            return 0;
        }

        int Daemon::compute_timeout() const {
            uint64_t now = now_ms();
            uint64_t best = 0;
            bool have = false;
            for(const auto& ct : m_containers) {
                if(ct->kill_pending && ct->kill_deadline_ms >= 0) {
                    uint64_t d = static_cast<uint64_t>(ct->kill_deadline_ms);
                    if(d <= now)
                        return 0;
                    if(!have || d < best) {
                        best = d;
                        have = true;
                    }
                }
            }
            if(!have)
                return -1;
            uint64_t delta = best - now;
            if(delta > 1000)
                delta = 1000;
            return static_cast<int>(delta);
        }

        void Daemon::check_kill_deadlines() {
            uint64_t now = now_ms();
            for(auto& ct : m_containers) {
                if(!ct->kill_pending || ct->kill_deadline_ms < 0)
                    continue;
                if(now < static_cast<uint64_t>(ct->kill_deadline_ms))
                    continue;
                if(ct->state != ContainerState::Exited && ct->pid > 0) {
                    kill(ct->pid, SIGKILL);
                    fprintf(stderr, "capsula: container %s grace period expired, sent SIGKILL\n", ct->id.c_str());
                }
                ct->kill_pending = false;
                ct->kill_deadline_ms = -1;
            }
        }

        void Daemon::handle_accept() {
            for(;;) {
                struct sockaddr_un addr;
                socklen_t len = sizeof(addr);
                int fd =
                    accept4(m_listen_fd, reinterpret_cast<struct sockaddr*>(&addr), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if(fd < 0) {
                    if(errno == EINTR)
                        continue;
                    if(errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    fprintf(stderr, "capsula: accept failed: %s\n", errno_string().c_str());
                    break;
                }
                struct ucred cred;
                socklen_t clen = sizeof(cred);
                if(getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) == 0 && cred.uid != geteuid()) {
                    close(fd);
                    continue;
                }
                auto c = std::make_unique<Client>();
                c->fd = fd;
                m_clients.push_back(std::move(c));
            }
        }

        void Daemon::handle_signal() {
            struct signalfd_siginfo si;
            for(;;) {
                ssize_t n = read(m_sig_fd, &si, sizeof(si));
                if(n < 0) {
                    if(errno == EINTR)
                        continue;
                    if(errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    return;
                }
                if(n != static_cast<ssize_t>(sizeof(si)))
                    break;
                if(si.ssi_signo == SIGCHLD) {
                    reap_children();
                } else if(si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT || si.ssi_signo == SIGHUP ||
                          si.ssi_signo == SIGQUIT) {
                    fprintf(stderr, "capsula: shutdown requested by signal %u\n", si.ssi_signo);
                    m_shutdown = true;
                }
            }
        }

        void Daemon::handle_client(Client& c, short revents) {
            if(c.fd < 0)
                return;
            if(revents & (POLLERR | POLLNVAL)) {
                close_client(c);
                return;
            }

            if(revents & POLLIN) {
                char buf[65536];
                for(;;) {
                    ssize_t n = recv(c.fd, buf, sizeof(buf), 0);
                    if(n > 0) {
                        c.reader.feed(buf, static_cast<size_t>(n));
                        if(!process_messages(c))
                            return;
                        if(c.fd < 0)
                            return;
                    } else if(n == 0) {
                        close_client(c);
                        return;
                    } else {
                        if(errno == EINTR)
                            continue;
                        if(errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        close_client(c);
                        return;
                    }
                }
            }

            if(revents & POLLOUT) {
                while(c.out.size() > c.out_off) {
                    ssize_t n = send(c.fd, c.out.data() + c.out_off, c.out.size() - c.out_off, MSG_NOSIGNAL);
                    if(n > 0) {
                        c.out_off += static_cast<size_t>(n);
                    } else if(n < 0 && errno == EINTR) {
                        continue;
                    } else if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    } else {
                        close_client(c);
                        return;
                    }
                }
                if(c.out_off == c.out.size()) {
                    c.out.clear();
                    c.out_off = 0;
                } else if(c.out_off > 1024 * 1024) {
                    c.out.erase(c.out.begin(), c.out.begin() + static_cast<std::ptrdiff_t>(c.out_off));
                    c.out_off = 0;
                }
            }

            if(revents & POLLHUP) {
                close_client(c);
            }
        }

        bool Daemon::process_messages(Client& c) {
            Message m;
            while(c.reader.next(m)) {
                if(!dispatch(c, m)) {
                    close_client(c);
                    return false;
                }
                if(c.fd < 0)
                    return false;
            }
            if(c.reader.failed()) {
                close_client(c);
                return false;
            }
            return true;
        }

        bool Daemon::dispatch(Client& c, const Message& m) {
            const auto type = static_cast<MsgType>(m.type);
            if(type == MsgType::Ping) {
                send_msg(c, MsgType::Pong, 0, {});
                return true;
            }
            if(type == MsgType::Hello) {
                Writer w;
                w.u32(kProtoVersion);
                send_msg(c, MsgType::HelloOk, 0, w.data());
                return true;
            }
            if(type == MsgType::Shutdown) {
                send_msg(c, MsgType::Pong, 0, {});
                m_shutdown = true;
                return true;
            }

            if(c.mode == Client::Mode::Attached) {
                Container* ct = find_container_by_id(c.target);
                if(type == MsgType::Input) {
                    if(ct && !m.payload.empty()) {
                        if(!queue_input(*ct, m.payload.data(), m.payload.size()))
                            return false;
                    }
                    return true;
                }
                if(type == MsgType::InputEof) {
                    if(ct && !ct->tty && ct->input_fd >= 0) {
                        ct->input_close_after = true;
                        if(ct->input_off >= ct->input_buf.size())
                            flush_input(*ct);
                    }
                    return true;
                }
                if(type == MsgType::Resize) {
                    Reader r(m.payload);
                    uint16_t cols = r.u16();
                    uint16_t rows = r.u16();
                    if(ct && ct->tty && ct->master_fd >= 0 && r.ok())
                        set_winsize(ct->master_fd, cols, rows);
                    return true;
                }
                if(type == MsgType::Detach) {
                    close_client(c);
                    return true;
                }
                send_error(c, 2, "client is attached; only input/resize/detach are allowed");
                return true;
            }

            if(c.mode == Client::Mode::Logs) {
                if(type == MsgType::Detach) {
                    close_client(c);
                    return true;
                }
                send_error(c, 2, "client is following logs; only detach is allowed");
                return true;
            }

            switch(type) {
                case MsgType::Run:
                    handle_run(c, m);
                    return true;
                case MsgType::List:
                    handle_list(c);
                    return true;
                case MsgType::Logs:
                    handle_logs(c, m);
                    return true;
                case MsgType::Attach:
                    handle_attach(c, m);
                    return true;
                case MsgType::Kill:
                    handle_kill(c, m);
                    return true;
                default:
                    send_error(c, 1, "unexpected request");
                    return true;
            }
        }

        void Daemon::handle_list(Client& c) {
            for(const auto& ct : m_containers) {
                ContainerInfo info;
                info.id = ct->id;
                info.name = ct->name;
                info.command = ct->command;
                info.state = ct->state;
                info.pid = static_cast<int32_t>(ct->pid);
                info.exit_code = ct->exit_code;
                info.term_signal = ct->term_signal;
                info.created_ms = ct->created_ms;
                info.tty = ct->tty;
                queue_bytes(c, encode_info(info));
                if(c.fd < 0)
                    return;
            }
            send_msg(c, MsgType::ListEnd, 0, {});
        }

        void Daemon::handle_logs(Client& c, const Message& m) {
            std::string target;
            bool follow = false;
            if(!decode_logs_request(m, target, follow)) {
                send_error(c, 1, "invalid logs request");
                return;
            }
            Container* ct = find_container(target);
            if(!ct) {
                send_error(c, 2, "container not found: " + target);
                return;
            }

            c.mode = Client::Mode::Logs;
            c.target = ct->id;
            c.follow = follow;

            int fd = open(ct->log_path.c_str(), O_RDONLY | O_CLOEXEC);
            if(fd >= 0) {
                std::vector<uint8_t> chunk(65536);
                for(;;) {
                    ssize_t n = read(fd, chunk.data(), chunk.size());
                    if(n > 0) {
                        std::vector<uint8_t> data(chunk.begin(), chunk.begin() + n);
                        send_msg(c, MsgType::LogData, 0, data);
                        if(c.fd < 0) {
                            close(fd);
                            return;
                        }
                    } else if(n < 0 && errno == EINTR) {
                        continue;
                    } else {
                        break;
                    }
                }
                close(fd);
            }

            if(!follow || ct->exit_sent) {
                send_msg(c, MsgType::LogEnd, 0, {});
                c.mode = Client::Mode::Control;
                c.target.clear();
                c.follow = false;
            }
        }

        void Daemon::handle_attach(Client& c, const Message& m) {
            std::string target;
            uint16_t cols = 0;
            uint16_t rows = 0;
            if(!decode_attach_request(m, target, cols, rows)) {
                send_error(c, 1, "invalid attach request");
                return;
            }
            Container* ct = find_container(target);
            if(!ct) {
                send_error(c, 2, "container not found: " + target);
                return;
            }
            if(ct->state == ContainerState::Exited) {
                send_error(c, 3, "container is not running: " + ct->id);
                return;
            }
            if(ct->tty && ct->master_fd >= 0 && cols && rows)
                set_winsize(ct->master_fd, cols, rows);

            c.mode = Client::Mode::Attached;
            c.target = ct->id;
            c.follow = false;

            Writer w;
            w.str(ct->id);
            w.i32(static_cast<int32_t>(ct->pid));
            w.u8(ct->tty ? 1 : 0);
            send_msg(c, MsgType::AttachOk, 0, w.data());
        }

        void Daemon::handle_kill(Client& c, const Message& m) {
            std::string target;
            int sig = SIGTERM;
            bool wait = false;
            if(!decode_kill_request(m, target, sig, wait)) {
                send_error(c, 1, "invalid kill request");
                return;
            }
            (void)wait;
            Container* ct = find_container(target);
            if(!ct) {
                send_error(c, 2, "container not found: " + target);
                return;
            }

            bool delivered = false;
            if(ct->state != ContainerState::Exited && ct->pid > 0) {
                if(kill(ct->pid, sig) == 0) {
                    delivered = true;
                    if(sig != 0 && sig != SIGKILL) {
                        ct->kill_pending = true;
                        ct->kill_deadline_ms = static_cast<int64_t>(now_ms()) + kKillGraceMs;
                    }
                } else if(errno == ESRCH) {
                    delivered = false;
                }
            }

            Writer w;
            w.str(ct->id);
            w.i32(static_cast<int32_t>(ct->pid));
            w.u8(delivered ? 1 : 0);
            w.i32(ct->exit_code);
            w.i32(ct->term_signal);
            send_msg(c, MsgType::KillOk, 0, w.data());
        }

        void Daemon::loop() {
            struct Target {
                // тип источника события:
                // - Signal - signalfd демона
                // - Listen - слушающий Unix-сокет;
                // - Client - соединение с клиентом;
                // - Container - один из fd контейнера.
                enum class Kind { Signal, Listen, Client, Container } kind;
                // индекс объекта в m_clients или в m_containers
                size_t index = 0;
                // какой именно fd контейнера дал событие:
                // - Master - PTY master;
                // - Stdout - pipe stdout;
                // - Stderr - pipe stderr;
                // - Stdin - write-конец pipe для stdin;
                // - Status - pipe стартовых ошибок дочернего процесса.
                FdRole role = FdRole::Master;
            };

            while(!m_shutdown) {
                std::vector<pollfd> pfds;
                std::vector<Target> targets;

                auto add = [&](int fd, short events, Target::Kind kind, size_t index, FdRole role) {
                    if(fd < 0)
                        return;
                    struct pollfd p;
                    p.fd = fd;
                    p.events = events;
                    p.revents = 0;
                    pfds.push_back(p);
                    Target t;
                    t.kind = kind;
                    t.index = index;
                    t.role = role;
                    targets.push_back(t);
                };

                // ждём сигналы
                add(m_sig_fd, POLLIN, Target::Kind::Signal, 0, FdRole::Master);

                // io с клиентами
                add(m_listen_fd, POLLIN, Target::Kind::Listen, 0, FdRole::Master);
                for(size_t i = 0; i < m_clients.size(); ++i) {
                    const Client& cl = *m_clients[i];
                    if(cl.fd < 0)
                        continue;
                    short ev = POLLIN;  // слушаем клиента
                    if(cl.out.size() > cl.out_off)
                        ev |= POLLOUT;  // в буфере есть данные для этого клиента
                    add(cl.fd, ev, Target::Kind::Client, i, FdRole::Master);
                }

                // io с контейнерами
                for(size_t i = 0; i < m_containers.size(); ++i) {
                    const Container& ct = *m_containers[i];
                    const bool pending = ct.input_off < ct.input_buf.size() || ct.input_close_after;
                    if(ct.master_fd >= 0) {
                        short ev = POLLIN;
                        if(pending && ct.input_fd == ct.master_fd)
                            ev |= POLLOUT;
                        add(ct.master_fd, ev, Target::Kind::Container, i, FdRole::Master);
                    }
                    if(ct.stdout_fd >= 0)
                        add(ct.stdout_fd, POLLIN, Target::Kind::Container, i, FdRole::Stdout);
                    if(ct.stderr_fd >= 0)
                        add(ct.stderr_fd, POLLIN, Target::Kind::Container, i, FdRole::Stderr);
                    if(ct.status_fd >= 0)
                        add(ct.status_fd, POLLIN, Target::Kind::Container, i, FdRole::Status);
                    if(ct.input_fd >= 0 && ct.input_fd != ct.master_fd && pending)
                        add(ct.input_fd, POLLOUT, Target::Kind::Container, i, FdRole::Stdin);
                }

                const int timeout = compute_timeout();
                const int n = poll(pfds.data(), pfds.size(), timeout);
                if(n < 0) {
                    if(errno == EINTR)
                        continue;
                    fprintf(stderr, "capsula: poll failed: %s\n", errno_string().c_str());
                    break;
                }
                if(n == 0) {
                    check_kill_deadlines();
                    continue;
                }

                for(size_t i = 0; i < pfds.size(); ++i) {
                    short rev = pfds[i].revents;
                    if(!rev)
                        continue;
                    const Target& t = targets[i];
                    switch(t.kind) {
                        case Target::Kind::Signal:
                            handle_signal();
                            break;
                        case Target::Kind::Listen:
                            handle_accept();
                            break;
                        case Target::Kind::Client:
                            if(t.index < m_clients.size())
                                handle_client(*m_clients[t.index], rev);
                            break;
                        case Target::Kind::Container:
                            if(t.index < m_containers.size())
                                handle_container_fd(*m_containers[t.index], t.role, rev);
                            break;
                    }
                }

                check_kill_deadlines();

                // Compact closed clients. Container indices stay stable because
                // containers are never removed during a daemon run.
                m_clients.erase(std::remove_if(m_clients.begin(), m_clients.end(),
                                               [](const std::unique_ptr<Client>& c) { return c->fd < 0; }),
                                m_clients.end());
            }
        }

        std::string Daemon::unique_id() {
            for(;;) {
                std::string id = random_hex(8);
                bool used = false;
                for(const auto& ct : m_containers) {
                    if(ct->id == id) {
                        used = true;
                        break;
                    }
                }
                if(!used)
                    return id;
            }
        }

        std::string Daemon::unique_mountpoint(const std::string& id) {
            std::string roots = join_path(m_runtime, "roots");
            if(!ensure_dir(roots, 0700))
                return std::string();
            std::string mp = join_path(roots, id);
            if(!ensure_dir(mp, 0700))
                return std::string();
            return mp;
        }

        std::string Daemon::make_log_dir(const std::string& id) {
            std::string dir = join_path(join_path(m_runtime, "containers"), id);
            if(!ensure_dir(dir, 0700))
                return std::string();
            return join_path(dir, "log");
        }

        void Daemon::handle_run(Client& c, const Message& m) {
            RunSpec spec;
            if(!decode_run(m, spec)) {
                send_error(c, 1, "invalid run request");
                return;
            }
            if(spec.argv.empty()) {
                send_error(c, 1, "empty command");
                return;
            }
            if(!valid_name(spec.name) && !spec.name.empty()) {
                send_error(c, 1, "invalid container name");
                return;
            }
            if(!spec.name.empty()) {
                for(const auto& ct : m_containers) {
                    if(ct->name == spec.name) {
                        send_error(c, 4, "container name already exists: " + spec.name);
                        return;
                    }
                }
            }
            std::string rootfs = abs_path(spec.rootfs);
            struct stat st;
            if(rootfs.empty() || stat(rootfs.c_str(), &st) < 0 || !S_ISDIR(st.st_mode)) {
                send_error(c, 1, "rootfs is not a directory: " + spec.rootfs);
                return;
            }
            std::string cwd = spec.cwd.empty() ? "/" : spec.cwd;
            if(cwd[0] != '/') {
                send_error(c, 1, "container cwd must be absolute");
                return;
            }

            std::string id = unique_id();
            std::string mountpoint = unique_mountpoint(id);
            if(mountpoint.empty()) {
                send_error(c, 1, "cannot create container mountpoint");
                return;
            }
            std::string log_path = make_log_dir(id);
            if(log_path.empty()) {
                rmdir(mountpoint.c_str());
                send_error(c, 1, "cannot create container log directory");
                return;
            }
            int log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
            if(log_fd < 0) {
                rmdir(mountpoint.c_str());
                send_error(c, 1, "cannot open container log: " + errno_string());
                return;
            }

            std::string hostname = spec.hostname;
            if(hostname.empty())
                hostname = spec.name.empty() ? id : spec.name;

            int syncp[2] = {-1, -1};
            int statusp[2] = {-1, -1};
            int pidpipe[2] = {-1, -1};
            int outp[2] = {-1, -1};
            int errp[2] = {-1, -1};
            int inp[2] = {-1, -1};
            int devnull = -1;
            int master = -1;
            std::string slave_path;
            pid_t launcher_pid = -1;
            pid_t init_pid = -1;
            bool maps_ok = false;

            auto close_fd = [](int& fd) {
                if(fd >= 0) {
                    close(fd);
                    fd = -1;
                }
            };

            auto fail_run = [&](const std::string& text) {
                if(init_pid > 0)
                    kill(init_pid, SIGKILL);
                if(launcher_pid > 0) {
                    close_fd(syncp[1]);
                    kill(launcher_pid, SIGKILL);
                    int status = 0;
                    while(waitpid(launcher_pid, &status, 0) < 0 && errno == EINTR) {
                    }
                }
                close_fd(syncp[0]);
                close_fd(syncp[1]);
                close_fd(statusp[0]);
                close_fd(statusp[1]);
                close_fd(pidpipe[0]);
                close_fd(pidpipe[1]);
                close_fd(outp[0]);
                close_fd(outp[1]);
                close_fd(errp[0]);
                close_fd(errp[1]);
                close_fd(inp[0]);
                close_fd(inp[1]);
                close_fd(devnull);
                close_fd(master);
                close_fd(log_fd);
                unlink(log_path.c_str());
                rmdir(join_path(join_path(m_runtime, "containers"), id).c_str());
                rmdir(mountpoint.c_str());
                send_error(c, 1, text);
            };

            if(pipe2(syncp, O_CLOEXEC) < 0 || pipe2(statusp, O_CLOEXEC) < 0 || pipe2(pidpipe, O_CLOEXEC) < 0) {
                fail_run("cannot create startup pipes: " + errno_string());
                return;
            }

            if(spec.tty) {
                master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
                if(master < 0) {
                    fail_run("posix_openpt failed: " + errno_string());
                    return;
                }
                if(grantpt(master) < 0 || unlockpt(master) < 0) {
                    fail_run("cannot prepare PTY: " + errno_string());
                    return;
                }
                char ptn[256];
                if(ptsname_r(master, ptn, sizeof(ptn)) < 0) {
                    fail_run("ptsname_r failed: " + errno_string());
                    return;
                }
                slave_path = ptn;
                if(!set_nonblocking(master, true)) {
                    fail_run("cannot set PTY non-blocking: " + errno_string());
                    return;
                }
                set_winsize(master, spec.cols ? spec.cols : 80, spec.rows ? spec.rows : 24);
            } else {
                if(pipe2(outp, O_CLOEXEC) < 0 || pipe2(errp, O_CLOEXEC) < 0) {
                    fail_run("cannot create output pipes: " + errno_string());
                    return;
                }
                if(!set_nonblocking(outp[0], true) || !set_nonblocking(errp[0], true)) {
                    fail_run("cannot set output pipes non-blocking: " + errno_string());
                    return;
                }
                if(spec.detach) {
                    devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
                    if(devnull < 0) {
                        fail_run("cannot open /dev/null: " + errno_string());
                        return;
                    }
                } else {
                    if(pipe2(inp, O_CLOEXEC) < 0) {
                        fail_run("cannot create stdin pipe: " + errno_string());
                        return;
                    }
                    set_nonblocking(inp[1], true);
                }
            }

            ChildConfig cfg;
            cfg.sync_fd = syncp[0];
            cfg.status_fd = statusp[1];
            cfg.pid_fd = pidpipe[1];
            cfg.master_fd = master;
            cfg.slave_path = slave_path;
            cfg.tty = spec.tty;
            cfg.rootfs = rootfs;
            cfg.mountpoint = mountpoint;
            cfg.hostname = hostname;
            cfg.cwd = cwd;
            cfg.argv = spec.argv;
            cfg.env = spec.env;
            if(spec.tty) {
                cfg.stdin_read = -1;
                cfg.stdout_write = -1;
                cfg.stderr_write = -1;
            } else {
                cfg.stdin_read = spec.detach ? devnull : inp[0];
                cfg.stdout_write = outp[1];
                cfg.stderr_write = errp[1];
            }

            const size_t stack_size = 1024 * 1024;
            void* stack =
                mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
            if(stack == MAP_FAILED) {
                fail_run("cannot allocate child stack: " + errno_string());
                return;
            }
            void* stack_top = static_cast<char*>(stack) + stack_size;
            stack_top = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(stack_top) & ~static_cast<uintptr_t>(15));

            int clone_flags = CLONE_NEWUSER | SIGCHLD;
            launcher_pid = clone(container_launcher, stack_top, clone_flags, &cfg);
            int clone_err = errno;
            munmap(stack, stack_size);
            if(launcher_pid < 0) {
                std::string msg = "clone failed: " + errno_string(clone_err);
                if(clone_err == EPERM)
                    msg +=
                        " (user namespaces may be disabled; run as root or enable "
                        "unprivileged user namespaces)";
                fail_run(msg);
                return;
            }

            close_fd(syncp[0]);
            close_fd(statusp[1]);
            close_fd(pidpipe[1]);
            if(!spec.tty) {
                close_fd(outp[1]);
                close_fd(errp[1]);
                if(spec.detach)
                    close_fd(devnull);
                else
                    close_fd(inp[0]);
            }

            std::string map_err;
            maps_ok = setup_userns_maps(launcher_pid, map_err);
            if(!maps_ok) {
                fail_run(map_err);
                return;
            }

            if(!write_all(syncp[1], "g", 1)) {
                fail_run("cannot start container child: " + errno_string());
                return;
            }
            close_fd(syncp[1]);

            {
                struct pollfd pfd;
                pfd.fd = pidpipe[0];
                pfd.events = POLLIN;
                pfd.revents = 0;
                int pr = poll(&pfd, 1, 3000);
                int32_t pid32 = 0;
                ssize_t got = 0;
                while(pr > 0 && (pfd.revents & POLLIN) && got < static_cast<ssize_t>(sizeof(pid32))) {
                    ssize_t r = read(pidpipe[0], reinterpret_cast<char*>(&pid32) + got,
                                     sizeof(pid32) - static_cast<size_t>(got));
                    if(r > 0)
                        got += r;
                    else if(r < 0 && errno == EINTR)
                        continue;
                    else
                        break;
                }
                if(pr <= 0 || got != static_cast<ssize_t>(sizeof(pid32))) {
                    std::string detail;
                    struct pollfd spfd;
                    spfd.fd = statusp[0];
                    spfd.events = POLLIN;
                    spfd.revents = 0;
                    if(poll(&spfd, 1, 100) > 0 && (spfd.revents & POLLIN)) {
                        char ebuf[512];
                        ssize_t en = read(statusp[0], ebuf, sizeof(ebuf) - 1);
                        if(en > 0) {
                            ebuf[en] = '\0';
                            detail = ": " + describe_status(ebuf);
                        }
                    }
                    fail_run("container init did not start" + detail);
                    return;
                }
                init_pid = static_cast<pid_t>(pid32);
                close_fd(pidpipe[0]);
            }

            if(!set_nonblocking(statusp[0], true)) {
                fail_run("cannot set status pipe non-blocking: " + errno_string());
                return;
            }

            auto ct = std::make_unique<Container>();
            ct->id = id;
            ct->name = spec.name;
            ct->rootfs = rootfs;
            ct->mountpoint = mountpoint;
            ct->hostname = hostname;
            ct->cwd = cwd;
            ct->command = join_words(spec.argv);
            ct->log_path = log_path;
            ct->argv = spec.argv;
            ct->env = spec.env;
            ct->tty = spec.tty;
            ct->detach = spec.detach;
            ct->pid = init_pid;
            ct->launcher_pid = launcher_pid;
            ct->created_ms = static_cast<uint64_t>(time(nullptr)) * 1000ull;
            ct->master_fd = master;
            ct->stdout_fd = spec.tty ? -1 : outp[0];
            ct->stderr_fd = spec.tty ? -1 : errp[0];
            ct->status_fd = statusp[0];
            ct->input_fd = spec.tty ? master : (spec.detach ? -1 : inp[1]);
            ct->log_fd = log_fd;
            if(spec.tty)
                ct->master_eof = false;
            m_containers.push_back(std::move(ct));

            Writer w;
            w.str(id);
            w.str(spec.name);
            w.i32(static_cast<int32_t>(init_pid));
            w.u8(spec.tty ? 1 : 0);
            send_msg(c, MsgType::RunOk, 0, w.data());
            if(!spec.detach) {
                c.mode = Client::Mode::Attached;
                c.target = id;
                c.follow = false;
            }
        }

        void Daemon::reap_children() {
            for(;;) {
                int status = 0;
                pid_t pid = waitpid(-1, &status, WNOHANG);
                if(pid < 0) {
                    if(errno == EINTR)
                        continue;
                    break;
                }
                if(pid == 0)
                    break;
                Container* ct = nullptr;
                bool launcher = false;
                for(auto& c : m_containers) {
                    if(c->pid == pid) {
                        ct = c.get();
                        break;
                    }
                    if(c->launcher_pid == pid) {
                        ct = c.get();
                        launcher = true;
                        break;
                    }
                }
                if(!ct) {
                    fprintf(stderr, "capsula: reaped unknown child %d\n", static_cast<int>(pid));
                    continue;
                }
                if(launcher) {
                    ct->launcher_reaped = true;
                    continue;
                }
                int term_sig = 0;
                ct->exit_code = decode_wait_status(status, term_sig);
                ct->term_signal = term_sig;
                ct->child_reaped = true;
                ct->state = ContainerState::Exited;
                ct->kill_pending = false;
                ct->kill_deadline_ms = -1;
                maybe_finalize(*ct);
            }
        }

        void Daemon::handle_container_fd(Container& ct, FdRole role, short revents) {
            if(revents & POLLNVAL) {
                if(role == FdRole::Master)
                    ct.master_fd = -1;
                else if(role == FdRole::Stdout)
                    ct.stdout_fd = -1;
                else if(role == FdRole::Stderr)
                    ct.stderr_fd = -1;
                else if(role == FdRole::Status)
                    ct.status_fd = -1;
                else
                    ct.input_fd = -1;
                maybe_finalize(ct);
                return;
            }
            if(revents & POLLOUT) {
                if(role == FdRole::Master || role == FdRole::Stdin)
                    flush_input(ct);
            }
            if(revents & (POLLIN | POLLHUP | POLLERR)) {
                if(role == FdRole::Master) {
                    read_output(ct, role);
                } else if(role == FdRole::Stdout || role == FdRole::Stderr) {
                    read_output(ct, role);
                } else if(role == FdRole::Status) {
                    read_status(ct);
                }
            }
        }

        void Daemon::read_output(Container& ct, FdRole role) {
            int& fd = (role == FdRole::Master) ? ct.master_fd : (role == FdRole::Stdout) ? ct.stdout_fd : ct.stderr_fd;
            if(fd < 0)
                return;
            uint16_t stream = (role == FdRole::Stderr) ? kStreamStderr : kStreamStdout;
            char buf[65536];
            for(;;) {
                ssize_t n = read(fd, buf, sizeof(buf));
                if(n > 0) {
                    std::vector<uint8_t> data(buf, buf + n);
                    if(ct.log_fd >= 0)
                        write_all(ct.log_fd, data.data(), data.size());
                    broadcast_output(ct, stream, data);
                    broadcast_log(ct, data);
                } else if(n == 0) {
                    close_output_fd(ct, role);
                    return;
                } else if(errno == EINTR) {
                    continue;
                } else if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                } else {
                    // PTY masters report EIO after the last slave descriptor closes.
                    close_output_fd(ct, role);
                    return;
                }
            }
        }

        void Daemon::read_status(Container& ct) {
            if(ct.status_fd < 0)
                return;
            char buf[4096];
            for(;;) {
                ssize_t n = read(ct.status_fd, buf, sizeof(buf));
                if(n > 0) {
                    if(ct.status_buf.size() < 65536)
                        ct.status_buf.append(buf, static_cast<size_t>(n));
                } else if(n == 0) {
                    close(ct.status_fd);
                    ct.status_fd = -1;
                    ct.status_eof = true;
                    if(!ct.status_buf.empty()) {
                        ct.startup_error = describe_status(ct.status_buf);
                        std::string line = ct.startup_error + "\n";
                        if(ct.log_fd >= 0)
                            write_all(ct.log_fd, line);
                        std::vector<uint8_t> data(line.begin(), line.end());
                        broadcast_output(ct, kStreamStderr, data);
                        broadcast_log(ct, data);
                    } else if(ct.state == ContainerState::Starting) {
                        ct.state = ContainerState::Running;
                    }
                    maybe_finalize(ct);
                    return;
                } else if(errno == EINTR) {
                    continue;
                } else if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                } else {
                    close(ct.status_fd);
                    ct.status_fd = -1;
                    ct.status_eof = true;
                    if(ct.state == ContainerState::Starting)
                        ct.state = ContainerState::Running;
                    maybe_finalize(ct);
                    return;
                }
            }
        }

        void Daemon::close_output_fd(Container& ct, FdRole role) {
            if(role == FdRole::Master && ct.master_fd >= 0) {
                const int old_master = ct.master_fd;
                close(ct.master_fd);
                ct.master_fd = -1;
                ct.master_eof = true;
                if(ct.input_fd == old_master)
                    ct.input_fd = -1;
            } else if(role == FdRole::Stdout && ct.stdout_fd >= 0) {
                close(ct.stdout_fd);
                ct.stdout_fd = -1;
                ct.stdout_eof = true;
            } else if(role == FdRole::Stderr && ct.stderr_fd >= 0) {
                close(ct.stderr_fd);
                ct.stderr_fd = -1;
                ct.stderr_eof = true;
            }
            maybe_finalize(ct);
        }

        void Daemon::maybe_finalize(Container& ct) {
            if(ct.exit_sent || !ct.child_reaped)
                return;
            if(ct.status_fd >= 0 || ct.master_fd >= 0 || ct.stdout_fd >= 0 || ct.stderr_fd >= 0)
                return;

            ct.exit_sent = true;
            ct.state = ContainerState::Exited;

            for(auto& c : m_clients) {
                if(c->fd < 0 || c->target != ct.id)
                    continue;
                if(c->mode == Client::Mode::Attached) {
                    queue_bytes(*c, encode_exit(ct.exit_code, ct.term_signal));
                    c->mode = Client::Mode::Control;
                    c->target.clear();
                } else if(c->mode == Client::Mode::Logs) {
                    send_msg(*c, MsgType::LogEnd, 0, {});
                    c->mode = Client::Mode::Control;
                    c->follow = false;
                    c->target.clear();
                }
            }

            if(ct.log_fd >= 0) {
                close(ct.log_fd);
                ct.log_fd = -1;
            }
            if(!ct.mountpoint.empty())
                rmdir(ct.mountpoint.c_str());
        }

        void Daemon::queue_bytes(Client& c, const std::vector<uint8_t>& data) {
            if(c.fd < 0)
                return;
            size_t pending = c.out.size() - c.out_off;
            if(pending + data.size() > kMaxOutputQueue) {
                fprintf(stderr, "capsula: client output queue overflow, disconnecting\n");
                close_client(c);
                return;
            }
            if(c.out_off > 0 && c.out_off == c.out.size()) {
                c.out.clear();
                c.out_off = 0;
            }
            c.out.insert(c.out.end(), data.begin(), data.end());
        }

        void Daemon::send_msg(Client& c, MsgType type, uint16_t flags, const std::vector<uint8_t>& payload) {
            queue_bytes(c, encode_message(type, flags, payload));
        }

        void Daemon::send_error(Client& c, uint32_t code, const std::string& text) {
            // encode_error() already produces a complete framed message.
            queue_bytes(c, encode_error(code, text));
        }

        void Daemon::close_client(Client& c) {
            if(c.fd >= 0) {
                close(c.fd);
                c.fd = -1;
            }
            c.mode = Client::Mode::Control;
            c.target.clear();
            c.follow = false;
            c.reader.clear();
            c.out.clear();
            c.out_off = 0;
        }

        void Daemon::broadcast_output(Container& ct, uint16_t stream, const std::vector<uint8_t>& data) {
            for(auto& c : m_clients) {
                if(c->fd < 0 || c->mode != Client::Mode::Attached || c->target != ct.id)
                    continue;
                send_msg(*c, MsgType::Output, stream, data);
            }
        }

        void Daemon::broadcast_log(Container& ct, const std::vector<uint8_t>& data) {
            for(auto& c : m_clients) {
                if(c->fd < 0 || c->mode != Client::Mode::Logs || c->target != ct.id)
                    continue;
                send_msg(*c, MsgType::LogData, 0, data);
            }
        }

        bool Daemon::queue_input(Container& ct, const void* data, size_t len) {
            if(ct.input_fd < 0)
                return true;
            size_t pending = ct.input_buf.size() - ct.input_off;
            if(pending + len > kMaxInputQueue)
                return false;
            if(ct.input_off == ct.input_buf.size()) {
                ct.input_buf.clear();
                ct.input_off = 0;
            }
            const auto* p = static_cast<const uint8_t*>(data);
            ct.input_buf.insert(ct.input_buf.end(), p, p + len);
            flush_input(ct);
            return true;
        }

        void Daemon::flush_input(Container& ct) {
            if(ct.input_fd < 0) {
                ct.input_buf.clear();
                ct.input_off = 0;
                return;
            }
            while(ct.input_off < ct.input_buf.size()) {
                ssize_t n = write(ct.input_fd, ct.input_buf.data() + ct.input_off, ct.input_buf.size() - ct.input_off);
                if(n > 0) {
                    ct.input_off += static_cast<size_t>(n);
                } else if(n < 0 && errno == EINTR) {
                    continue;
                } else if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    return;
                } else {
                    if(ct.input_fd != ct.master_fd) {
                        close(ct.input_fd);
                        ct.input_fd = -1;
                    }
                    ct.input_buf.clear();
                    ct.input_off = 0;
                    return;
                }
            }
            ct.input_buf.clear();
            ct.input_off = 0;
            if(ct.input_close_after && ct.input_fd >= 0 && ct.input_fd != ct.master_fd) {
                close(ct.input_fd);
                ct.input_fd = -1;
            }
        }

        Container* Daemon::find_container(const std::string& target) {
            for(auto& ct : m_containers) {
                if(ct->id == target)
                    return ct.get();
            }
            for(auto& ct : m_containers) {
                if(!ct->name.empty() && ct->name == target)
                    return ct.get();
            }
            if(all_digits(target)) {
                long v = 0;
                if(parse_long(target, v, 1, INT_MAX)) {
                    for(auto& ct : m_containers) {
                        if(ct->pid == static_cast<pid_t>(v))
                            return ct.get();
                    }
                }
            }
            return nullptr;
        }

        Container* Daemon::find_container_by_id(const std::string& id) {
            for(auto& ct : m_containers) {
                if(ct->id == id)
                    return ct.get();
            }
            return nullptr;
        }

    }  // namespace

    int daemon_main(const DaemonOptions& options) {
        DaemonOptions opts = options;
        if(opts.runtime.empty())
            opts.runtime = runtime_dir();
        opts.runtime = abs_path(opts.runtime);
        if(opts.socket.empty())
            opts.socket = socket_path();
        opts.socket = abs_path(opts.socket);
        if(!opts.log.empty())
            opts.log = abs_path(opts.log);

        if(!opts.foreground) {
            if(!ensure_dir(opts.runtime, 0700)) {
                fprintf(stderr, "capsula: cannot create runtime directory: %s\n", errno_string().c_str());
                return 1;
            }
            std::string log = opts.log.empty() ? join_path(opts.runtime, "daemon.log") : opts.log;
            if(!daemonize(log))
                return 1;
        }

        Daemon d(std::move(opts));
        return d.run();
    }

}  // namespace capsula
