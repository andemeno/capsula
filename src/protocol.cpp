#include "capsula/protocol.hpp"

#include <cstring>

namespace capsula {

    namespace {

        void put_u16(std::vector<uint8_t>& b, uint16_t v) {
            b.push_back(static_cast<uint8_t>(v & 0xff));
            b.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
        }

        void put_u32(std::vector<uint8_t>& b, uint32_t v) {
            for(int i = 0; i < 4; ++i)
                b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
        }

        uint16_t get_u16(const uint8_t* p) {
            return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
        }

        uint32_t get_u32(const uint8_t* p) {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }

    }  // namespace

    void Writer::u8(uint8_t v) {
        m_buf.push_back(v);
    }

    void Writer::u16(uint16_t v) {
        put_u16(m_buf, v);
    }

    void Writer::u32(uint32_t v) {
        put_u32(m_buf, v);
    }

    void Writer::u64(uint64_t v) {
        put_u32(m_buf, static_cast<uint32_t>(v & 0xffffffffu));
        put_u32(m_buf, static_cast<uint32_t>((v >> 32) & 0xffffffffu));
    }

    void Writer::i32(int32_t v) {
        u32(static_cast<uint32_t>(v));
    }

    void Writer::str(const std::string& v) {
        u32(static_cast<uint32_t>(v.size()));
        bytes(v.data(), v.size());
    }

    void Writer::strlist(const std::vector<std::string>& v) {
        u32(static_cast<uint32_t>(v.size()));
        for(const auto& s : v)
            str(s);
    }

    void Writer::bytes(const void* data, size_t len) {
        const auto* p = static_cast<const uint8_t*>(data);
        m_buf.insert(m_buf.end(), p, p + len);
    }

    void Writer::bytes(const std::vector<uint8_t>& v) {
        m_buf.insert(m_buf.end(), v.begin(), v.end());
    }

    Reader::Reader(const void* data, size_t len) : m_p(static_cast<const uint8_t*>(data)), m_size(len) {
    }

    Reader::Reader(const std::vector<uint8_t>& v) : m_p(v.data()), m_size(v.size()) {
    }

    bool Reader::need(size_t n) {
        if(!m_ok)
            return false;
        if(m_off + n > m_size || m_off + n < m_off) {
            m_ok = false;
            return false;
        }
        return true;
    }

    uint8_t Reader::u8() {
        if(!need(1))
            return 0;
        return m_p[m_off++];
    }

    uint16_t Reader::u16() {
        if(!need(2))
            return 0;
        uint16_t v = get_u16(m_p + m_off);
        m_off += 2;
        return v;
    }

    uint32_t Reader::u32() {
        if(!need(4))
            return 0;
        uint32_t v = get_u32(m_p + m_off);
        m_off += 4;
        return v;
    }

    uint64_t Reader::u64() {
        uint32_t lo = u32();
        uint32_t hi = u32();
        return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    }

    int32_t Reader::i32() {
        return static_cast<int32_t>(u32());
    }

    std::string Reader::str() {
        uint32_t len = u32();
        if(len > kMaxPayload || !need(len))
            return std::string();
        std::string s(reinterpret_cast<const char*>(m_p + m_off), len);
        m_off += len;
        return s;
    }

    std::vector<std::string> Reader::strlist() {
        std::vector<std::string> out;
        uint32_t n = u32();
        if(!m_ok || n > 65536) {
            m_ok = false;
            return out;
        }
        out.reserve(n);
        for(uint32_t i = 0; i < n && m_ok; ++i)
            out.push_back(str());
        return out;
    }

    std::vector<uint8_t> Reader::bytes(size_t len) {
        if(len > kMaxPayload || !need(len))
            return std::vector<uint8_t>();
        std::vector<uint8_t> out(m_p + m_off, m_p + m_off + len);
        m_off += len;
        return out;
    }

    std::vector<uint8_t> encode_message(const Message& m) {
        return encode_message(static_cast<MsgType>(m.type), m.flags, m.payload);
    }

