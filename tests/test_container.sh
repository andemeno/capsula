#!/bin/sh
# Интеграционные тесты контейнеризации capsula.
set -eu

BIN=${1:?usage: test_container.sh /path/to/capsula /path/to/source}
SRC=${2:?usage: test_container.sh /path/to/capsula /path/to/source}

TMP=$(mktemp -d /tmp/capsula-container.XXXXXX)
RUNTIME=$TMP/runtime
export CAPSULA_RUNTIME_DIR=$RUNTIME
export CAPSULA_SOCKET=$RUNTIME/capsula.sock
ROOTFS=$TMP/rootfs

cleanup() {
    "$BIN" daemon --stop >/dev/null 2>&1 || true
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

fail() {
    echo "FAIL: $*" >&2
    echo "--- daemon log ---" >&2
    sed -n '1,200p' "$RUNTIME/daemon.log" >&2 2>/dev/null || true
    exit 1
}

"$SRC/tests/make_rootfs.sh" "$ROOTFS" >/dev/null
printf 'capsula-root-marker\n' > "$ROOTFS/capsula-marker"

# Проверяем доступность привилегий: на некоторых хостах непривилегированные
# user namespace'ы ограничены.
if probe=$("$BIN" run --rootfs "$ROOTFS" -- /bin/true 2>&1); then
    :
else
    case "$probe" in
        *"Permission denied"*|*"Operation not permitted"*|*"clone failed"*|*"make-rprivate"*|*"bind-rootfs"*)
            echo "SKIP: namespaces are not usable here: $probe"
            exit 77
            ;;
        *)
            fail "capability probe failed: $probe"
            ;;
    esac
fi

# 1. Вывод команды.
out=$("$BIN" run --rootfs "$ROOTFS" -- /bin/sh -c 'echo hello; echo world')
[ "$out" = "$(printf 'hello\nworld')" ] || fail "unexpected output: $out"

# 2. Проброс кода выхода.
set +e
"$BIN" run --rootfs "$ROOTFS" -- /bin/sh -c 'exit 42'
rc=$?
set -e
[ "$rc" -eq 42 ] || fail "exit code: expected 42, got $rc"

# 3. Команда является PID 1 в новом PID namespace.
out=$("$BIN" run --rootfs "$ROOTFS" -- /bin/sh -c 'echo $$')
[ "$out" = "1" ] || fail "PID inside container: expected 1, got $out"

# 4. Используется заданный rootfs.
"$BIN" run --rootfs "$ROOTFS" -- /usr/bin/test -f /capsula-marker ||
    fail "rootfs marker is not visible"

# 5. Смонтированы /proc и /dev.
"$BIN" run --rootfs "$ROOTFS" -- /bin/sh -c \
    'test -r /proc/self/stat && test -r /proc/1/status && test -c /dev/null && test -c /dev/zero && echo dev-ok >/dev/null' ||
    fail "/proc or /dev is not usable"
"$BIN" run --rootfs "$ROOTFS" -- /usr/bin/awk \
    '$2 == "/proc" && $3 == "proc" { found = 1 } END { exit !found }' /proc/self/mounts ||
    fail "/proc is not a procfs mount"

# 5b. Заблокированные демоном сигналы и проигнорированный SIGPIPE не должны
# утекать в контейнер: иначе Ctrl-C/SIGTERM останутся заблокированными, а
# процессы в pipeline получат EPIPE.
out=$("$BIN" run --rootfs "$ROOTFS" -- /bin/sh -c \
    'grep "^SigBlk:" /proc/self/status')
case "$out" in
    *"0000000000000000"*) : ;;
    *) fail "blocked signal mask leaked into the container: $out" ;;
esac
out=$("$BIN" run --rootfs "$ROOTFS" -- /bin/sh -c 'yes | head -n 1' 2>&1)
[ "$out" = "y" ] || fail "SIGPIPE disposition leaked into the container: $out"

# 6. Hostname в UTS namespace.
out=$("$BIN" run --rootfs "$ROOTFS" --hostname capsula-test -- /bin/hostname)
[ "$out" = "capsula-test" ] || fail "hostname: expected capsula-test, got $out"

# 7. User namespace отображает root контейнера на вызывающего пользователя.
out=$("$BIN" run --rootfs "$ROOTFS" -- /usr/bin/id -u)
[ "$out" = "0" ] || fail "container uid: expected 0, got $out"

# 8. Каждый запрошенный namespace является новым.
for ns in pid mnt uts ipc net user; do
    host_ns=$(readlink "/proc/self/ns/$ns")
    guest_ns=$("$BIN" run --rootfs "$ROOTFS" -- /usr/bin/readlink "/proc/self/ns/$ns")
    [ -n "$guest_ns" ] || fail "cannot read /proc/self/ns/$ns in container"
    [ "$host_ns" != "$guest_ns" ] ||
        fail "namespace $ns was not isolated ($host_ns)"
done

# 9. Изменения mount namespace не утекают на хост.
"$BIN" run --rootfs "$ROOTFS" -- /bin/sh -c \
    'mount -t tmpfs tmpfs /tmp && echo isolated > /tmp/isolated' ||
    fail "cannot mount tmpfs inside container"
[ ! -e "$ROOTFS/tmp/isolated" ] || fail "container mount leaked to the host"

# 10. stdout и stderr остаются раздельными для запуска без PTY.
"$BIN" run --rootfs "$ROOTFS" -- /bin/sh -c 'echo out; echo err >&2' \
    >"$TMP/stdout" 2>"$TMP/stderr"
[ "$(cat "$TMP/stdout")" = "out" ] || fail "stdout was not captured correctly"
[ "$(cat "$TMP/stderr")" = "err" ] || fail "stderr was not captured correctly"

# 11. stdin пробрасывается для запуска без PTY.
out=$(printf 'piped-input\n' | "$BIN" run --rootfs "$ROOTFS" -- /bin/cat)
[ "$out" = "piped-input" ] || fail "stdin forwarding failed: $out"

echo "container integration tests passed"
