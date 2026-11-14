#include "capsula/util.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

extern char** environ;

namespace capsula {

    std::string errno_string(int err) {
        char buf[256];
        buf[0] = '\0';
        const char* s = strerror(err);
        if(s)
            snprintf(buf, sizeof(buf), "%s", s);
        return std::string(buf);
    }

    std::string errno_string() {
        return errno_string(errno);
    }

    bool set_cloexec(int fd) {
        int flags = fcntl(fd, F_GETFD);
        if(flags < 0)
            return false;
        return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
    }

    bool set_nonblocking(int fd, bool enabled) {
        int flags = fcntl(fd, F_GETFL);
        if(flags < 0)
            return false;
        if(enabled)
            flags |= O_NONBLOCK;
        else
            flags &= ~O_NONBLOCK;
        return fcntl(fd, F_SETFL, flags) == 0;
    }

    bool write_all(int fd, const void* data, size_t len) {
        const auto* p = static_cast<const uint8_t*>(data);
        size_t off = 0;
        while(off < len) {
            ssize_t n = write(fd, p + off, len - off);
            if(n < 0) {
                if(errno == EINTR)
                    continue;
                return false;
            }
            if(n == 0)
                return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    bool write_all(int fd, const std::string& s) {
        return write_all(fd, s.data(), s.size());
    }

    bool write_file(const std::string& path, const std::string& content, int mode) {
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
        if(fd < 0)
            return false;
        bool ok = write_all(fd, content);
        if(close(fd) < 0)
            ok = false;
        return ok;
    }

    bool read_file(const std::string& path, std::string& out) {
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if(fd < 0)
            return false;
        out.clear();
        char buf[4096];
        for(;;) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if(n < 0) {
                if(errno == EINTR)
                    continue;
                close(fd);
                return false;
            }
            if(n == 0)
                break;
            out.append(buf, static_cast<size_t>(n));
        }
        close(fd);
        return true;
    }

    std::string self_exe_path() {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if(n <= 0)
            return std::string();
        buf[n] = '\0';
        return std::string(buf);
    }

    std::string runtime_dir() {
        const char* explicit_dir = getenv("CAPSULA_RUNTIME_DIR");
        if(explicit_dir && *explicit_dir)
            return abs_path(explicit_dir);
        const char* xdg = getenv("XDG_RUNTIME_DIR");
        if(xdg && *xdg)
            return join_path(xdg, "capsula");
        char buf[64];
        snprintf(buf, sizeof(buf), "/tmp/capsula-%u", static_cast<unsigned>(geteuid()));
        return std::string(buf);
    }

    std::string socket_path() {
        const char* sock = getenv("CAPSULA_SOCKET");
        if(sock && *sock)
            return abs_path(sock);
        return join_path(runtime_dir(), "capsula.sock");
    }

    std::string abs_path(const std::string& path) {
        if(path.empty())
            return path;
        char* resolved = realpath(path.c_str(), nullptr);
        if(resolved) {
            std::string s(resolved);
            free(resolved);
            return s;
        }
        if(path[0] == '/')
            return path;
        char cwd[4096];
        if(!getcwd(cwd, sizeof(cwd)))
            return path;
        return join_path(cwd, path);
    }

    std::string join_path(const std::string& a, const std::string& b) {
        if(a.empty())
            return b;
        if(b.empty())
            return a;
        if(a.back() == '/')
            return a + b;
        return a + "/" + b;
    }

    std::string base_name(const std::string& path) {
        size_t pos = path.find_last_of('/');
        if(pos == std::string::npos)
            return path;
        return path.substr(pos + 1);
    }

    bool is_dir(const std::string& path) {
        struct stat st;
        return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }

    bool ensure_dir(const std::string& path, int mode) {
        if(path.empty())
            return false;
        if(path == "/")
            return true;
        std::string cur;
        size_t i = 0;
        if(path[0] == '/') {
            cur = "/";
            i = 1;
        }
        while(i <= path.size()) {
            size_t slash = path.find('/', i);
            std::string part = path.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
            if(!part.empty()) {
                if(cur == "/")
                    cur += part;
                else if(cur.empty())
                    cur = part;
                else
                    cur += "/" + part;
                struct stat st;
                if(lstat(cur.c_str(), &st) == 0) {
                    if(!S_ISDIR(st.st_mode))
                        return false;
                } else if(errno == ENOENT) {
                    if(mkdir(cur.c_str(), static_cast<mode_t>(mode)) < 0 && errno != EEXIST)
                        return false;
                } else {
                    return false;
                }
            }
            if(slash == std::string::npos)
                break;
            i = slash + 1;
        }
        return is_dir(path);
    }

    uint64_t now_ms() {
        struct timespec ts;
        if(clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
            return 0;
        return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
    }

    uint64_t random_u64() {
        uint64_t v = 0;
        ssize_t n = getrandom(&v, sizeof(v), 0);
        if(n == static_cast<ssize_t>(sizeof(v)))
            return v;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        v = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
        v ^= static_cast<uint64_t>(getpid()) << 32;
        v ^= reinterpret_cast<uintptr_t>(&v);
        return v;
    }

    std::string random_hex(size_t chars) {
        static const char hex[] = "0123456789abcdef";
        std::string out;
        out.reserve(chars);
        while(out.size() < chars) {
            uint64_t v = random_u64();
            for(int i = 0; i < 16 && out.size() < chars; ++i) {
                out.push_back(hex[(v >> (4 * i)) & 0xf]);
            }
        }
        return out;
    }

    bool parse_long(const std::string& s, long& out, long min, long max) {
        if(s.empty())
            return false;
        errno = 0;
        char* end = nullptr;
        long v = strtol(s.c_str(), &end, 10);
        if(errno != 0 || !end || *end != '\0')
            return false;
        if(v < min || v > max)
            return false;
        out = v;
        return true;
    }

    bool parse_signal(const std::string& input, int& sig) {
        if(input.empty())
            return false;
        long num = 0;
        if(parse_long(input, num, 0, 64)) {
            sig = static_cast<int>(num);
            return true;
        }
        std::string s = input;
        for(auto& c : s)
            c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if(s.rfind("SIG", 0) == 0)
            s = s.substr(3);
        struct Entry {
            const char* name;
            int sig;
        };
        static const Entry table[] = {
            {"HUP", SIGHUP},   {"INT", SIGINT},     {"QUIT", SIGQUIT}, {"ILL", SIGILL},   {"TRAP", SIGTRAP},
            {"ABRT", SIGABRT}, {"BUS", SIGBUS},     {"FPE", SIGFPE},   {"KILL", SIGKILL}, {"USR1", SIGUSR1},
            {"SEGV", SIGSEGV}, {"USR2", SIGUSR2},   {"PIPE", SIGPIPE}, {"ALRM", SIGALRM}, {"TERM", SIGTERM},
            {"CHLD", SIGCHLD}, {"CONT", SIGCONT},   {"STOP", SIGSTOP}, {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN},
            {"TTOU", SIGTTOU}, {"URG", SIGURG},     {"XCPU", SIGXCPU}, {"XFSZ", SIGXFSZ}, {"VTALRM", SIGVTALRM},
            {"PROF", SIGPROF}, {"WINCH", SIGWINCH}, {"IO", SIGIO},     {"PWR", SIGPWR},   {"SYS", SIGSYS},
        };
        for(const auto& e : table) {
            if(s == e.name) {
                sig = e.sig;
                return true;
            }
        }
        return false;
    }

    std::string signal_name(int sig) {
        switch(sig) {
            case SIGHUP:
                return "HUP";
            case SIGINT:
                return "INT";
            case SIGQUIT:
                return "QUIT";
            case SIGKILL:
                return "KILL";
            case SIGTERM:
                return "TERM";
            case SIGSTOP:
                return "STOP";
            case SIGCONT:
                return "CONT";
            case SIGWINCH:
                return "WINCH";
            default:
                break;
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", sig);
        return std::string(buf);
    }

    std::vector<std::string> current_env() {
        std::vector<std::string> out;
        if(!environ)
            return out;
        for(char** p = environ; *p; ++p) {
            if(**p)
                out.push_back(*p);
        }
        return out;
    }

    bool env_lookup(const std::vector<std::string>& env, const std::string& key, std::string& value) {
        const std::string prefix = key + "=";
        for(const auto& e : env) {
            if(e.rfind(prefix, 0) == 0) {
                value = e.substr(prefix.size());
                return true;
            }
        }
        return false;
    }

    void env_set(std::vector<std::string>& env, const std::string& key, const std::string& value) {
        const std::string prefix = key + "=";
        for(auto& e : env) {
            if(e.rfind(prefix, 0) == 0) {
                e = prefix + value;
                return;
            }
        }
        env.push_back(prefix + value);
    }

    void env_unset(std::vector<std::string>& env, const std::string& key) {
        const std::string prefix = key + "=";
        env.erase(std::remove_if(env.begin(), env.end(), [&](const std::string& e) { return e.rfind(prefix, 0) == 0; }),
                  env.end());
    }

    std::string shell_quote(const std::string& s) {
        if(s.find_first_of(" \t'\"\\$`") == std::string::npos)
            return s;
        std::string out = "'";
        for(char c : s) {
            if(c == '\'')
                out += "'\\''";
            else
                out.push_back(c);
        }
        out += "'";
        return out;
    }

    std::string join_words(const std::vector<std::string>& words) {
        std::string out;
        for(size_t i = 0; i < words.size(); ++i) {
            if(i)
                out.push_back(' ');
            out += shell_quote(words[i]);
        }
        return out;
    }

    int connect_unix(const std::string& path, std::string& err) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if(path.size() >= sizeof(addr.sun_path)) {
            err = "socket path is too long";
            return -1;
        }
        memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if(fd < 0) {
            err = errno_string();
            return -1;
        }
        for(;;) {
            if(connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0)
                return fd;
            if(errno == EINTR)
                continue;
            err = errno_string();
            close(fd);
            return -1;
        }
    }

    int listen_unix(const std::string& path, std::string& err) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if(path.size() >= sizeof(addr.sun_path)) {
            err = "socket path is too long";
            return -1;
        }
        memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if(fd < 0) {
            err = errno_string();
            return -1;
        }
        unlink(path.c_str());
        if(bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            err = errno_string();
            close(fd);
            return -1;
        }
        chmod(path.c_str(), 0600);
        if(listen(fd, 64) < 0) {
            err = errno_string();
            close(fd);
            unlink(path.c_str());
            return -1;
        }
        return fd;
    }

    bool set_socket_timeout(int fd, int seconds) {
        struct timeval tv;
        tv.tv_sec = seconds;
        tv.tv_usec = 0;
        return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
               setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
    }

    bool read_winsize(int fd, uint16_t& cols, uint16_t& rows) {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        if(ioctl(fd, TIOCGWINSZ, &ws) < 0)
            return false;
        cols = ws.ws_col ? ws.ws_col : 80;
        rows = ws.ws_row ? ws.ws_row : 24;
        return true;
    }

    bool set_winsize(int fd, uint16_t cols, uint16_t rows) {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_col = cols;
        ws.ws_row = rows;
        return ioctl(fd, TIOCSWINSZ, &ws) == 0;
    }

    bool daemonize(const std::string& log_path) {
        pid_t pid = fork();
        if(pid < 0)
            return false;
        if(pid > 0)
            _exit(0);

        // отвязываем дочерний процесс от терминала родителя и делаем его лидером нового сеанса
        if(setsid() < 0)
            return false;

        // создаём "внучатый" процесс
        pid = fork();
        if(pid < 0)
            return false;
        if(pid > 0)
            _exit(0);

        // у процесса-внука нет управлящего терминала и он не может стать лидером сеанса,
        // чтобы получить управляющий терминал => получили настоящий демон

        if(chdir("/") < 0)
            return false;
        umask(077);
        int nullfd = open("/dev/null", O_RDWR | O_CLOEXEC);
        if(nullfd < 0)
            return false;
        int outfd = nullfd;
        if(!log_path.empty())
            outfd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if(outfd < 0)
            outfd = nullfd;
        dup2(nullfd, STDIN_FILENO);
        dup2(outfd, STDOUT_FILENO);
        dup2(outfd, STDERR_FILENO);
        if(nullfd > STDERR_FILENO)
            close(nullfd);
        if(outfd > STDERR_FILENO && outfd != nullfd)
            close(outfd);
        return true;
    }

    bool spawn_detached(const std::vector<std::string>& argv, const std::string& log_path, std::string& err) {
        if(argv.empty()) {
            err = "empty command";
            return false;
        }
        pid_t pid = fork();
        if(pid < 0) {
            err = errno_string();
            return false;
        }
        if(pid == 0) {
            if(setsid() < 0)
                _exit(127);
            if(chdir("/") < 0)
                _exit(127);
            int nullfd = open("/dev/null", O_RDWR);
            if(nullfd < 0)
                _exit(127);
            int outfd = nullfd;
            if(!log_path.empty())
                outfd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
            if(outfd < 0)
                outfd = nullfd;
            dup2(nullfd, STDIN_FILENO);
            dup2(outfd, STDOUT_FILENO);
            dup2(outfd, STDERR_FILENO);
            if(nullfd > STDERR_FILENO)
                close(nullfd);
            if(outfd > STDERR_FILENO && outfd != nullfd)
                close(outfd);
            std::vector<char*> cargv;
            cargv.reserve(argv.size() + 1);
            for(const auto& a : argv)
                cargv.push_back(const_cast<char*>(a.c_str()));
            cargv.push_back(nullptr);
            execv(cargv[0], cargv.data());
            _exit(127);
        }
        return true;
    }

    bool wait_for_socket(const std::string& path, int timeout_ms) {
        uint64_t deadline = now_ms() + static_cast<uint64_t>(timeout_ms);
        for(;;) {
            std::string err;
            int fd = connect_unix(path, err);
            if(fd >= 0) {
                close(fd);
                return true;
            }
            if(now_ms() >= deadline)
                return false;
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 20 * 1000 * 1000;
            nanosleep(&ts, nullptr);
        }
    }

    bool kill_process(pid_t pid, int sig) {
        if(pid <= 0)
            return false;
        return kill(pid, sig) == 0;
    }

}  // namespace capsula
