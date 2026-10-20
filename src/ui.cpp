#include "core.hpp"

#include <algorithm>
#include <clocale>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <curses.h>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>

namespace busb {
namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
struct Signals {
    using Handler = void (*)(int);
    Handler old_int, old_term;
    Signals() : old_int(std::signal(SIGINT, stop)), old_term(std::signal(SIGTERM, stop)) { stopped = 0; }
    ~Signals() {
        std::signal(SIGINT, old_int);
        std::signal(SIGTERM, old_term);
    }
};
struct Terminal {
    Terminal() {
        std::setlocale(LC_ALL, "");
        if (!initscr())
            throw std::runtime_error("Cannot initialize terminal");
        cbreak();
        noecho();
        keypad(stdscr, true);
        nodelay(stdscr, true);
        curs_set(0);
        if (has_colors()) {
            start_color();
            use_default_colors();
            // ANSI colors are supplied by the user's terminal theme, including Omarchy themes.
            init_pair(1, COLOR_YELLOW, -1);
            init_pair(2, COLOR_GREEN, -1);
            init_pair(3, COLOR_CYAN, -1);
            init_pair(4, COLOR_RED, -1);
            init_pair(5, COLOR_YELLOW, -1);
            init_pair(6, COLOR_BLACK, COLOR_GREEN);
            init_pair(7, COLOR_WHITE, -1);
            init_pair(8, COLOR_MAGENTA, -1);
            init_pair(9, -1, -1);
            init_pair(10, COLOR_BLUE, -1);
        }
        set_escdelay(25);
    }
    ~Terminal() { endwin(); }
};
void put(int y, int x, const std::string &text, int color = 0, int limit = -1, bool bold = false) {
    int h, w;
    getmaxyx(stdscr, h, w);
    if (y < 0 || y >= h || x < 0 || x >= w)
        return;
    int available = std::min(limit < 0 ? w - x : limit, w - x);
    std::wstring out;
    std::mbstate_t state{};
    const char *ptr = text.data();
    size_t remaining = text.size();
    while (remaining && available > 0) {
        wchar_t c;
        size_t bytes = std::mbrtowc(&c, ptr, remaining, &state);
        if (bytes == size_t(-1) || bytes == size_t(-2)) {
            state = {};
            c = L'?';
            bytes = 1;
        }
        if (!bytes)
            break;
        int width = wcwidth(c);
        if (width < 0) {
            c = L'?';
            width = 1;
        }
        if (width > available)
            break;
        out.push_back(c);
        available -= width;
        ptr += bytes;
        remaining -= bytes;
    }
    attrset(COLOR_PAIR(color) | (bold ? A_BOLD : 0) | (color == 7 ? A_DIM : 0));
    mvaddnwstr(y, x, out.c_str(), out.size());
    attrset(A_NORMAL);
}
void box(int y, int x, int h, int w, const std::string &title, int color = 8) {
    if (h < 2 || w < 2)
        return;
    for (int col = x + 1; col < x + w - 1; ++col) {
        put(y, col, "─", color, 1);
        put(y + h - 1, col, "─", color, 1);
    }
    for (int row = y + 1; row < y + h - 1; ++row) {
        put(row, x, "│", color, 1);
        put(row, x + w - 1, "│", color, 1);
    }
    put(y, x, "╭", color, 1);
    put(y, x + w - 1, "╮", color, 1);
    put(y + h - 1, x, "╰", color, 1);
    put(y + h - 1, x + w - 1, "╯", color, 1);
    put(y, x + 2, " " + title + " ", 1, w - 4, true);
}
std::string fixed(double n, int precision = 2) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(precision) << n;
    return s.str();
}
void braille(int y, int x, unsigned dots, int color, bool bold = false) {
    wchar_t glyph[] = {wchar_t(0x2800 + dots), 0};
    attrset(COLOR_PAIR(color) | (color == 7 ? A_DIM : 0) | (bold ? A_BOLD : 0));
    mvaddnwstr(y, x, glyph, 1);
    attrset(A_NORMAL);
}
double peak_rate(const std::deque<double> &history) {
    return history.empty() ? 0 : *std::max_element(history.begin(), history.end());
}
void graph(int y, int x, int height, int width, const std::deque<double> &history, int color,
           bool inverted = false) {
    if (height < 1 || width < 1)
        return;
    double peak = 1;
    size_t begin = history.size() > size_t(width * 2) ? history.size() - width * 2 : 0;
    for (size_t i = begin; i < history.size(); ++i)
        peak = std::max(peak, history[i]);
    // Two samples per character; each braille column has four independently lit dots.
    static const unsigned bits[2][4] = {{6, 2, 1, 0}, {7, 5, 4, 3}};
    for (int column = 0; column < width; ++column) {
        double values[2]{};
        bool available = false;
        for (int side = 0; side < 2; ++side) {
            auto index = static_cast<long long>(history.size()) - width * 2 + column * 2 + side;
            if (index >= 0) {
                values[side] = history[size_t(index)] / peak * height * 4;
                available = true;
            }
        }
        for (int row = 0; row < height; ++row) {
            unsigned dots = 0;
            for (int side = 0; side < 2; ++side) {
                int level = std::clamp(int(std::ceil(values[side] - row * 4)), 0, 4);
                for (int dot = 0; dot < level; ++dot) {
                    unsigned bit = inverted ? bits[side][3 - dot] : bits[side][dot];
                    dots |= 1u << bit;
                }
            }
            int screen_row = inverted ? y + row : y + height - 1 - row;
            if (dots)
                braille(screen_row, x + column, dots, color, row > height / 2);
            else if (row == 0)
                put(screen_row, x + column, "╌", color == 6 ? 6 : 7, 1);
            else if (available && column % 8 == 0)
                put(screen_row, x + column, "·", 7, 1);
        }
    }
}
void meter(int y, int x, int width, double ratio, int color) {
    int filled = std::clamp(int(std::round(ratio * width)), 0, width);
    for (int i = 0; i < width; ++i)
        put(y, x + i, i < filled ? "━" : "─", i < filled ? color : 7, 1, i < filled);
}
bool consume(Source &source, Model &model, Recorder *recorder) {
    bool changed = false;
    auto until = Clock::now() + std::chrono::milliseconds(12);
    for (int i = 0; i < 4096 && Clock::now() < until; ++i) {
        auto event = source.next();
        if (!event)
            break;
        if (recorder)
            recorder->write(*event);
        model.ingest(*event);
        changed = true;
    }
    return changed;
}
void topology(Model &model, const Options &o, bool initial = false) {
    if (!o.replay.empty())
        return; // Saved addresses must never be mapped to today's hardware.
    auto devices = o.demo ? demo_devices() : scan_devices();
    if (o.bus)
        std::erase_if(devices, [&](const Device &d) { return d.bus != o.bus; });
    model.devices(devices, initial);
}
struct Engine {
    Source &source;
    Model &model;
    Recorder *recorder;
    const Options &options;
    Clock::time_point start{Clock::now()}, sampled{start}, scanned{start};
    Engine(Source &s, Model &m, Recorder *r, const Options &o)
        : source(s), model(m), recorder(r), options(o) {
        topology(model, options, true);
    }
    void tick() {
        auto now = Clock::now();
        if (std::chrono::duration<double>(now - scanned).count() >= 0.5) {
            topology(model, options);
            scanned = now;
        }
        consume(source, model, recorder);
        double seconds = std::chrono::duration<double>(now - sampled).count();
        if (seconds >= 0.5) {
            model.sample(seconds);
            model.dropped += source.dropped();
            sampled = now;
        }
    }
    bool done() const {
        return stopped || (options.duration > 0 &&
                           std::chrono::duration<double>(Clock::now() - start).count() >= options.duration);
    }
};
void render(const Model &model, const Source &source, const Options &o, std::string &selected, bool all,
            int filter, bool hex, size_t scroll, bool paused, bool help) {
    erase();
    int height, width;
    getmaxyx(stdscr, height, width);
    put(0, 2, "busb", 1, 6, true);
    std::string capture = o.demo              ? "synthetic"
                          : !o.replay.empty() ? "replay"
                          : o.bus             ? "bus " + std::to_string(o.bus)
                                              : "all buses";
    put(0, 8, "USB activity · " + capture, 7, std::max(0, width - 43));
    auto time = std::time(nullptr);
    char clock[12]{};
    std::strftime(clock, sizeof(clock), "%H:%M:%S", std::localtime(&time));
    const std::string state = paused              ? "Ⅱ FROZEN"
                              : source.finished() ? "○ EOF"
                              : o.demo            ? "● DEMO"
                              : !o.replay.empty() ? "▷ REPLAY"
                                                  : "● LIVE";
    if (width >= 80) {
        put(0, width - 32, state, paused ? 5 : 2, 14, true);
        if (!o.record.empty())
            put(0, width - 20, "● REC", 4, 6, true);
        put(0, width - 10, clock, 1, 8, true);
    }
    if (width < 80 || height < 24) {
        put(3, 2, "Resize to at least 80 columns × 24 rows. Capture continues.", 5);
        put(5, 2, "q quit   Space freeze display", 1);
        refresh();
        return;
    }
    std::vector<std::string> keys;
    int connected = 0, active = 0;
    for (const auto &[key, p] : model.ports) {
        if (!key.starts_with("bus") || p.counters.transfers)
            keys.push_back(key);
        if (p.device.connected)
            ++connected;
        if (p.counters.transfer_rate > 0)
            ++active;
    }
    if (selected.empty() && !keys.empty())
        selected = keys.front();
    auto it = model.ports.find(selected);
    const Port *port = it == model.ports.end() ? nullptr : &it->second;
    const auto &c = (!all && port) ? port->counters : model.total;
    std::string scope = all ? "all ports" : selected.empty() ? "waiting for a device" : selected;
    int top_h = height < 30 ? 8 : std::clamp(height / 3, 10, 20);
    box(1, 0, top_h, width, "1 traffic · " + scope);
    put(1, width - 25, " ↓ IN / OUT ↑  500ms ", 7, 23);
    int stats_w = 29, stats_x = width - stats_w - 2, chart_w = stats_x - 4;
    put(2, 2, "↓ device → host", 2, chart_w / 2, true);
    std::string scale = "peak " + human(peak_rate(c.in_history)) + "/s";
    put(2, stats_x - int(scale.size()) - 2, scale, 7, chart_w / 2);
    int plot_y = 3, plot_h = top_h - 4;
    int upper_h = std::max(1, (plot_h - 1) / 2), lower_h = std::max(1, plot_h - upper_h - 1);
    graph(plot_y, 2, upper_h, chart_w, c.in_history, 2);
    int axis = plot_y + upper_h;
    for (int col = 2; col < 2 + chart_w; ++col)
        put(axis, col, "─", 8, 1);
    put(axis, std::max(2, chart_w / 2 - 7), " IN ↓  ·  ↑ OUT ", 1, chart_w - 2, true);
    graph(axis + 1, 2, lower_h, chart_w, c.out_history, 3, true);
    put(top_h - 1, 2, "↑ host → device", 3, chart_w / 2, true);
    scale = "peak " + human(peak_rate(c.out_history)) + "/s";
    put(top_h - 1, stats_x - int(scale.size()) - 2, scale, 7, chart_w / 2);
    box(2, stats_x, top_h - 2, stats_w, "throughput", 7);
    int sx = stats_x + 2, sw = stats_w - 4;
    if (top_h >= 12) {
        put(3, sx, "↓ " + human(c.in_rate) + "/s", 2, sw, true);
        put(4, sx, "  total " + human(c.in), 7, sw);
        meter(5, sx, sw, c.in_rate / std::max(1.0, peak_rate(c.in_history)), 2);
        put(6, sx, "↑ " + human(c.out_rate) + "/s", 3, sw, true);
        put(7, sx, "  total " + human(c.out), 7, sw);
        meter(8, sx, sw, c.out_rate / std::max(1.0, peak_rate(c.out_history)), 3);
        put(9, sx, fixed(c.transfer_rate, 0) + " URBs/s", 1, sw, true);
        if (top_h >= 13)
            put(10, sx,
                std::to_string(c.errors) + " errors  ·  " + std::to_string(model.pending()) + " pending",
                c.errors ? 4 : 7, sw);
        if (top_h >= 15) {
            put(12, sx, std::to_string(active) + " active / " + std::to_string(connected) + " connected", 1,
                sw);
            put(13, sx, "scope: " + scope, 7, sw);
        }
        if (top_h >= 18) {
            put(15, sx, "completed USB transfers", 7, sw);
            put(16, sx, "IN = device → host", 2, sw);
        }
    } else {
        put(3, sx, "↓ " + human(c.in_rate) + "/s", 2, sw, true);
        put(4, sx, "↑ " + human(c.out_rate) + "/s", 3, sw, true);
        put(5, sx, fixed(c.transfer_rate, 0) + " URBs/s", 1, sw);
        put(6, sx, std::to_string(c.errors) + " errors", c.errors ? 4 : 7, sw);
        if (top_h >= 10)
            put(7, sx, "total ↓ " + human(c.in), 7, sw);
    }
    int middle_y = 1 + top_h, middle_h = std::clamp(height / 3, 8, 18);
    int split = width * 3 / 5;
    box(middle_y, 0, middle_h, split, "2 ports");
    put(middle_y, split - 18, " " + std::to_string(active) + " active · ↑↓ ", 7, 16);
    int port_w = split >= 66 ? 13 : 11;
    int in_x = split - 23, out_x = split - 12;
    bool sparks = split >= 76;
    int spark_x = in_x - 9;
    int name_x = port_w + 3, name_w = (sparks ? spark_x : in_x) - name_x - 1;
    put(middle_y + 1, 3, "PORT", 1, port_w, true);
    put(middle_y + 1, name_x, "DEVICE", 1, name_w, true);
    if (sparks)
        put(middle_y + 1, spark_x, "HISTORY", 7, 7);
    put(middle_y + 1, in_x, "      IN/s", 2, 10, true);
    put(middle_y + 1, out_x, "     OUT/s", 3, 10, true);
    size_t selected_index = std::find(keys.begin(), keys.end(), selected) - keys.begin();
    size_t capacity = middle_h - 3, offset = selected_index >= capacity ? selected_index - capacity + 1 : 0;
    for (size_t i = offset; i < keys.size() && i - offset < capacity; ++i) {
        const auto &p = model.ports.at(keys[i]);
        const auto &d = p.device;
        int row = middle_y + 2 + int(i - offset);
        bool chosen = keys[i] == selected;
        int color = chosen ? 6 : d.connected ? 9 : 7;
        if (chosen)
            put(row, 1, std::string(split - 2, ' '), 6, split - 2);
        put(row, 1,
            chosen                         ? "▸"
            : p.counters.transfer_rate > 0 ? "●"
                                           : "·",
            chosen                         ? 6
            : p.counters.transfer_rate > 0 ? 2
                                           : 7,
            1);
        put(row, 3, d.port, color, port_w - 1, chosen);
        put(row, name_x, (d.connected || keys[i].starts_with("bus") ? "" : "[gone] ") + d.name, color,
            name_w);
        if (sparks) {
            std::deque<double> history;
            for (size_t j = 0; j < p.counters.in_history.size(); ++j)
                history.push_back(p.counters.in_history[j] + p.counters.out_history[j]);
            graph(row, spark_x, 1, 7, history, chosen ? 6 : 2);
        }
        auto rate = [&](double n, int x, int direction_color) {
            std::string label = human(n);
            put(row, x, std::string(std::max(0, 10 - int(label.size())), ' ') + label,
                chosen  ? 6
                : n > 0 ? direction_color
                        : 7,
                10, chosen);
        };
        rate(p.counters.in_rate, in_x, 2);
        rate(p.counters.out_rate, out_x, 3);
    }
    if (keys.empty())
        put(middle_y + 2, 3, "Waiting for USB devices…", 7, split - 6);
    if (keys.size() > capacity)
        put(middle_y + middle_h - 1, 3,
            " " + std::to_string(std::min(selected_index + 1, keys.size())) + "/" +
                std::to_string(keys.size()) + " ports ",
            7, split - 6);
    box(middle_y, split, middle_h, width - split, "3 device · " + selected);
    int right = split + 2, room = width - split - 4;
    if (port) {
        const auto &d = port->device;
        const auto &p = port->counters;
        put(middle_y + 1, right, d.name, 1, room, true);
        put(middle_y + 2, right,
            (d.connected ? "● connected  " : "○ disconnected / unresolved  ") + d.speed + " Mb/s",
            d.connected ? 2 : 7, room);
        put(middle_y + 3, right,
            d.vendor + ":" + d.product + "  bus " + std::to_string(d.bus) + ":" + std::to_string(d.address),
            7, room);
        put(middle_y + 4, right,
            fixed(p.transfer_rate, 0) + " URBs/s  ·  " + std::to_string(p.errors) + " errors",
            p.errors ? 4 : 1, room);
        put(middle_y + 5, right,
            "latency " + fixed(p.paired ? p.latency_ms / p.paired : 0) + "ms / " + fixed(p.max_latency_ms) +
                "ms max",
            7, room);
        if (middle_h >= 9)
            put(middle_y + 6, right, "total ↓ " + human(p.in) + "  ↑ " + human(p.out), 7, room);
        int row = middle_y + 7;
        if (middle_h >= 11) {
            put(row++, right,
                "reenumerations " + std::to_string(port->reconnects) + "  DFU interfaces " +
                    std::to_string(d.dfu_interfaces.size()),
                7, room);
            if (!port->dfu_status.empty())
                put(row++, right, port->dfu_status, 5, room);
            else if (!d.serial.empty())
                put(row++, right, "serial " + d.serial, 7, room);
        }
        if (row < middle_y + middle_h - 2 && !port->endpoints.empty())
            put(row++, right, "ENDPOINT        RATE       URBs", 1, room);
        for (const auto &[ep, ec] : port->endpoints) {
            if (row >= middle_y + middle_h - 1)
                break;
            std::ostringstream label;
            label << "ep" << (ep & 15) << ((ep & 128) ? " ↓ IN  " : " ↑ OUT ") << std::setw(10)
                  << human((ep & 128) ? ec.in_rate : ec.out_rate) << "/s  " << ec.transfers;
            put(row++, right, label.str(), (ep & 128) ? 2 : 3, room);
        }
    } else
        put(middle_y + 2, right, "Waiting for port " + selected, 7, room);
    int log_y = middle_y + middle_h, log_h = height - log_y - 2;
    std::string filter_name = filter == 1 ? "DFU" : filter == 2 ? "errors" : "all";
    box(log_y, 0, log_h, width, "4 transfers · " + scope);
    put(log_y, width - 28,
        " " + filter_name + " · " + (scroll ? "history" : "live") + (hex ? " · hex " : " "), 7, 25);
    bool compact = width < 110;
    int port_x = compact ? 11 : 15, ep_x = compact ? 23 : 28, dir_x = compact ? 26 : 32;
    int type_x = compact ? 31 : 38, bytes_x = compact ? 36 : 44;
    int result_x = compact ? 47 : 56, latency_x = compact ? 57 : 67, detail_x = compact ? 66 : 78;
    put(log_y + 1, 2, "TIME", 1, 8, true);
    put(log_y + 1, port_x, "PORT", 1, 11, true);
    put(log_y + 1, ep_x, "EP", 1, 2, true);
    put(log_y + 1, dir_x, "DIR", 1, 4, true);
    put(log_y + 1, type_x, "TYPE", 1, 4, true);
    put(log_y + 1, bytes_x, "     BYTES", 1, 10, true);
    put(log_y + 1, result_x, "RESULT", 1, 9, true);
    put(log_y + 1, latency_x, "LATENCY", 1, 8, true);
    put(log_y + 1, detail_x, "REQUEST / PAYLOAD", 1, width - detail_x - 2, true);
    int line = log_y + 2, max_line = log_y + log_h - 1;
    size_t skipped = 0;
    std::set<std::pair<int, uint64_t>> completed;
    for (auto rit = model.transfers.rbegin(); rit != model.transfers.rend() && line < max_line; ++rit) {
        const auto &t = *rit;
        if (!all && t.port != selected)
            continue;
        if (filter == 1 && !t.dfu)
            continue;
        if (filter == 2 && (!t.complete || t.status >= 0))
            continue;
        auto id = std::make_pair(t.bus, t.id);
        if (t.complete)
            completed.insert(id);
        else if (completed.erase(id))
            continue;
        if (skipped++ < scroll)
            continue;
        long long ms = static_cast<long long>(t.timestamp * 1000);
        std::ostringstream stamp;
        stamp << std::setfill('0') << std::setw(2) << (ms / 3600000 % 24) << ':' << std::setw(2)
              << (ms / 60000 % 60) << ':' << std::setw(2) << (ms / 1000 % 60) << '.' << std::setw(3)
              << (ms % 1000);
        put(line, 2, stamp.str(), 7, compact ? 8 : 12);
        put(line, port_x, t.port, 9, compact ? 11 : 12);
        put(line, ep_x, std::to_string(t.endpoint), 7, 2);
        put(line, dir_x, t.input ? "IN ↓" : "OUT ↑", t.input ? 2 : 3, compact ? 4 : 5);
        put(line, type_x, t.kind, 7, 4);
        std::string bytes = human(t.length);
        put(line, bytes_x, std::string(std::max(0, 10 - int(bytes.size())), ' ') + bytes, 9, 10);
        put(line, result_x, t.complete ? status_name(t.status) : "SUBMIT",
            t.complete && t.status < 0 ? 4
            : t.complete               ? 2
                                       : 7,
            compact ? 9 : 10);
        put(line, latency_x, t.latency_ms < 0 ? "—" : fixed(t.latency_ms) + "ms", 7, compact ? 8 : 9);
        std::string detail = t.description;
        if (hex && !t.payload.empty())
            detail += (detail.empty() ? "" : "  │  ") + t.payload;
        put(line, detail_x, detail, t.dfu ? 5 : 7, width - detail_x - 2);
        ++line;
    }
    if (line == log_y + 2)
        put(line, 3,
            filter == 0 ? "Waiting for USB transfers…" : "No matching transfers. d / e change the filter.", 7,
            width - 6);
    put(height - 2, 2,
        std::to_string(connected) + " connected · " + std::to_string(active) + " active  │  dropped " +
            std::to_string(model.dropped) + "  unpaired " + std::to_string(model.unpaired) + "  expired " +
            std::to_string(model.expired) + (model.lifecycle.empty() ? "" : "  │  " + model.lifecycle.back()),
        model.dropped ? 4 : 7, width - 4);
    put(height - 1, 2,
        "↑↓ select  a scope  d DFU  e errors  h hex  Space freeze  PgUp/Dn history  ? help  q quit", 1,
        width - 4);
    if (help) {
        int w = std::min(86, width - 4), x = (width - w) / 2, y = (height - 19) / 2;
        for (int r = y; r < y + 19; ++r)
            mvhline(r, x, ' ', w);
        box(y, x, 19, w, "busb · controls & capture semantics");
        const std::vector<std::string> lines = {
            "↑/↓ or j/k   Select physical USB port (follows re-enumeration)",
            "a            Switch selected-port / all-port traffic",
            "d / e        Toggle DFU-only / transfer-error filter",
            "h            Toggle captured payload hex preview",
            "Space        Freeze display; capture and recording continue",
            "PgUp/PgDn    Browse retained transfer history; Home = live",
            "? / q        Close help / quit",
            "",
            "Rates count completed URB actual lengths once, in host directions.",
            "Latency measures submission → callback, not device processing alone.",
            "DFU decoding requires a DFU interface or --dfu-interface N.",
            "Data is limited by --snaplen and the kernel's own capture limits.",
            "usbmon observes host requests, not wire packets/NAKs/bus voltage.",
            "Recorded pcap covers the capture bus; UI filters affect display only.",
            "Replay has no saved device descriptors; use --dfu-interface as needed.",
            "Port lifecycle is sampled every 500ms; very short resets may be missed."};
        for (size_t i = 0; i < lines.size(); ++i)
            put(y + 1 + i, x + 2, lines[i], 0, w - 4);
    }
    refresh();
}
} // namespace
void run_ui(Source &source, Model &model, Recorder *recorder, const Options &options) {
    Terminal terminal;
    Signals signals;
    Engine engine(source, model, recorder, options);
    std::string selected = options.port;
    bool all = options.port.empty(), hex = true, paused = false, help = false;
    int filter = 0;
    size_t scroll = 0;
    std::optional<Model> frozen;
    auto drawn = Clock::now() - std::chrono::seconds(1);
    while (!engine.done()) {
        engine.tick();
        bool redraw = false;
        int key = getch();
        if (key != ERR) {
            redraw = true;
            if (help && (key == '?' || key == 'q' || key == 27))
                help = false;
            else if (key == 'q' || key == 27)
                break;
            else if (key == '?')
                help = !help;
            else if (key == 'a') {
                all = !all;
                scroll = 0;
            } else if (key == 'h')
                hex = !hex;
            else if (key == 'd') {
                filter = filter == 1 ? 0 : 1;
                scroll = 0;
            } else if (key == 'e') {
                filter = filter == 2 ? 0 : 2;
                scroll = 0;
            } else if (key == ' ') {
                paused = !paused;
                if (paused)
                    frozen = model;
                else
                    frozen.reset();
            } else if (key == KEY_PPAGE)
                scroll = std::min<size_t>(1990, scroll + 5);
            else if (key == KEY_NPAGE)
                scroll = scroll > 5 ? scroll - 5 : 0;
            else if (key == KEY_HOME)
                scroll = 0;
            else if (key == KEY_UP || key == KEY_DOWN || key == 'j' || key == 'k') {
                const auto &current = frozen ? *frozen : model;
                std::vector<std::string> keys;
                for (const auto &[port, p] : current.ports)
                    if (!port.starts_with("bus") || p.counters.transfers)
                        keys.push_back(port);
                if (!keys.empty()) {
                    auto pos = std::find(keys.begin(), keys.end(), selected);
                    int index = pos == keys.end() ? 0 : int(pos - keys.begin());
                    index =
                        std::clamp(index + ((key == KEY_UP || key == 'k') ? -1 : 1), 0, int(keys.size()) - 1);
                    selected = keys[index];
                    all = false;
                    scroll = 0;
                }
            }
        }
        auto now = Clock::now();
        if (redraw || now - drawn >= std::chrono::milliseconds(100)) {
            render(frozen ? *frozen : model, source, options, selected, all, filter, hex, scroll, paused,
                   help);
            drawn = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    model.dropped += source.dropped();
}
void run_headless(Source &source, Model &model, Recorder *recorder, const Options &options) {
    Signals signals;
    Engine engine(source, model, recorder, options);
    while (!engine.done() && !source.finished()) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    model.dropped += source.dropped();
    std::cout << source.label() << '\n';
    for (const auto &[key, p] : model.ports) {
        if (!options.port.empty() && key != options.port)
            continue;
        std::cout << key << "  " << p.device.name << "  IN " << human(p.counters.in) << "  OUT "
                  << human(p.counters.out) << "  URBs " << p.counters.transfers << "  errors "
                  << p.counters.errors;
        if (!p.dfu_status.empty())
            std::cout << "  " << p.dfu_status;
        std::cout << '\n';
    }
    std::cout << "dropped=" << model.dropped << " unpaired=" << model.unpaired << " expired=" << model.expired
              << '\n';
}
} // namespace busb
