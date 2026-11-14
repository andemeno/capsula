#!/bin/sh
# Интеграционные тесты протокола демона capsula (list/logs/attach/kill).
set -eu

BIN=${1:?usage: test_daemon.sh /path/to/capsula /path/to/source}
SRC=${2:?usage: test_daemon.sh /path/to/capsula /path/to/source}

TMP=$(mktemp -d /tmp/capsula-daemon.XXXXXX)
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
    sed -n '1,250p' "$RUNTIME/daemon.log" >&2 2>/dev/null || true
    exit 1
}

"$SRC/tests/make_rootfs.sh" "$ROOTFS" >/dev/null

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

# Вспомогательные функции для разбора вывода list.
list() { "$BIN" list; }
name_state() { list | awk -v n="$1" '$6 == n { print $3; exit }'; }
name_pid() { list | awk -v n="$1" '$6 == n { print $2; exit }'; }
name_exit() { list | awk -v n="$1" '$6 == n { print $4; exit }'; }

# Ждём нужного состояния контейнера не более 5 секунд (100 попыток по 50 мс).
wait_for_state() {
    name=$1
    wanted=$2
    i=0
    while [ "$i" -lt 100 ]; do
        if [ "$(name_state "$name")" = "$wanted" ]; then
            return 0
        fi
        i=$((i + 1))
        sleep 0.05
    done
    return 1
}

# 1. Фоновый запуск, отображение в list, проверка PID и снимок лога.
id=$("$BIN" run -d --name sleeper --rootfs "$ROOTFS" -- \
    /bin/sh -c 'echo started; sleep 30')
[ -n "$id" ] || fail "detached run returned no id"
wait_for_state sleeper running || fail "sleeper is not running"
list | grep -q sleeper || fail "list does not show sleeper"
pid=$(name_pid sleeper)
case "$pid" in
    ''|*[!0-9]*) fail "list returned an invalid pid: $pid" ;;
esac
kill -0 "$pid" 2>/dev/null || fail "container pid $pid is not alive"
sleep 0.2
"$BIN" logs sleeper | grep -q started || fail "logs snapshot is missing output"

# 2. Чтение лога контейнера в реальном времени (logs -f).
"$BIN" run -d --name follower --rootfs "$ROOTFS" -- \
    /bin/sh -c 'echo first; sleep 1; echo second' >/dev/null
"$BIN" logs -f follower >"$TMP/follow.log" &
follow_pid=$!
wait "$follow_pid" || fail "logs -f failed"
grep -q first "$TMP/follow.log" || fail "logs -f missed first"
grep -q second "$TMP/follow.log" || fail "logs -f missed second"
wait_for_state follower exited || fail "follower did not exit"

# 3a. Завершение контейнера по SIGKILL (код возврата 137).
"$BIN" kill -s KILL "$pid" >/dev/null || fail "kill by pid failed"
wait_for_state sleeper exited || fail "sleeper did not exit after kill"
[ "$(name_exit sleeper)" = "137" ] ||
    fail "sleeper exit code after SIGKILL: expected 137, got $(name_exit sleeper)"

# 3b. Завершение контейнера по SIGTERM (код возврата 7).
"$BIN" run -d --name term --rootfs "$ROOTFS" -- \
    /bin/sh -c 'trap "exit 7" TERM; while :; do sleep 1; done' >/dev/null
wait_for_state term running || fail "term container did not start"
"$BIN" kill term >/dev/null || fail "kill by name failed"
wait_for_state term exited || fail "term container did not exit after SIGTERM"
[ "$(name_exit term)" = "7" ] ||
    fail "SIGTERM trap exit code: expected 7, got $(name_exit term)"

# 4. Подключение к терминалу работающего контейнера.
"$BIN" run -d -t --name shell --rootfs "$ROOTFS" -- /bin/sh >/dev/null
wait_for_state shell running || fail "shell container did not start"
if ! command -v script >/dev/null 2>&1; then
    echo "SKIP: util-linux script is required for attach tests" >&2
    exit 77
fi
printf 'echo ATTACH_OK\nexit\n' |
    script -qec "$BIN attach shell" /dev/null >"$TMP/attach.log" 2>&1 ||
    fail "attach command failed"
grep -q ATTACH_OK "$TMP/attach.log" || fail "attach did not run the command"
wait_for_state shell exited || fail "shell did not exit after attach"

# 5. Ctrl-] отсоединяет от PTY, не останавливая контейнер.
"$BIN" run -d -t --name shell2 --rootfs "$ROOTFS" -- /bin/sh >/dev/null
wait_for_state shell2 running || fail "shell2 container did not start"
printf 'echo BEFORE_DETACH\n\035' |
    script -qec "$BIN attach shell2" /dev/null >"$TMP/detach.log" 2>&1 ||
    fail "detach command failed"
grep -q BEFORE_DETACH "$TMP/detach.log" || fail "command output was not seen"
sleep 0.2
[ "$(name_state shell2)" = "running" ] ||
    fail "Ctrl-] stopped the container instead of detaching"
"$BIN" kill -s KILL shell2 >/dev/null
wait_for_state shell2 exited || fail "shell2 did not exit after kill"

# 6. Интерактивный запуск через PTY клиента.
printf 'echo RUN_IT\nexit\n' |
    script -qec "$BIN run -it --rootfs $ROOTFS -- /bin/sh" /dev/null \
    >"$TMP/run_it.log" 2>&1 ||
    fail "interactive run failed"
grep -q RUN_IT "$TMP/run_it.log" || fail "interactive run output not found"

# 7. Подключение к завершённому контейнеру отклоняется.
set +e
attach_err=$("$BIN" attach shell 2>&1 >/dev/null)
attach_rc=$?
set -e
[ "$attach_rc" -ne 0 ] || fail "attach to an exited container should fail"
case "$attach_err" in
    *"not running"*) : ;;
    *) fail "attach error message is missing or malformed: $attach_err" ;;
esac

echo "daemon protocol tests passed"
