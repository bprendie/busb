#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace busb {
using Clock = std::chrono::steady_clock;
// Linux usbmon binary ABI. Setup words are USB little endian; other fields native endian.
struct Header {
    uint64_t id{};
    uint8_t type{}, transfer{}, endpoint{}, device{};
    uint16_t bus{};
    uint8_t setup_flag{}, data_flag{};
    int64_t seconds{};
    int32_t micros{}, status{};
    uint32_t length{}, captured{};
    std::array<uint8_t, 8> setup{};
    int32_t interval{}, start_frame{};
    uint32_t flags{}, descriptors{};
};
static_assert(sizeof(Header) == 64);
static_assert(offsetof(Header, setup) == 40);
struct Event {
    Header h;
    std::vector<uint8_t> data;
    double timestamp() const { return double(h.seconds) + h.micros / 1e6; }
    bool input() const { return h.endpoint & 0x80; }
};
struct Device {
    std::string port, name, vendor, product, serial, speed;
    std::string class_summary, interfaces_text;
    int bus{}, address{};
    std::vector<int> dfu_interfaces;
    bool connected{true}, hub{};
    double megabits{};
};
std::vector<Device> scan_devices(const std::filesystem::path &root = "/sys/bus/usb/devices");
std::string human(double value);
std::string status_name(int status);
std::string hex_preview(const std::vector<uint8_t> &data, size_t limit = 24);
std::string decode_setup(const Header &h, const Device *device, int forced_interface = -1);
std::string decode_dfu_status(const std::vector<uint8_t> &data);
std::string decode_bot(const std::vector<uint8_t> &data);

inline constexpr size_t rate_samples = 180;

struct Counters {
    uint64_t in{}, out{}, transfers{}, errors{};
    double latency_ms{}, max_latency_ms{};
    uint64_t paired{};
    std::deque<double> in_history, out_history;
    uint64_t previous_in{}, previous_out{}, previous_transfers{}, previous_errors{}, previous_paired{};
    double previous_latency_sum{}, pending_window_max{};
    int pending_error{};
    double in_rate{}, out_rate{}, transfer_rate{};
    double window_latency_ms{}, window_max_latency_ms{};
    int last_error{};
    void sample(double seconds);
};
struct Transfer {
    uint64_t id{};
    double timestamp{};
    std::string port, description, payload;
    int bus{}, address{}, endpoint{}, status{};
    uint32_t length{}, captured{};
    bool input{}, complete{}, dfu{}, has_setup{};
    double latency_ms{-1};
    std::string kind;
    std::vector<uint8_t> bytes;
    std::array<uint8_t, 8> setup{};
};
struct Port {
    Device device;
    Counters counters;
    std::map<int, Counters> endpoints;
    std::deque<Transfer> transfers;
    std::string dfu_status;
    unsigned reconnects{};
};
class Model {
    struct Pending {
        Event event;
        std::string port, description;
        bool dfu;
    };
    std::map<std::pair<int, uint64_t>, Pending> pending_;
    std::deque<std::pair<std::pair<int, uint64_t>, double>> pending_order_;
    std::map<std::pair<int, int>, std::string> addresses_;

  public:
    std::map<std::string, Port> ports;
    Counters total;
    std::deque<Transfer> transfers;
    std::deque<std::string> lifecycle;
    uint64_t dropped{}, interval_dropped{}, unpaired{}, expired{};
    int forced_interface{-1};
    void devices(const std::vector<Device> &snapshot, bool initial = false);
    void ingest(const Event &e);
    void sample(double seconds);
    size_t pending() const { return pending_.size(); }
};

class Source {
  public:
    virtual ~Source() = default;
    virtual std::optional<Event> next() = 0;
    virtual uint32_t dropped() { return 0; }
    virtual bool finished() const { return false; }
    virtual std::string label() const = 0;
};
std::unique_ptr<Source> live_source(int bus, size_t snaplen);
std::unique_ptr<Source> demo_source();
std::unique_ptr<Source> replay_source(const std::string &path);
std::vector<Device> demo_devices();
class Recorder {
    int fd_{-1};
    void bytes(const void *data, size_t size);

  public:
    explicit Recorder(const std::string &path);
    ~Recorder();
    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;
    void write(const Event &event);
};
struct Options {
    int bus{}, dfu_interface{-1};
    size_t snaplen{4096};
    bool demo{}, list{}, headless{};
    double duration{};
    std::string port, record, replay;
};
void run_ui(Source &source, Model &model, Recorder *recorder, const Options &options);
void run_headless(Source &source, Model &model, Recorder *recorder, const Options &options);
} // namespace busb
