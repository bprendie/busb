#include "core.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>

namespace busb {
namespace {
std::string read_attr(const std::filesystem::path &path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    for (auto &c : s)
        if (static_cast<unsigned char>(c) < 32)
            c = ' ';
    return s;
}
int number(const std::string &s, int base = 10) {
    try {
        return std::stoi(s, nullptr, base);
    } catch (...) {
        return -1;
    }
}
unsigned word(const std::array<uint8_t, 8> &s, int pos) { return s[pos] | (unsigned(s[pos + 1]) << 8); }
template <class T> void bounded_push(std::deque<T> &q, T value, size_t limit) {
    q.push_back(std::move(value));
    if (q.size() > limit)
        q.pop_front();
}
void count(Counters &c, const Event &e, double latency) {
    ++c.transfers;
    if (e.h.status < 0) {
        ++c.errors;
        c.pending_error = e.h.status;
    }
    // Only callback lengths represent actual I/O. S lengths are requested sizes.
    if (e.h.type == 'C')
        (e.input() ? c.in : c.out) += e.h.length;
    if (latency >= 0) {
        c.latency_ms += latency;
        c.max_latency_ms = std::max(c.max_latency_ms, latency);
        c.pending_window_max = std::max(c.pending_window_max, latency);
        ++c.paired;
    }
}
double parse_speed(const std::string &speed) {
    double value = 0;
    auto [ptr, ec] = std::from_chars(speed.data(), speed.data() + speed.size(), value);
    return ec == std::errc{} ? value : 0;
}
const char *usb_class_name(int code) {
    switch (code) {
    case 0x01:
        return "Audio";
    case 0x02:
        return "CDC";
    case 0x03:
        return "HID";
    case 0x06:
        return "Image";
    case 0x07:
        return "Printer";
    case 0x08:
        return "Mass Storage";
    case 0x09:
        return "Hub";
    case 0x0a:
        return "CDC-Data";
    case 0x0b:
        return "Smart Card";
    case 0x0e:
        return "Video";
    case 0x0f:
        return "Healthcare";
    case 0x10:
        return "Audio/Video";
    case 0xdc:
        return "Diagnostic";
    case 0xe0:
        return "Wireless";
    case 0xef:
        return "Misc";
    case 0xfe:
        return "Application";
    case 0xff:
        return "Vendor";
    default:
        return "";
    }
}
void add_unique(std::vector<std::string> &names, const std::string &name) {
    if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end())
        names.push_back(name);
}
const char *kinds[] = {"ISO", "INTR", "CTRL", "BULK"};
std::vector<uint8_t> preview_bytes(const Event &e) {
    // Keep only the visible preview, without retaining a full snaplen buffer per pending URB.
    auto offset = e.h.transfer == 0 ? std::min<uint64_t>(e.data.size(), uint64_t(e.h.descriptors) * 16) : 0;
    auto end = std::min<uint64_t>(e.data.size(), offset + 32);
    return {e.data.begin() + offset, e.data.begin() + end};
}
} // namespace

