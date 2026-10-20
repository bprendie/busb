#include "core.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
void usage() {
    std::cout << "busb 0.1.0 — USB I/O activity monitor\n\n"
                 "Usage: busb [options]\n"
                 "  --demo               Synthetic mouse, storage and DFU; no root needed\n"
                 "  --list               List connected USB devices and physical port paths\n"
                 "  --bus N              Capture a USB bus (default 0 = all buses)\n"
                 "  --port PATH          Initially select physical port, e.g. 3-2.1\n"
                 "  --dfu-interface N    Decode class requests on interface N as DFU\n"
                 "  --snaplen N          Max captured bytes per live event (default 4096)\n"
                 "  --record FILE.pcap   Record bus traffic for Wireshark/replay; must be new\n"
                 "  --replay FILE.pcap   Replay busb/native-endian usbmon pcap at original timing\n"
                 "  --headless           Print summary on exit, without terminal UI\n"
                 "  --duration SECONDS   Stop after the given time\n"
                 "  --help / --version   Show help / version\n\n"
                 "Live capture: sudo modprobe usbmon; sudo ./build/busb\n"
                 "Keys: ↑↓ select port, a scope, d DFU, e errors, h hex, Space freeze, ? help, q quit\n";
}
double numeric(const std::string &s) {
    size_t end = 0;
    double n = std::stod(s, &end);
    if (end != s.size() || !std::isfinite(n))
        throw std::runtime_error("Invalid number: " + s);
    return n;
}
int integer(const std::string &s, int low, int high) {
    double n = numeric(s);
    if (n != std::floor(n) || n < low || n > high)
        throw std::runtime_error("Number out of range: " + s);
    return int(n);
}
} // namespace
int main(int argc, char **argv) {
    try {
        busb::Options o;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto value = [&]() -> std::string {
                if (++i == argc)
                    throw std::runtime_error("Missing value for " + a);
                return argv[i];
            };
            if (a == "--help" || a == "-h") {
                usage();
                return 0;
            }
            if (a == "--version") {
                std::cout << "busb 0.1.0\n";
                return 0;
            }
            if (a == "--demo")
                o.demo = true;
            else if (a == "--list")
                o.list = true;
            else if (a == "--headless")
                o.headless = true;
            else if (a == "--bus")
                o.bus = integer(value(), 0, 127);
            else if (a == "--dfu-interface")
                o.dfu_interface = integer(value(), 0, 255);
            else if (a == "--snaplen")
                o.snaplen = integer(value(), 0, 1024 * 1024);
            else if (a == "--port")
                o.port = value();
            else if (a == "--record")
                o.record = value();
            else if (a == "--replay")
                o.replay = value();
            else if (a == "--duration") {
                o.duration = numeric(value());
                if (o.duration <= 0)
                    throw std::runtime_error("Duration must be positive");
            } else
                throw std::runtime_error("Unknown option: " + a + ". See --help.");
        }
        if (o.demo && !o.replay.empty())
            throw std::runtime_error("Choose either --demo or --replay");
        if ((o.demo || !o.replay.empty()) && o.bus)
            throw std::runtime_error("--bus is for live capture; use --port to focus demo/replay");
        if (o.list) {
            std::cout << "PORT\tBUS:ADDR\tVID:PID\tMb/s\tDFU interfaces\tDEVICE\n";
            for (const auto &d : busb::scan_devices()) {
                if (o.bus && d.bus != o.bus)
                    continue;
                std::cout << d.port << '\t' << d.bus << ':' << d.address << '\t' << d.vendor << ':'
                          << d.product << '\t' << d.speed << '\t';
                for (int iface : d.dfu_interfaces)
                    std::cout << iface << ',';
                std::cout << '\t' << d.name << '\n';
            }
            return 0;
        }
        if (!o.headless && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)))
            throw std::runtime_error(
                "Interactive mode needs a terminal; use --headless --duration N or --list");
        auto source = o.demo              ? busb::demo_source()
                      : !o.replay.empty() ? busb::replay_source(o.replay)
                                          : busb::live_source(o.bus, o.snaplen);
        std::unique_ptr<busb::Recorder> recorder;
        if (!o.record.empty())
            recorder = std::make_unique<busb::Recorder>(o.record);
        busb::Model model;
        model.forced_interface = o.dfu_interface;
        if (o.headless)
            busb::run_headless(*source, model, recorder.get(), o);
        else
            busb::run_ui(*source, model, recorder.get(), o);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "busb: " << e.what() << '\n';
        return 1;
    }
}
