#include "core.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace busb;
#define CHECK(condition)                                                                                     \
    do {                                                                                                     \
        if (!(condition))                                                                                    \
            throw std::runtime_error(std::string(__func__) + ": " + #condition);                             \
    } while (0)

namespace {
Device device(int address = 7) {
    Device d;
    d.port = "1-2.3";
    d.bus = 1;
    d.address = address;
    d.name = "Test board";
    d.vendor = "1234";
    d.product = "5678";
    d.dfu_interfaces = {2};
    return d;
}
Event submission(uint64_t id = 1, bool input = false) {
    Event e;
    e.h.id = id;
    e.h.type = 'S';
    e.h.transfer = 3;
    e.h.endpoint = input ? 0x81 : 1;
    e.h.bus = 1;
    e.h.device = 7;
    e.h.seconds = 100;
    e.h.status = -EINPROGRESS;
    e.h.length = 1024;
    e.h.setup_flag = '-';
    return e;
}
Event completion(Event e, uint32_t actual = 512, int status = 0) {
    e.h.type = 'C';
    e.h.length = actual;
    e.h.status = status;
    e.h.micros += 2500;
    e.h.setup_flag = '-';
    e.h.setup.fill(0);
    return e;
}
void bytes_counted_once() {
    Model m;
    m.devices({device()}, true);
    auto s = submission();
    m.ingest(s);
    CHECK(m.total.out == 0);
    CHECK(m.total.errors == 0);
    CHECK(m.pending() == 1);
    m.ingest(completion(s));
    CHECK(m.total.out == 512);
    CHECK(m.total.transfers == 1);
    CHECK(m.total.errors == 0);
    CHECK(m.pending() == 0);
    CHECK(std::abs(m.total.latency_ms - 2.5) < 0.001);
    auto input = submission(2, true);
    m.ingest(input);
    m.ingest(completion(input, 8));
    CHECK(m.total.in == 8);
    CHECK(m.ports.at("1-2.3").endpoints.at(0x81).in == 8);
    m.sample(0.5);
    CHECK(m.total.in_rate == 16);
    CHECK(m.total.out_rate == 1024);
    CHECK(m.total.transfer_rate == 4);
    m.sample(0.5);
    CHECK(m.total.in_rate == 0);
    CHECK(m.total.out_rate == 0);
}
void errors_and_missing_submissions() {
    Model m;
    auto s = submission();
    m.ingest(s);
    m.ingest(completion(s, 0, -EPIPE));
    CHECK(m.total.errors == 1);
    CHECK(m.total.out == 0);
    CHECK(status_name(-EPIPE) == "STALL");
    auto orphan = completion(submission(2, true), 12);
    m.ingest(orphan);
    CHECK(m.unpaired == 1);
    CHECK(m.total.in == 12);
    auto failed = submission(3);
    m.ingest(failed);
    failed.h.type = 'E';
    failed.h.status = -ENODEV;
    m.ingest(failed);
    CHECK(m.total.errors == 2);
    CHECK(m.total.out == 0);
    CHECK(m.pending() == 0);
}
void dfu_decoding() {
    Model m;
    auto d = device();
    m.devices({d}, true);
    auto s = submission();
    s.h.transfer = 2;
    s.h.endpoint = 0x80;
    s.h.setup_flag = 0;
    s.h.setup = {0xa1, 3, 0, 0, 2, 0, 6, 0};
    CHECK(decode_setup(s.h, &d).starts_with("DFU_GETSTATUS"));
    auto wrong = s;
    wrong.h.setup[4] = 1;
    CHECK(!decode_setup(wrong.h, &d).starts_with("DFU_"));
    CHECK(decode_setup(wrong.h, nullptr, 1).starts_with("DFU_GETSTATUS"));
    wrong.h.setup[0] = 0x21;
    CHECK(!decode_setup(wrong.h, nullptr, 1).starts_with("DFU_"));
    m.ingest(s);
    auto c = completion(s, 6);
    c.data = {0, 0x56, 0x34, 0x12, 4, 0};
    c.h.captured = 6;
    m.ingest(c);
    CHECK(m.ports.at(d.port).dfu_status == "dfuDNBUSY OK poll=1193046ms");
    CHECK(m.transfers.back().dfu);
    CHECK(decode_dfu_status({0, 0}).starts_with("short"));
    CHECK(decode_dfu_status({7, 0, 0, 0, 10, 0}).find("errVERIFY") != std::string::npos);
    s.h.setup = {0x21, 1, 42, 0, 2, 0, 0, 0};
    CHECK(decode_setup(s.h, &d).find("block=42 len=0 (manifest)") != std::string::npos);
}
void reenumeration_follows_port() {
    Model m;
    auto d = device();
    m.devices({d}, true);
    auto s = submission();
    m.ingest(s);
    m.devices({});
    CHECK(!m.ports.at(d.port).device.connected);
    auto new_d = device(9);
    new_d.product = "df11";
    m.devices({new_d});
    CHECK(m.ports.at(d.port).reconnects == 1);
    CHECK(m.ports.at(d.port).device.address == 9);
    // An old in-flight callback still belongs to its original physical port.
    m.ingest(completion(s, 20));
    auto newer = submission(2);
    newer.h.device = 9;
    m.ingest(newer);
    m.ingest(completion(newer, 30));
    CHECK(m.ports.at(d.port).counters.out == 50);
    CHECK(m.ports.size() == 1);
    CHECK(m.lifecycle.back().starts_with("REENUMERATE"));
}
void correlation_and_retention() {
    Model m;
    auto a = submission();
    auto b = a;
    b.h.bus = 2;
    m.ingest(a);
    m.ingest(b);
    m.ingest(completion(a));
    CHECK(m.pending() == 1);
    m.ingest(completion(b));
    CHECK(m.pending() == 0);
    m.ingest(submission(100));
    auto late = submission(101);
    late.h.seconds += 61;
    m.ingest(late);
    CHECK(m.expired == 1);
    for (unsigned i = 0; i < 3000; ++i) {
        auto e = submission(1000 + i);
        e.h.seconds += 62;
        m.ingest(e);
        m.ingest(completion(e));
    }
    CHECK(m.transfers.size() == 2000);
    CHECK(m.ports.begin()->second.transfers.size() <= 128);
    for (int i = 0; i < 200; ++i)
        m.sample(0.5);
    CHECK(m.total.in_history.size() == 180);
}
void recent_latency_and_last_error() {
    Model m;
    auto stalled = submission();
    m.ingest(stalled);
    m.ingest(completion(stalled, 0, -EPIPE));
    m.sample(0.5);
    CHECK(m.total.last_error == -EPIPE);
    CHECK(status_name(m.total.last_error) == "STALL");
    CHECK(m.total.window_max_latency_ms > 2);
    auto ok = submission(2);
    m.ingest(ok);
    m.ingest(completion(ok, 8));
    m.sample(0.5);
    CHECK(m.total.last_error == 0);
    CHECK(std::abs(m.total.window_latency_ms - 2.5) < 0.001);
    CHECK(m.total.window_max_latency_ms < 3);
}
void mass_storage_command() {
    std::vector<uint8_t> cbw(31, 0);
    cbw[0] = 'U';
    cbw[1] = 'S';
    cbw[2] = 'B';
    cbw[3] = 'C';
    cbw[14] = 10;
    cbw[15] = 0x28;
    cbw[17] = 0x00;
    cbw[18] = 0x10;
    cbw[19] = 0x20;
    cbw[20] = 0x30;
    cbw[23] = 64;
    CHECK(decode_bot(cbw) == "READ(10) LBA 0x102030 × 64");
    cbw[15] = 0x2a;
    CHECK(decode_bot(cbw).starts_with("WRITE(10) LBA 0x102030"));
    cbw[0] = 0;
    CHECK(decode_bot(cbw).empty());
    cbw[0] = 'U';
    cbw[15] = 0x28;
    Model m;
    auto s = submission();
    s.data = cbw;
    s.h.captured = s.data.size();
    m.ingest(s);
    m.ingest(completion(s, 31));
    CHECK(m.transfers.back().description == "READ(10) LBA 0x102030 × 64");
    CHECK(m.transfers.back().bytes[0] == 'U');
}
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        char p[] = "/tmp/busb-test-XXXXXX";
        auto *result = mkdtemp(p);
        if (!result)
            throw std::runtime_error("mkdtemp");
        path = result;
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};
void fixture(const std::filesystem::path &p, const std::string &value) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << value << '\n';
}
void sysfs_discovery() {
    TempDir t;
    auto p = t.path / "3-4";
    fixture(p / "busnum", "3");
    fixture(p / "devnum", "17");
    fixture(p / "idVendor", "1234");
    fixture(p / "idProduct", "5678");
    fixture(p / "product", "Development board");
    fixture(p / "speed", "480");
    fixture(p / "bDeviceClass", "00");
    auto hid = p / "3-4:1.0";
    fixture(hid / "bInterfaceClass", "03");
    fixture(hid / "bInterfaceSubClass", "00");
    fixture(hid / "bInterfaceNumber", "00");
    auto iface = p / "3-4:1.2";
    fixture(iface / "bInterfaceClass", "fe");
    fixture(iface / "bInterfaceSubClass", "01");
    fixture(iface / "bInterfaceNumber", "02");
    auto hub = t.path / "3-1";
    fixture(hub / "busnum", "3");
    fixture(hub / "devnum", "1");
    fixture(hub / "bDeviceClass", "09");
    fixture(hub / "product", "USB hub");
    fixture(hub / "speed", "480");
    auto hub_if = hub / "3-1:1.0";
    fixture(hub_if / "bInterfaceClass", "09");
    fixture(hub_if / "bInterfaceSubClass", "00");
    fixture(hub_if / "bInterfaceNumber", "00");
    auto loader = t.path / "3-5";
    fixture(loader / "busnum", "3");
    fixture(loader / "devnum", "53");
    fixture(loader / "idVendor", "2207");
    fixture(loader / "idProduct", "262d");
    fixture(loader / "speed", "480");
    fixture(loader / "bDeviceClass", "00");
    auto rock = loader / "3-5:1.0";
    fixture(rock / "bInterfaceClass", "ff");
    fixture(rock / "bInterfaceSubClass", "06");
    fixture(rock / "bInterfaceProtocol", "05");
    fixture(rock / "bInterfaceNumber", "00");
    auto vendor = t.path / "3-6";
    fixture(vendor / "busnum", "3");
    fixture(vendor / "devnum", "8");
    fixture(vendor / "idVendor", "06cb");
    fixture(vendor / "idProduct", "0104");
    fixture(vendor / "speed", "12");
    fixture(vendor / "bDeviceClass", "ff");
    auto vendor_if = vendor / "3-6:1.0";
    fixture(vendor_if / "bInterfaceClass", "ff");
    fixture(vendor_if / "bInterfaceSubClass", "00");
    fixture(vendor_if / "bInterfaceProtocol", "00");
    fixture(vendor_if / "bInterfaceNumber", "00");
    std::filesystem::create_directories(t.path / "usb3");
    auto devices = scan_devices(t.path);
    CHECK(devices.size() == 4);
    auto board = std::find_if(devices.begin(), devices.end(), [](const Device &d) { return d.port == "3-4"; });
    auto hub_device = std::find_if(devices.begin(), devices.end(), [](const Device &d) { return d.port == "3-1"; });
    CHECK(board != devices.end());
    CHECK(hub_device != devices.end());
    CHECK(board->address == 17);
    CHECK(board->dfu_interfaces == std::vector<int>{2});
    CHECK(std::abs(board->megabits - 480) < 0.01);
    CHECK(board->class_summary.find("HID") != std::string::npos);
    CHECK(board->class_summary.find("DFU") != std::string::npos);
    CHECK(board->interfaces_text.find("0 HID") != std::string::npos);
    CHECK(board->interfaces_text.find("2 DFU") != std::string::npos);
    CHECK(hub_device->hub);
    CHECK(hub_device->class_summary.find("Hub") != std::string::npos);
    auto rockusb = std::find_if(devices.begin(), devices.end(), [](const Device &d) { return d.port == "3-5"; });
    CHECK(rockusb != devices.end());
    CHECK(rockusb->name == "Rockchip Rockusb");
    CHECK(rockusb->class_summary == "Rockusb");
    CHECK(rockusb->interfaces_text == "0 Rockusb");
    CHECK(rockusb->dfu_interfaces.empty());
    auto other = std::find_if(devices.begin(), devices.end(), [](const Device &d) { return d.port == "3-6"; });
    CHECK(other != devices.end());
    CHECK(other->name == "USB device");
    CHECK(other->class_summary == "Vendor");
}
void pcap_roundtrip_and_validation() {
    TempDir t;
    auto path = (t.path / "trace.pcap").string();
    auto e = submission();
    e.h.seconds = 0;
    e.data = {1, 2, 3};
    e.h.captured = 4096;
    {
        Recorder r(path);
        r.write(e);
    }
    auto source = replay_source(path);
    auto read = source->next();
    CHECK(read.has_value());
    CHECK(read->h.id == e.h.id);
    CHECK(read->data == e.data);
    CHECK(read->h.captured == 4096);
    CHECK(!source->next());
    CHECK(source->finished());
    bool refused = false;
    try {
        Recorder r(path);
    } catch (const std::exception &) {
        refused = true;
    }
    CHECK(refused);
    auto truncated = (t.path / "truncated.pcap").string();
    std::filesystem::copy_file(path, truncated);
    std::filesystem::resize_file(truncated, std::filesystem::file_size(truncated) - 1);
    refused = false;
    try {
        auto broken = replay_source(truncated);
        broken->next();
    } catch (const std::exception &) {
        refused = true;
    }
    CHECK(refused);
    fixture(t.path / "bad.pcap", "not a capture");
    refused = false;
    try {
        replay_source((t.path / "bad.pcap").string());
    } catch (const std::exception &) {
        refused = true;
    }
    CHECK(refused);
}
void iso_preview_skips_descriptors() {
    Model m;
    auto e = completion(submission(1, true));
    e.h.transfer = 0;
    e.h.descriptors = 1;
    e.data.assign(16, 0);
    e.data.push_back(0xab);
    e.h.captured = e.data.size();
    m.ingest(e);
    CHECK(m.transfers.back().payload == "ab");
}
void bulk_counters_exceed_four_gibibytes() {
    Model m;
    for (unsigned i = 0; i < 5000; ++i) {
        auto s = submission(i, true);
        s.h.length = 1024 * 1024;
        m.ingest(s);
        m.ingest(completion(s, s.h.length));
    }
    CHECK(m.total.in == uint64_t(5000) * 1024 * 1024);
    CHECK(m.total.transfers == 5000);
    CHECK(m.total.errors == 0);
    CHECK(m.pending() == 0);
    m.sample(1);
    CHECK(m.total.in_rate > 4.0 * 1024 * 1024 * 1024);
    CHECK(human(m.total.in_rate).find("GiB") != std::string::npos);
}
} // namespace
int main() {
    try {
        bytes_counted_once();
        errors_and_missing_submissions();
        dfu_decoding();
        reenumeration_follows_port();
        correlation_and_retention();
        sysfs_discovery();
        pcap_roundtrip_and_validation();
        iso_preview_skips_descriptors();
        bulk_counters_exceed_four_gibibytes();
        recent_latency_and_last_error();
        mass_storage_command();
        std::cout << "11 core scenarios passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