std::vector<Device> scan_devices(const std::filesystem::path &root) {
    std::vector<Device> result;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
        auto p = entry.path();
        const auto port = p.filename().string();
        if (port.find(':') != std::string::npos || port.starts_with("usb"))
            continue;
        Device d;
        d.port = port;
        d.bus = number(read_attr(p / "busnum"));
        d.address = number(read_attr(p / "devnum"));
        if (d.bus < 1 || d.address < 1)
            continue;
        d.vendor = read_attr(p / "idVendor");
        d.product = read_attr(p / "idProduct");
        d.name = read_attr(p / "product");
        if (d.name.empty())
            d.name = read_attr(p / "manufacturer");
        bool named = !d.name.empty();
        if (!named)
            d.name = "USB device";
        d.serial = read_attr(p / "serial");
        d.speed = read_attr(p / "speed");
        d.megabits = parse_speed(d.speed);
        if (number(read_attr(p / "bDeviceClass"), 16) == 0x09)
            d.hub = true;
        std::vector<std::string> classes, ifaces;
        bool rockusb = false;
        for (const auto &iface : std::filesystem::directory_iterator(p, ec)) {
            auto ip = iface.path();
            int cls = number(read_attr(ip / "bInterfaceClass"), 16);
            if (cls < 0)
                continue;
            int sub = number(read_attr(ip / "bInterfaceSubClass"), 16);
            int proto = number(read_attr(ip / "bInterfaceProtocol"), 16);
            int n = number(read_attr(ip / "bInterfaceNumber"), 16);
            if (cls == 0x09)
                d.hub = true;
            bool dfu = cls == 0xfe && sub == 1;
            if (dfu && n >= 0)
                d.dfu_interfaces.push_back(n);
            // U-Boot f_rockusb: class ff, subclass 06, protocol 05. Rockchip calls this DFU.
            bool rockusb_if = cls == 0xff && sub == 0x06 && proto == 0x05;
            rockusb = rockusb || rockusb_if;
            std::string name = dfu ? "DFU" : (rockusb_if ? "Rockusb" : usb_class_name(cls));
            add_unique(classes, name);
            if (n >= 0 && !name.empty())
                ifaces.push_back(std::to_string(n) + " " + name);
        }
        if (!named && rockusb)
            d.name = d.vendor == "2207" ? "Rockchip Rockusb" : "Rockusb " + d.vendor + ":" + d.product;
        if (d.hub)
            add_unique(classes, "Hub");
        for (size_t i = 0; i < classes.size(); ++i) {
            if (i)
                d.class_summary += " · ";
            d.class_summary += classes[i];
        }
        for (size_t i = 0; i < ifaces.size(); ++i) {
            if (i)
                d.interfaces_text += " · ";
            d.interfaces_text += ifaces[i];
        }
        result.push_back(std::move(d));
    }
    return result;
}
std::string human(double n) {
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    while (n >= 1024 && unit < 4) {
        n /= 1024;
        ++unit;
    }
    std::ostringstream s;
    s << std::fixed << std::setprecision(unit ? 1 : 0) << n << ' ' << units[unit];
    return s.str();
}
std::string status_name(int status) {
    if (status >= 0)
        return "OK";
    switch (-status) {
    case EPIPE:
        return "STALL";
    case ETIMEDOUT:
        return "TIMEOUT";
    case ENODEV:
        return "NO_DEVICE";
    case EPROTO:
        return "PROTOCOL";
    case EILSEQ:
        return "CRC/SEQ";
    case EOVERFLOW:
        return "OVERFLOW";
    case ENOENT:
        return "CANCELLED";
    case ECONNRESET:
        return "UNLINKED";
    case ESHUTDOWN:
        return "SHUTDOWN";
    default:
        return std::to_string(status);
    }
}
std::string hex_preview(const std::vector<uint8_t> &data, size_t limit) {
    std::ostringstream s;
    for (size_t i = 0; i < std::min(limit, data.size()); ++i) {
        if (i)
            s << ' ';
        s << std::hex << std::setw(2) << std::setfill('0') << unsigned(data[i]);
    }
    if (data.size() > limit)
        s << " ...";
    return s.str();
}
std::string decode_setup(const Header &h, const Device *d, int forced) {
    if (h.transfer != 2 || h.type != 'S' || h.setup_flag != 0)
        return {};
    const auto &s = h.setup;
    unsigned value = word(s, 2), iface = word(s, 4), length = word(s, 6);
    bool known = (forced >= 0 && iface == unsigned(forced)) ||
                 (d && std::find(d->dfu_interfaces.begin(), d->dfu_interfaces.end(), int(iface)) !=
                           d->dfu_interfaces.end());
    bool input = s[0] == 0xa1;
    bool dfu =
        known && (s[0] == 0x21 || input) && s[1] <= 6 && (input == (s[1] == 2 || s[1] == 3 || s[1] == 5));
    std::ostringstream out;
    if (dfu) {
        static const char *names[] = {"DETACH",    "DNLOAD",   "UPLOAD", "GETSTATUS",
                                      "CLRSTATUS", "GETSTATE", "ABORT"};
        out << "DFU_" << names[s[1]] << " if=" << iface;
        if (s[1] == 1 || s[1] == 2)
            out << " block=" << value << " len=" << length;
        if (s[1] == 1 && length == 0)
            out << " (manifest)";
        if (s[1] == 0)
            out << " timeout=" << value << "ms";
    } else {
        static const std::map<int, std::string> names = {
            {0, "GET_STATUS"},     {1, "CLEAR_FEATURE"},  {3, "SET_FEATURE"},       {5, "SET_ADDRESS"},
            {6, "GET_DESCRIPTOR"}, {7, "SET_DESCRIPTOR"}, {8, "GET_CONFIGURATION"}, {9, "SET_CONFIGURATION"},
            {10, "GET_INTERFACE"}, {11, "SET_INTERFACE"}};
        auto it = names.find(s[1]);
        if ((s[0] & 0x60) == 0 && it != names.end())
            out << it->second;
        else
            out << "SETUP type=0x" << std::hex << unsigned(s[0]) << " req=0x" << unsigned(s[1]) << std::dec;
        out << " value=" << value << " index=" << iface << " len=" << length;
    }
    return out.str();
}
std::string decode_dfu_status(const std::vector<uint8_t> &data) {
    static const char *states[] = {"appIDLE",          "appDETACH",   "dfuIDLE",
                                   "dfuDNLOAD-SYNC",   "dfuDNBUSY",   "dfuDNLOAD-IDLE",
                                   "dfuMANIFEST-SYNC", "dfuMANIFEST", "dfuMANIFEST-WAIT-RESET",
                                   "dfuUPLOAD-IDLE",   "dfuERROR"};
    static const char *errors[] = {"OK",         "errTARGET",       "errFILE",     "errWRITE",
                                   "errERASE",   "errCHECK_ERASED", "errPROG",     "errVERIFY",
                                   "errADDRESS", "errNOTDONE",      "errFIRMWARE", "errVENDOR",
                                   "errUSBR",    "errPOR",          "errUNKNOWN",  "errSTALLEDPKT"};
    if (data.size() < 6)
        return "short DFU_GETSTATUS response";
    unsigned poll = data[1] | (unsigned(data[2]) << 8) | (unsigned(data[3]) << 16);
    std::string state = data[4] < 11 ? states[data[4]] : "state=" + std::to_string(data[4]);
    std::string error = data[0] < 16 ? errors[data[0]] : "status=" + std::to_string(data[0]);
    return state + " " + error + " poll=" + std::to_string(poll) + "ms";
}
std::string decode_bot(const std::vector<uint8_t> &data) {
    if (data.size() < 31)
        return {};
    uint32_t signature = uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16) |
                         (uint32_t(data[3]) << 24);
    if (signature != 0x43425355)
        return {};
    unsigned cb_len = data[14];
    if (cb_len < 1 || cb_len > 16 || data.size() < 15u + cb_len)
        return {};
    const uint8_t *cb = data.data() + 15;
    if ((cb[0] == 0x28 || cb[0] == 0x2a) && cb_len >= 10) {
        uint32_t lba = (uint32_t(cb[2]) << 24) | (uint32_t(cb[3]) << 16) | (uint32_t(cb[4]) << 8) | cb[5];
        unsigned blocks = (unsigned(cb[7]) << 8) | cb[8];
        std::ostringstream out;
        out << (cb[0] == 0x28 ? "READ(10)" : "WRITE(10)") << " LBA 0x" << std::hex << lba << std::dec << " × "
            << blocks;
        return out.str();
    }
    switch (cb[0]) {
    case 0x00:
        return "TEST UNIT READY";
    case 0x12:
        return "INQUIRY";
    case 0x1a:
    case 0x5a:
        return "MODE SENSE";
    case 0x25:
        return "READ CAPACITY";
    case 0x2f:
        return "VERIFY";
    default: {
        std::ostringstream out;
        out << "SCSI 0x" << std::hex << std::setw(2) << std::setfill('0') << unsigned(cb[0]);
        return out.str();
    }
    }
}
void Counters::sample(double seconds) {
    if (seconds <= 0)
        return;
    auto new_transfers = transfers - previous_transfers;
    auto new_pairs = paired - previous_paired;
    auto new_errors = errors - previous_errors;
    in_rate = double(in - previous_in) / seconds;
    out_rate = double(out - previous_out) / seconds;
    transfer_rate = double(new_transfers) / seconds;
    window_latency_ms = new_pairs ? (latency_ms - previous_latency_sum) / double(new_pairs) : 0;
    window_max_latency_ms = pending_window_max;
    if (new_errors)
        last_error = pending_error;
    else if (new_transfers)
        last_error = 0;
    previous_transfers = transfers;
    previous_in = in;
    previous_out = out;
    previous_paired = paired;
    previous_errors = errors;
    previous_latency_sum = latency_ms;
    pending_window_max = 0;
    pending_error = 0;
    bounded_push(in_history, in_rate, rate_samples);
    bounded_push(out_history, out_rate, rate_samples);
}
void Model::devices(const std::vector<Device> &snapshot, bool initial) {
    std::map<std::string, Device> present;
    for (const auto &d : snapshot)
        present[d.port] = d;
    for (auto &[key, port] : ports) {
        if (port.device.connected && !present.contains(key)) {
            port.device.connected = false;
            bounded_push(lifecycle, "DISCONNECT " + key + " " + port.device.name, 100);
        }
    }
    addresses_.clear();
    for (const auto &[key, d] : present) {
        auto it = ports.find(key);
        if (it == ports.end()) {
            Port p;
            p.device = d;
            ports.emplace(key, std::move(p));
            if (!initial)
                bounded_push(lifecycle, "CONNECT " + key + " " + d.vendor + ":" + d.product + " " + d.name,
                             100);
        } else {
            auto &p = it->second;
            if (!p.device.connected || p.device.address != d.address || p.device.product != d.product ||
                p.device.vendor != d.vendor) {
                ++p.reconnects;
                p.dfu_status.clear();
                bounded_push(lifecycle,
                             "REENUMERATE " + key + " " + d.vendor + ":" + d.product +
                                 " address=" + std::to_string(d.address),
                             100);
            }
            p.device = d;
        }
        addresses_[{d.bus, d.address}] = key;
    }
}
void Model::ingest(const Event &e) {
    if (e.h.type != 'S' && e.h.type != 'C' && e.h.type != 'E')
        return;
    auto id = std::make_pair(int(e.h.bus), e.h.id);
    auto address = addresses_.find({e.h.bus, e.h.device});
    std::string key = address != addresses_.end()
                          ? address->second
                          : "bus" + std::to_string(e.h.bus) + ":" + std::to_string(e.h.device);
    // Bounded correlation state, including requests that never produce a callback.
    while (!pending_order_.empty() &&
           (pending_order_.size() >= 65536 || e.timestamp() - pending_order_.front().second > 60)) {
        auto [old_id, time] = pending_order_.front();
        pending_order_.pop_front();
        auto old = pending_.find(old_id);
        if (old != pending_.end() && old->second.event.timestamp() == time) {
            pending_.erase(old);
            ++expired;
        }
    }
    auto found = pending_.find(id);
    if (e.h.type != 'S' && found != pending_.end())
        key = found->second.port;
    auto [port_it, inserted] = ports.try_emplace(key);
    auto &port = port_it->second;
    if (inserted) {
        port.device.port = key;
        port.device.bus = e.h.bus;
        port.device.address = e.h.device;
        port.device.name = "Unresolved / root hub";
        port.device.connected = false;
    }
    std::string desc = decode_setup(e.h, &port.device, forced_interface);
    double latency = -1;
    std::vector<uint8_t> payload = preview_bytes(e);
    if (desc.empty() && e.h.transfer == 3)
        desc = decode_bot(payload);
    bool dfu = desc.starts_with("DFU_");
    std::array<uint8_t, 8> setup = e.h.setup;
    bool has_setup = e.h.transfer == 2 && e.h.setup_flag == 0;
    if (e.h.type == 'S') {
        // Keep only enough OUT bytes for the preview; capture recording retains the chosen snaplen.
        Event saved;
        saved.h = e.h;
        saved.data = payload;
        pending_[id] = {std::move(saved), key, desc, dfu};
        pending_order_.push_back({id, e.timestamp()});
    } else if (found != pending_.end()) {
        const auto &p = found->second;
        desc = p.description;
        dfu = p.dfu;
        setup = p.event.h.setup;
        has_setup = p.event.h.transfer == 2 && p.event.h.setup_flag == 0;
        latency = std::max(0.0, (e.timestamp() - p.event.timestamp()) * 1000);
        if (!e.input() && payload.empty())
            payload = p.event.data;
        if (desc.empty() && e.h.transfer == 3)
            desc = decode_bot(payload);
        if (dfu && e.h.status == 0 && p.event.h.setup[1] == 3) {
            port.dfu_status = decode_dfu_status(e.data);
            desc += " -> " + port.dfu_status;
        }
        if (dfu && e.h.status == 0 && p.event.h.setup[1] == 5 && !e.data.empty())
            desc += " -> state=" + std::to_string(e.data[0]);
        pending_.erase(found);
    } else
        ++unpaired;
    Transfer t;
    t.id = e.h.id;
    t.timestamp = e.timestamp();
    t.port = key;
    t.description = desc;
    t.payload = hex_preview(payload);
    t.bytes = payload;
    t.setup = setup;
    t.has_setup = has_setup;
    t.bus = e.h.bus;
    t.address = e.h.device;
    t.endpoint = e.h.endpoint & 15;
    t.status = e.h.status;
    t.length = e.h.length;
    t.captured = e.data.size();
    t.input = e.input();
    t.complete = e.h.type != 'S';
    t.dfu = dfu;
    t.latency_ms = latency;
    t.kind = e.h.transfer < 4 ? kinds[e.h.transfer] : "OTHER";
    if (t.complete) {
        count(total, e, latency);
        count(port.counters, e, latency);
        count(port.endpoints[e.h.endpoint], e, latency);
    }
    bounded_push(transfers, t, 2000);
    bounded_push(port.transfers, std::move(t), 128);
}
void Model::sample(double seconds) {
    total.sample(seconds);
    for (auto &[_, p] : ports) {
        p.counters.sample(seconds);
        for (auto &[__, c] : p.endpoints)
            c.sample(seconds);
    }
}

