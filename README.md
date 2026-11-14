# capsula

`capsula` — учебный проект системы контейнеризации.

## Требования

Для сборки требуется CMake >= 3.16 и компилятор с поддержкой C++17.
Для тестов: `curl` или `wget`, `util-linux` (утилита `script`).

## Сборка
```sh
cmake -S . -B build
cmake --build build
```
В результате получаем исполняемый файл `build/capsula`.

## Запуск и работа
Создать корневую файловую систему из Alpine minirootfs:
```sh
tests/make_rootfs.sh /tmp/capsula-rootfs
```
Скрипт скачивает `alpine-minirootfs-<версия>-<архитектура>.tar.gz` с `dl-cdn.alpinelinux.org`, проверяет sha256 и кэширует архив 
(по умолчанию в `~/.cache/capsula`). Повторные запуски используют кэш. Источник можно переопределить:
```sh
# Локальный архив — без сети.
CAPSULA_ALPINE_ROOTFS=/path/to/alpine-minirootfs.tar.gz \
    tests/make_rootfs.sh /tmp/capsula-rootfs

# Другая ветка или архитектура.
ALPINE_VERSION=v3.23 ALPINE_ARCH=aarch64 tests/make_rootfs.sh /tmp/capsula-rootfs

# Явный URL или каталог кэша.
CAPSULA_ALPINE_URL=https://example.org/alpine-minirootfs.tar.gz \
CAPSULA_ALPINE_CACHE=/var/tmp/capsula-cache \
    tests/make_rootfs.sh /tmp/capsula-rootfs
```
Запуск контейнера:
```sh
./build/capsula run --rootfs /tmp/capsula-rootfs -- \
    /bin/sh -c 'echo hello from capsula; id -u'
```
Интерактивная оболочка через псевдотерминал:
```sh
./build/capsula run -it --rootfs /tmp/capsula-rootfs -- /bin/sh
```
Detached-контейнер и работа с логами:
```sh
./build/capsula run -d --name demo --rootfs /tmp/capsula-rootfs -- \
    /bin/sh -c 'echo started; while :; do sleep 1; done'

./build/capsula list
./build/capsula logs demo
./build/capsula logs -f demo
./build/capsula attach demo
./build/capsula kill demo
./build/capsula kill -s KILL demo
```

Демон запускается автоматически при первой клиентской команде и продолжает работать в фоне. Чтобы остановить его, следует выполнить:
```sh
./build/capsula daemon --stop
```

## Аргументы командной строки

```text
capsula run [опции] [--] КОМАНДА [АРГУМЕНТЫ...]
  -d, --detach          запустить в фоне
  -t, --tty             выделить псевдотерминал
  -i, --interactive     оставить stdin подключённым (по умолчанию)
      --name NAME       имя контейнера
      --rootfs PATH     корневая файловая система (обязательно)
      --hostname NAME   hostname в UTS namespace
      --cwd PATH        рабочий каталог (по умолчанию /)
      --env KEY=VALUE   задать переменную окружения

capsula list [--running]          # псевдоним: ps
capsula logs [-f] CONTAINER
capsula attach CONTAINER
capsula kill [-s SIGNAL] CONTAINER|PID
capsula daemon [--foreground] [--socket PATH] [--runtime DIR] [--log FILE]
capsula daemon --stop
capsula version
```
К контейнерам можно ображаться по id, по имени и по PID на хосте. 

### Переменные окружения

`CAPSULA_RUNTIME_DIR` - каталог состояния. По умолчанию `$XDG_RUNTIME_DIR/capsula` или `/tmp/capsula-$UID`.
`CAPSULA_SOCKET` - путь к сокету демона. По умолчанию `$CAPSULA_RUNTIME_DIR/capsula.sock`.
`CAPSULA_NO_AUTOSTART` - если задана, клиент не запускает демона автоматически.

Структура каталога состояния:

```text
<runtime>/
  capsula.sock
  daemon.lock
  daemon.pid
  daemon.log
  containers/<id>/log
  roots/<id>/
```
Логи контейнеров лежат в `<runtime>/containers/<id>/log`.


### Демон

Демон однопоточный и использует `poll()` для обмена данными с клиентами, контейнерами, получения сигналов.
Он управляет жизненным циклом контейнеров, завершённые контейнеры видны в списке (list) до остановки демона, пишет вывод в логи (отдельно для каждого контейнера) и рассылает его подключённым клиентам и подписчикам логов. Демон использует `daemon.lock`, чтобы одновременно работал только один экземпляр процесса.

Демонизация процесса выполняется в два этапа:
 1. Выполняется fork(0) и в порожденном процессе вызов setsid(), отвязывая его процесс от терминала родителя и делаем его лидером нового сеанса. Вызов setsid() важен, потому что у демонов не должно быть управляющих терминалов. Если демон запускается из командной оболочки, он подвергается воздействию сигналов от управляющего терминала пользователя, которые могут спровоцировать его неожиданное завершение.
 2. В порожденном процессе выполняется ещё один fork() - порожденный этим вызовом процесс (внук исходного) является настоящим демоном, так как у него нет управлящего терминала и он не может стать лидером сеанса, чтобы получить управляющий терминал (так как порождён лидером сеанса).

### Протокол Клиент-Демон

Клиент и демон обмениваются бинарными сообщениями с префиксом длины через Unix-сокет:
```text
12-байтовый заголовок:
  prefix "CAPS" (little-endian 0x53504143)
  uint16 type
  uint16 flags
  uint32 payload length
payload: поля конкретного сообщения, все целые — little-endian
```
Строки передаются как длина + байты, списки - как количество + строки.

Протокол поддерживает сообщения:
 - Hello - проверить версию протокола
 - HelloOk - версия протокола, поддерживаемая демоном
 - Run - запустить контейнер: RunSpec
 - RunOk - контейнер создан: id, имя, host PID, наличие TTY
 - List - запросить список контейнеров
 - ListItem - один контейнер в ответе на List
 - ListEnd - конец списка контейнеров
 - Logs - запросить лог: follow — подписка на новые записи
 - LogData - очередная порция лога
 - LogEnd - лог закончился или follow остановлен
 - Attach - подключиться к работающему контейнеру
 - AttachOk - подключение разрешено: id, PID, наличие TTY
 - Kill - отправить сигнал контейнеру
 - KillOk -результат: доставлен ли сигнал, код выхода
 - Output - вывод контейнера: flags — stdout или stderr
 - Input - ввод клиента, направляемый контейнеру
 - InputEof - конец stdin для контейнера без TTY
 - Resize - изменить размер PTY: cols, rows
 - Detach - отключить клиента, не останавливая контейнер
 - Exit - контейнер завершился: код выхода и сигнал
 - Error - ошибка обработки запроса
 - Ping - проверить доступность демона
 - Pong - ответ на Ping и подтверждение Shutdown
 - Shutdown - остановить демон

## Тесты

Тесты собирают тестовый rootfs из Alpine minirootfs (`tests/make_rootfs.sh`), поэтому при первом запуске нужен доступ к `dl-cdn.alpinelinux.org` или заранее скачанный архив в `CAPSULA_ALPINE_ROOTFS`.
```sh
ctest --test-dir build --output-on-failure
```
Некоторые дистрибутивы ограничивают unprivileged user namespace'ы или монтирование через AppArmor/SELinux. На таких хостах интеграционные тесты завершаются с кодом `77`, который CTest показывает как `SKIP`. Для обхода этого ограничения следует выполнить команду:
```sh
sysctl kernel.apparmor_restrict_unprivileged_userns=0
```
