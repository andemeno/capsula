#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace capsula {

    // Wire format: 12-byte header (prefix, type, flags, payload length), all
    // integers little-endian, followed by the payload.
    constexpr uint32_t kProtoPrefix = 0x53504143u;  // "CAPS"
    constexpr uint16_t kProtoVersion = 1;
    constexpr size_t kHeaderSize = 12;
    constexpr uint32_t kMaxPayload = 32u * 1024u * 1024u;

    enum class MsgType : uint16_t {
        Hello = 1,      // проверить версию протокола
        HelloOk = 2,    // версия протокола, поддерживаемая демоном
        Run = 10,       // запустить контейнер: RunSpec
        RunOk = 11,     // контейнер создан: id, имя, host PID, наличие TTY
        List = 20,      // запросить список контейнеров
        ListItem = 21,  // один контейнер в ответе на List
        ListEnd = 22,   // конец списка контейнеров
        Logs = 30,      // запросить лог: follow — подписка на новые записи
        LogData = 31,   // очередная порция лога
        LogEnd = 32,    // лог закончился или follow остановлен
        Attach = 40,    // подключиться к работающему контейнеру
        AttachOk = 41,  // подключение разрешено: id, PID, наличие TTY
        Kill = 50,      // отправить сигнал контейнеру
        KillOk = 51,    // результат: доставлен ли сигнал, код выхода
        Output = 70,    // вывод контейнера: flags — stdout или stderr
        Input = 71,     // ввод клиента, направляемый контейнеру
        InputEof = 72,  // конец stdin для контейнера без TTY
        Resize = 73,    // изменить размер PTY: cols, rows
        Detach = 74,    // отключить клиента, не останавливая контейнер
        Exit = 75,      // контейнер завершился: код выхода и сигнал
        Error = 90,     // ошибка обработки запроса
        Ping = 91,      // проверить доступность демона
        Pong = 92,      // ответ на Ping и подтверждение Shutdown
        Shutdown = 93,  // остановить демон
    };

    constexpr uint16_t kStreamStdout = 1;
    constexpr uint16_t kStreamStderr = 2;

    constexpr uint32_t kRunFlagTty = 1u << 0;
    constexpr uint32_t kRunFlagDetach = 1u << 1;

    constexpr uint32_t kLogsFlagFollow = 1u << 0;

    enum class ContainerState : uint8_t {
        Starting = 0,
        Running = 1,
        Exited = 2,
    };

    struct Message {
        uint16_t type = 0;
        uint16_t flags = 0;
        std::vector<uint8_t> payload;
    };

    struct RunSpec {
        std::string name;
        std::string rootfs;
        std::string hostname;
        std::string cwd = "/";
        std::vector<std::string> argv;
        std::vector<std::string> env;
        bool tty = false;
        bool detach = false;
        uint16_t cols = 80;
        uint16_t rows = 24;
    };

    struct ContainerInfo {
        std::string id;
        std::string name;
        std::string command;
        ContainerState state = ContainerState::Starting;
        int32_t pid = 0;
        int32_t exit_code = -1;
        int32_t term_signal = 0;
        uint64_t created_ms = 0;
        bool tty = false;
    };

    class Writer {
       public:
        void u8(uint8_t v);
        void u16(uint16_t v);
        void u32(uint32_t v);
        void u64(uint64_t v);
        void i32(int32_t v);
        void str(const std::string& v);
        void strlist(const std::vector<std::string>& v);
        void bytes(const void* data, size_t len);
        void bytes(const std::vector<uint8_t>& v);

        const std::vector<uint8_t>& data() const {
            return m_buf;
        }
        std::vector<uint8_t> take() {
            return std::move(m_buf);
        }

       private:
        std::vector<uint8_t> m_buf;
    };

    class Reader {
       public:
        Reader(const void* data, size_t len);
        explicit Reader(const std::vector<uint8_t>& v);

        bool ok() const {
            return m_ok;
        }
        size_t remaining() const {
            return m_ok ? m_size - m_off : 0;
        }
        bool empty() const {
            return remaining() == 0;
        }

        uint8_t u8();
        uint16_t u16();
        uint32_t u32();
        uint64_t u64();
        int32_t i32();
        std::string str();
        std::vector<std::string> strlist();
        std::vector<uint8_t> bytes(size_t len);

       private:
        bool need(size_t n);

        const uint8_t* m_p;
        size_t m_size;
        size_t m_off = 0;
        bool m_ok = true;
    };

    std::vector<uint8_t> encode_message(const Message& m);
    std::vector<uint8_t> encode_message(MsgType type, uint16_t flags, const std::vector<uint8_t>& payload);

    // Incremental parser for a non-blocking stream.
    class MessageReader {
       public:
        void feed(const void* data, size_t len);
        bool next(Message& out);
        bool failed() const {
            return m_failed;
        }
        void clear();
        size_t buffered() const {
            return m_buf.size() - m_off;
        }

       private:
        std::vector<uint8_t> m_buf;
        size_t m_off = 0;
        bool m_failed = false;
    };

    std::vector<uint8_t> encode_run(const RunSpec& spec);
    bool decode_run(const Message& m, RunSpec& spec);

    std::vector<uint8_t> encode_info(const ContainerInfo& info);
    bool decode_info(const Message& m, ContainerInfo& info);

    std::vector<uint8_t> encode_logs_request(const std::string& target, bool follow);
    bool decode_logs_request(const Message& m, std::string& target, bool& follow);

    std::vector<uint8_t> encode_attach_request(const std::string& target, uint16_t cols, uint16_t rows);
    bool decode_attach_request(const Message& m, std::string& target, uint16_t& cols, uint16_t& rows);

    std::vector<uint8_t> encode_kill_request(const std::string& target, int signal, bool wait);
    bool decode_kill_request(const Message& m, std::string& target, int& signal, bool& wait);

    std::vector<uint8_t> encode_exit(int32_t code, int32_t term_signal);
    bool decode_exit(const Message& m, int32_t& code, int32_t& term_signal);

    std::vector<uint8_t> encode_error(uint32_t code, const std::string& text);
    bool decode_error(const Message& m, uint32_t& code, std::string& text);

    std::string state_name(ContainerState s);

}  // namespace capsula
