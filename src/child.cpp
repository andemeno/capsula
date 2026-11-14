#include "capsula/child.hpp"
#include "capsula/util.hpp"

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace capsula {
    namespace {

        [[noreturn]] void fail(int status_fd, const char* stage, int err) {
            if(status_fd >= 0) {
                char buf[512];
                int n = snprintf(buf, sizeof(buf), "%s:%d:%s", stage, err, errno_string(err).c_str());
                if(n > 0) {
                    size_t len = static_cast<size_t>(n);
                    if(len >= sizeof(buf))
                        len = sizeof(buf) - 1;
                    ssize_t ignored = write(status_fd, buf, len);
                    (void)ignored;
                }
            }
            _exit(127);
        }

        // Unprivileged user namespaces cannot create device nodes with mknod(), even
        // when the caller is root inside the namespace. Bind mount the host device
        // nodes over empty files in the tmpfs instead: mounting a device that is
        // already accessible needs no device-node creation capability.
        bool bind_device(const std::string& source, const std::string& target) {
            int fd = open(target.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
            if(fd < 0)
                return false;
            close(fd);
            return mount(source.c_str(), target.c_str(), nullptr, MS_BIND, nullptr) == 0;
        }

        // The root filesystem is a bind mount at this point.  Everything is mounted
        // into that tree *before* pivot_root: on some kernels mounting procfs after
        // pivot_root inside a user namespace returns EPERM.
        void setup_devices(const std::string& root, int status_fd) {
            const std::string dev = root + "/dev";
            mkdir(dev.c_str(), 0755);
            if(mount("tmpfs", dev.c_str(), "tmpfs", MS_NOSUID | MS_STRICTATIME, "mode=0755") < 0) {
                fail(status_fd, "mount-dev", errno);
            }

            static const char* const devices[] = {
                "/null", "/zero", "/full", "/random", "/urandom", "/tty",
            };
            for(const char* name : devices) {
                if(!bind_device(std::string("/dev") + name, dev + name)) {
                    if(strcmp(name, "/null") == 0)
                        fail(status_fd, "bind-dev-null", errno);
                }
            }

            const std::string pts = dev + "/pts";
            mkdir(pts.c_str(), 0755);
            if(mount("devpts", pts.c_str(), "devpts", MS_NOSUID | MS_NOEXEC, "newinstance,ptmxmode=0666,mode=0620") ==
               0) {
                const int rc = symlink("pts/ptmx", (dev + "/ptmx").c_str());
                (void)rc;  // suppress warn_unused_result
            }

            const std::string shm = dev + "/shm";
            mkdir(shm.c_str(), 01777);
            if(mount("tmpfs", shm.c_str(), "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777") < 0) {
                // Not fatal: many commands work without shared memory.
            }

            int rc = symlink("/proc/self/fd", (dev + "/fd").c_str());
            rc = symlink("/proc/self/fd/0", (dev + "/stdin").c_str());
            rc = symlink("/proc/self/fd/1", (dev + "/stdout").c_str());
            rc = symlink("/proc/self/fd/2", (dev + "/stderr").c_str());
            (void)rc;  // suppress warn_unused_result
        }

        void setup_proc(const std::string& root, int status_fd) {
            const std::string proc = root + "/proc";
            mkdir(proc.c_str(), 0555);
            if(mount("proc", proc.c_str(), "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) < 0) {
                fail(status_fd, "mount-proc", errno);
            }
        }

        // The daemon blocks several signals for its signalfd and ignores SIGPIPE for
        // its sockets.  Both signal masks and ignored dispositions survive fork() and
        // exec(), so init must restore the defaults before running the container
        // command; otherwise Ctrl-C/SIGTERM stay blocked and pipelines see EPIPE
        // instead of dying from SIGPIPE.
        void reset_signals(int status_fd) {
            sigset_t empty;
            sigemptyset(&empty);
            if(sigprocmask(SIG_SETMASK, &empty, nullptr) < 0)
                fail(status_fd, "sigprocmask", errno);

            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = SIG_DFL;
            sigemptyset(&sa.sa_mask);
            if(sigaction(SIGPIPE, &sa, nullptr) < 0)
                fail(status_fd, "sigaction-pipe", errno);
        }

        void mark_inherited_fds_cloexec() {
            long maxfd = sysconf(_SC_OPEN_MAX);
            if(maxfd < 0 || maxfd > 65536)
                maxfd = 65536;
            for(int fd = 3; fd < maxfd; ++fd) {
                int flags = fcntl(fd, F_GETFD);
                if(flags >= 0)
                    fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
            }
        }

        void setup_stdio(ChildConfig& cfg, int status_fd) {
            if(setsid() < 0)
                fail(status_fd, "setsid", errno);

            if(cfg.tty) {
                int slave = open(cfg.slave_path.c_str(), O_RDWR | O_NOCTTY);
                if(slave < 0)
                    fail(status_fd, "open-pty-slave", errno);
                ioctl(slave, TIOCSCTTY, 0);
                if(dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0 || dup2(slave, STDERR_FILENO) < 0)
                    fail(status_fd, "dup-pty", errno);
                if(slave > STDERR_FILENO)
                    close(slave);
                if(cfg.master_fd >= 0)
                    close(cfg.master_fd);
            } else {
                if(dup2(cfg.stdin_read, STDIN_FILENO) < 0 || dup2(cfg.stdout_write, STDOUT_FILENO) < 0 ||
                   dup2(cfg.stderr_write, STDERR_FILENO) < 0)
                    fail(status_fd, "dup-pipe", errno);
                if(cfg.stdin_read > STDERR_FILENO)
                    close(cfg.stdin_read);
                if(cfg.stdout_write > STDERR_FILENO)
                    close(cfg.stdout_write);
                if(cfg.stderr_write > STDERR_FILENO)
                    close(cfg.stderr_write);
                if(cfg.master_fd >= 0)
                    close(cfg.master_fd);
            }
        }

        void setup_rootfs(ChildConfig& cfg, int status_fd) {
            if(mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0)
                fail(status_fd, "make-rprivate", errno);

            if(mount(cfg.rootfs.c_str(), cfg.mountpoint.c_str(), nullptr, MS_BIND | MS_REC, nullptr) < 0)
                fail(status_fd, "bind-rootfs", errno);

            setup_proc(cfg.mountpoint, status_fd);
            setup_devices(cfg.mountpoint, status_fd);

            if(chdir(cfg.mountpoint.c_str()) < 0)
                fail(status_fd, "chdir-rootfs", errno);

            if(syscall(SYS_pivot_root, ".", ".") == 0) {
                if(umount2(".", MNT_DETACH) < 0) {
                    // The old root stays stacked but is no longer reachable by path.
                }
                if(chdir("/") < 0)
                    fail(status_fd, "chdir-new-root", errno);
            } else {
                int pivot_err = errno;
                // Fallback: chroot is weaker than pivot_root but still isolates the
                // filesystem view for the purposes of this project.
                if(chroot(cfg.mountpoint.c_str()) < 0 || chdir("/") < 0) {
                    fail(status_fd, "chroot", pivot_err);
                }
            }
        }

        bool exec_with_path(const std::vector<std::string>& argv, const std::vector<std::string>& env, int status_fd) {
            std::vector<char*> cargv;
            cargv.reserve(argv.size() + 1);
            for(const auto& a : argv)
                cargv.push_back(const_cast<char*>(a.c_str()));
            cargv.push_back(nullptr);

            std::vector<char*> cenv;
            cenv.reserve(env.size() + 1);
            for(const auto& e : env)
                cenv.push_back(const_cast<char*>(e.c_str()));
            cenv.push_back(nullptr);

            const std::string& cmd = argv[0];
            if(cmd.find('/') != std::string::npos) {
                execve(cmd.c_str(), cargv.data(), cenv.data());
                fail(status_fd, "exec", errno);
            }

            std::string path;
            if(!env_lookup(env, "PATH", path) || path.empty())
                path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
            int saved = ENOENT;
            size_t pos = 0;
            while(pos <= path.size()) {
                size_t colon = path.find(':', pos);
                std::string dir = path.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos);
                std::string candidate = dir.empty() ? cmd : join_path(dir, cmd);
                execve(candidate.c_str(), cargv.data(), cenv.data());
                if(errno == EACCES || errno == ENOEXEC || errno == EPERM)
                    saved = errno;
                if(colon == std::string::npos)
                    break;
                pos = colon + 1;
            }
            fail(status_fd, "exec", saved);
        }

    }  // namespace

    namespace {

        [[noreturn]] void container_init(ChildConfig& cfg) {
            const int status_fd = cfg.status_fd;
            reset_signals(status_fd);

            if(setresgid(0, 0, 0) < 0)
                fail(status_fd, "setgid", errno);
            if(setresuid(0, 0, 0) < 0)
                fail(status_fd, "setuid", errno);

            setup_stdio(cfg, status_fd);
            mark_inherited_fds_cloexec();

            setup_rootfs(cfg, status_fd);

            if(!cfg.hostname.empty()) {
                if(sethostname(cfg.hostname.c_str(), cfg.hostname.size()) < 0)
                    fail(status_fd, "sethostname", errno);
            }

            if(chdir(cfg.cwd.c_str()) < 0)
                fail(status_fd, "chdir-cwd", errno);

            umask(0022);
            exec_with_path(cfg.argv, cfg.env, status_fd);
            _exit(127);
        }

    }  // namespace

    int container_launcher(void* raw) {
        ChildConfig& cfg = *static_cast<ChildConfig*>(raw);
        const int status_fd = cfg.status_fd;

        char go = 0;
        ssize_t n;
        do {
            n = read(cfg.sync_fd, &go, 1);
        } while(n < 0 && errno == EINTR);
        if(n != 1)
            _exit(126);
        close(cfg.sync_fd);
        cfg.sync_fd = -1;

        if(setresgid(0, 0, 0) < 0)
            fail(status_fd, "setgid", errno);
        if(setresuid(0, 0, 0) < 0)
            fail(status_fd, "setuid", errno);

        // The user namespace already exists.  Creating the remaining namespaces
        // now makes them owned by that user namespace, which is required for
        // unprivileged mounts (proc, tmpfs, devpts, pivot_root).
        if(unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET) < 0) {
            fail(status_fd, "unshare", errno);
        }

        pid_t init_pid = fork();
        if(init_pid < 0)
            fail(status_fd, "fork-init", errno);

        if(init_pid > 0) {
            if(cfg.pid_fd >= 0) {
                int32_t pid32 = static_cast<int32_t>(init_pid);
                ssize_t ignored = write(cfg.pid_fd, &pid32, sizeof(pid32));
                (void)ignored;
                close(cfg.pid_fd);
                cfg.pid_fd = -1;
            }
            // The launcher exits immediately.  The daemon is a child subreaper,
            // so the init process is reparented to it and can be reaped normally.
            if(cfg.status_fd >= 0)
                close(cfg.status_fd);
            if(cfg.master_fd >= 0)
                close(cfg.master_fd);
            if(cfg.stdin_read >= 0)
                close(cfg.stdin_read);
            if(cfg.stdout_write >= 0)
                close(cfg.stdout_write);
            if(cfg.stderr_write >= 0)
                close(cfg.stderr_write);
            _exit(0);
        }

        if(cfg.pid_fd >= 0) {
            close(cfg.pid_fd);
            cfg.pid_fd = -1;
        }
        container_init(cfg);
    }

}  // namespace capsula