namespace {
struct GetArg {
    Header *header;
    void *data;
    size_t size;
};
struct Stats {
    uint32_t queued, dropped;
};
class Live : public Source {
    int fd_, bus_;
    std::vector<uint8_t> buffer_;

  public:
    Live(int bus, size_t snaplen) : bus_(bus), buffer_(snaplen) {
        auto path = "/dev/usbmon" + std::to_string(bus);
        fd_ = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0)
            throw std::runtime_error("Cannot open " + path + ": " + std::strerror(errno) +
                                     ". Load with `sudo modprobe usbmon`, then run `sudo ./build/busb`. Use "
                                     "--demo without capture access.");
    }
    ~Live() override { close(fd_); }
    std::optional<Event> next() override {
        Event e;
        GetArg arg{&e.h, buffer_.data(), buffer_.size()};
        if (ioctl(fd_, _IOW(0x92, 10, GetArg), &arg) < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return {};
            throw std::runtime_error(std::string("usbmon GETX: ") + std::strerror(errno));
        }
        size_t n = std::min<size_t>(e.h.captured, buffer_.size());
        e.data.assign(buffer_.begin(), buffer_.begin() + n);
        return e;
    }
    uint32_t dropped() override {
        Stats s{};
        if (ioctl(fd_, _IOR(0x92, 3, Stats), &s) < 0)
            throw std::runtime_error(std::string("usbmon stats: ") + std::strerror(errno));
        return s.dropped;
    }
    std::string label() const override { return "LIVE usbmon" + std::to_string(bus_); }
};