    std::vector<uint8_t> encode_message(MsgType type, uint16_t flags, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> out;
        out.reserve(kHeaderSize + payload.size());
        put_u32(out, kProtoPrefix);
        put_u16(out, static_cast<uint16_t>(type));
        put_u16(out, flags);
        put_u32(out, static_cast<uint32_t>(payload.size()));
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    void MessageReader::feed(const void* data, size_t len) {
        if(m_failed)
            return;
        const auto* p = static_cast<const uint8_t*>(data);
        m_buf.insert(m_buf.end(), p, p + len);
    }

    bool MessageReader::next(Message& out) {
        if(m_failed)
            return false;
        const size_t avail = m_buf.size() - m_off;
        if(avail < kHeaderSize)
            return false;
        const uint8_t* p = m_buf.data() + m_off;
        if(get_u32(p) != kProtoPrefix) {
            m_failed = true;
            return false;
        }
        const uint16_t type = get_u16(p + 4);
        const uint16_t flags = get_u16(p + 6);
        const uint32_t len = get_u32(p + 8);
        if(len > kMaxPayload) {
            m_failed = true;
            return false;
        }
        if(avail < kHeaderSize + len)
            return false;
        out.type = type;
        out.flags = flags;
        out.payload.assign(p + kHeaderSize, p + kHeaderSize + len);
        m_off += kHeaderSize + len;
        if(m_off > 65536 || m_off == m_buf.size()) {
            m_buf.clear();
            m_off = 0;
        }
        return true;
    }

    void MessageReader::clear() {
        m_buf.clear();
        m_off = 0;
        m_failed = false;
    }

    std::vector<uint8_t> encode_run(const RunSpec& s) {
        Writer w;
        uint32_t flags = 0;
        if(s.tty)
            flags |= kRunFlagTty;
        if(s.detach)
            flags |= kRunFlagDetach;
        w.u32(flags);
        w.str(s.name);
        w.str(s.rootfs);
        w.str(s.hostname);
        w.str(s.cwd);
        w.strlist(s.argv);
        w.strlist(s.env);
        w.u16(s.cols);
        w.u16(s.rows);
        return encode_message(MsgType::Run, 0, w.data());
    }

    bool decode_run(const Message& m, RunSpec& s) {
        Reader r(m.payload);
        uint32_t flags = r.u32();
        s.name = r.str();
        s.rootfs = r.str();
        s.hostname = r.str();
        s.cwd = r.str();
        s.argv = r.strlist();
        s.env = r.strlist();
        s.cols = r.u16();
        s.rows = r.u16();
        s.tty = (flags & kRunFlagTty) != 0;
        s.detach = (flags & kRunFlagDetach) != 0;
        return r.ok() && r.empty() && !s.argv.empty();
    }

    std::vector<uint8_t> encode_info(const ContainerInfo& i) {
        Writer w;
        w.str(i.id);
        w.str(i.name);
        w.str(i.command);
        w.u8(static_cast<uint8_t>(i.state));
        w.i32(i.pid);
        w.i32(i.exit_code);
        w.i32(i.term_signal);
        w.u64(i.created_ms);
        w.u8(i.tty ? 1 : 0);
        return encode_message(MsgType::ListItem, 0, w.data());
    }

    bool decode_info(const Message& m, ContainerInfo& i) {
        Reader r(m.payload);
        i.id = r.str();
        i.name = r.str();
        i.command = r.str();
        i.state = static_cast<ContainerState>(r.u8());
        i.pid = r.i32();
        i.exit_code = r.i32();
        i.term_signal = r.i32();
        i.created_ms = r.u64();
        i.tty = r.u8() != 0;
        return r.ok() && r.empty();
    }

    std::vector<uint8_t> encode_logs_request(const std::string& target, bool follow) {
        Writer w;
        w.str(target);
        w.u32(follow ? kLogsFlagFollow : 0);
        return encode_message(MsgType::Logs, 0, w.data());
    }

    bool decode_logs_request(const Message& m, std::string& target, bool& follow) {
        Reader r(m.payload);
        target = r.str();
        follow = (r.u32() & kLogsFlagFollow) != 0;
        return r.ok() && r.empty();
    }

    std::vector<uint8_t> encode_attach_request(const std::string& target, uint16_t cols, uint16_t rows) {
        Writer w;
        w.str(target);
        w.u16(cols);
        w.u16(rows);
        return encode_message(MsgType::Attach, 0, w.data());
    }

    bool decode_attach_request(const Message& m, std::string& target, uint16_t& cols, uint16_t& rows) {
        Reader r(m.payload);
        target = r.str();
        cols = r.u16();
        rows = r.u16();
        return r.ok() && r.empty();
    }

    std::vector<uint8_t> encode_kill_request(const std::string& target, int signal, bool wait) {
        Writer w;
        w.str(target);
        w.i32(signal);
        w.u8(wait ? 1 : 0);
        return encode_message(MsgType::Kill, 0, w.data());
    }

    bool decode_kill_request(const Message& m, std::string& target, int& signal, bool& wait) {
        Reader r(m.payload);
        target = r.str();
        signal = r.i32();
        wait = r.u8() != 0;
        return r.ok() && r.empty();
    }

    std::vector<uint8_t> encode_exit(int32_t code, int32_t term_signal) {
        Writer w;
        w.i32(code);
        w.i32(term_signal);
        return encode_message(MsgType::Exit, 0, w.data());
    }

    bool decode_exit(const Message& m, int32_t& code, int32_t& term_signal) {
        Reader r(m.payload);
        code = r.i32();
        term_signal = r.i32();
        return r.ok() && r.empty();
    }

    std::vector<uint8_t> encode_error(uint32_t code, const std::string& text) {
        Writer w;
        w.u32(code);
        w.str(text);
        return encode_message(MsgType::Error, 0, w.data());
    }

    bool decode_error(const Message& m, uint32_t& code, std::string& text) {
        Reader r(m.payload);
        code = r.u32();
        text = r.str();
        return r.ok() && r.empty();
    }

    std::string state_name(ContainerState s) {
        switch(s) {
            case ContainerState::Starting:
                return "starting";
            case ContainerState::Running:
                return "running";
            case ContainerState::Exited:
                return "exited";
        }
        return "unknown";
    }

}  // namespace capsula