struct PcapHeader {
    uint32_t magic;
    uint16_t major, minor;
    int32_t zone;
    uint32_t accuracy, snaplen, network;
};
struct PacketHeader {
    uint32_t seconds, micros, captured, original;
};
static_assert(sizeof(PcapHeader) == 24);
static_assert(sizeof(PacketHeader) == 16);
class Replay : public Source {
    std::ifstream file_;
    std::optional<Event> buffered_;
    std::optional<double> first_;
    Clock::time_point start_{Clock::now()};
    bool eof_{};

  public:
    explicit Replay(const std::string &path) : file_(path, std::ios::binary) {
        PcapHeader p{};
        if (!file_.read(reinterpret_cast<char *>(&p), sizeof(p)) || p.magic != 0xa1b2c3d4 || p.major != 2 ||
            p.network != 220)
            throw std::runtime_error("Replay requires native-endian classic pcap with "
                                     "LINKTYPE_USB_LINUX_MMAPPED (220), as recorded by busb.");
    }
    bool finished() const override { return eof_; }
    std::string label() const override { return "REPLAY (original timing)"; }
    std::optional<Event> next() override {
        if (eof_)
            return {};
        if (!buffered_) {
            PacketHeader p{};
            file_.read(reinterpret_cast<char *>(&p), sizeof(p));
            if (file_.gcount() == 0 && file_.eof()) {
                eof_ = true;
                return {};
            }
            if (!file_ || p.captured < sizeof(Header) || p.captured > 16 * 1024 * 1024 ||
                p.original < p.captured)
                throw std::runtime_error("Invalid or truncated pcap record");
            Event e;
            if (!file_.read(reinterpret_cast<char *>(&e.h), sizeof(Header)))
                throw std::runtime_error("Truncated usbmon header");
            e.data.resize(p.captured - sizeof(Header));
            if (!file_.read(reinterpret_cast<char *>(e.data.data()), e.data.size()))
                throw std::runtime_error("Truncated USB payload");
            e.h.seconds = p.seconds;
            e.h.micros = p.micros;
            buffered_ = std::move(e);
            if (!first_)
                first_ = buffered_->timestamp();
        }
        if (buffered_->timestamp() - *first_ > std::chrono::duration<double>(Clock::now() - start_).count())
            return {};
        auto result = std::move(buffered_);
        buffered_.reset();
        return result;
    }
};
class Demo : public Source {
    Clock::time_point start_{Clock::now()};
    double next_{};
    uint64_t id_{};
    std::deque<Event> queue_;

  public:
    std::string label() const override { return "DEMO - synthetic mouse / disk / DFU"; }
    std::optional<Event> next() override {
        double now = std::chrono::duration<double>(Clock::now() - start_).count();
        if (!queue_.empty()) {
            auto e = queue_.front();
            if (e.timestamp() > now)
                return {};
            queue_.pop_front();
            return e;
        }
        if (now < next_)
            return {};
        next_ = now + 0.08;
        Event s;
        s.h.id = ++id_;
        s.h.type = 'S';
        s.h.transfer = 2;
        s.h.bus = 1;
        s.h.device = 7;
        s.h.status = -EINPROGRESS;
        s.h.seconds = int64_t(now);
        s.h.micros = int((now - std::floor(now)) * 1e6);
        bool getstatus = id_ % 3 == 0;
        s.h.endpoint = getstatus ? 0x80 : 0;
        s.h.setup = {uint8_t(getstatus ? 0xa1 : 0x21),
                     uint8_t(getstatus ? 3 : 1),
                     uint8_t(id_ & 255),
                     uint8_t(id_ >> 8),
                     0,
                     0,
                     uint8_t(getstatus ? 6 : 0),
                     uint8_t(getstatus ? 0 : 4)};
        s.h.length = getstatus ? 6 : 1024;
        if (!getstatus) {
            s.data.resize(1024);
            for (size_t i = 0; i < s.data.size(); ++i)
                s.data[i] = uint8_t(id_ + i);
        }
        s.h.captured = s.data.size();
        s.h.data_flag = getstatus ? '<' : 0;
        Event c = s;
        c.h.type = 'C';
        c.h.setup_flag = '-';
        c.h.setup.fill(0);
        c.h.micros += 2400;
        if (c.h.micros >= 1000000) {
            ++c.h.seconds;
            c.h.micros -= 1000000;
        }
        c.h.status = id_ % 97 == 0 ? -EPIPE : 0;
        c.data = getstatus ? std::vector<uint8_t>{0, 5, 0, 0, 5, 0} : std::vector<uint8_t>{};
        c.h.captured = c.data.size();
        c.h.data_flag = getstatus ? 0 : '>';
        if (c.h.status < 0)
            c.h.length = 0;
        if (id_ % 5 == 0 || id_ % 5 == 1) {
            // Interrupt HID input and sustained bulk storage reads use the same collector.
            bool mouse = id_ % 5 == 0;
            for (auto *e : {&s, &c}) {
                e->h.device = mouse ? 8 : 9;
                e->h.transfer = mouse ? 1 : 3;
                e->h.endpoint = 0x81;
                e->h.setup_flag = '-';
                e->h.setup.fill(0);
                e->h.length = mouse ? 8 : 128 * 1024;
                e->data.clear();
            }
            c.h.status = 0;
            c.data =
                mouse ? std::vector<uint8_t>{0, 3, 0xff, 0, 0, 0, 0, 0} : std::vector<uint8_t>(256, 0x5a);
            s.h.captured = 0;
            s.h.data_flag = '<';
            c.h.captured = c.data.size();
            c.h.data_flag = 0;
        }
        queue_.push_back(std::move(c));
        return s;
    }
};
} // namespace
std::unique_ptr<Source> live_source(int bus, size_t snaplen) { return std::make_unique<Live>(bus, snaplen); }
std::unique_ptr<Source> demo_source() { return std::make_unique<Demo>(); }
std::unique_ptr<Source> replay_source(const std::string &path) { return std::make_unique<Replay>(path); }
std::vector<Device> demo_devices() {
    Device d;
    d.port = "1-2.3";
    d.name = "DFU development board";
    d.vendor = "0483";
    d.product = "df11";
    d.serial = "DEMO-001";
    d.speed = "480";
    d.bus = 1;
    d.address = 7;
    d.dfu_interfaces = {0};
    d.class_summary = "DFU";
    d.interfaces_text = "0 DFU";
    d.megabits = 480;
    Device mouse;
    mouse.port = "1-3";
    mouse.name = "USB mouse (synthetic motion)";
    mouse.vendor = "demo";
    mouse.product = "0002";
    mouse.speed = "12";
    mouse.bus = 1;
    mouse.address = 8;
    mouse.class_summary = "HID";
    mouse.interfaces_text = "0 HID";
    mouse.megabits = 12;
    Device disk;
    disk.port = "1-4";
    disk.name = "USB storage (synthetic reads)";
    disk.vendor = "demo";
    disk.product = "0003";
    disk.speed = "5000";
    disk.bus = 1;
    disk.address = 9;
    disk.class_summary = "Mass Storage";
    disk.interfaces_text = "0 Mass Storage";
    disk.megabits = 5000;
    return {d, mouse, disk};
}
Recorder::Recorder(const std::string &path) {
    // Refuse to overwrite captures (also protects replaying into the same file).
    fd_ = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd_ < 0)
        throw std::runtime_error("Cannot create capture " + path + ": " + std::strerror(errno));
    PcapHeader p{0xa1b2c3d4, 2, 4, 0, 0, 16 * 1024 * 1024, 220};
    try {
        bytes(&p, sizeof(p));
    } catch (...) {
        close(fd_);
        throw;
    }
}
Recorder::~Recorder() {
    if (fd_ >= 0)
        close(fd_);
}
void Recorder::bytes(const void *data, size_t size) {
    const auto *p = static_cast<const char *>(data);
    while (size) {
        auto written = ::write(fd_, p, size);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            throw std::runtime_error("Writing capture failed: " + std::string(std::strerror(errno)));
        p += written;
        size -= written;
    }
}
void Recorder::write(const Event &e) {
    PacketHeader p{uint32_t(e.h.seconds), uint32_t(e.h.micros), uint32_t(sizeof(Header) + e.data.size()),
                   uint32_t(sizeof(Header) + e.h.captured)};
    // len_cap is the original kernel capture length; pcap incl_len tracks our snaplen.
    bytes(&p, sizeof(p));
    bytes(&e.h, sizeof(e.h));
    bytes(e.data.data(), e.data.size());
}
} // namespace busb
