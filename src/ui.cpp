#include "core.hpp"

#include <algorithm>
#include <atomic>
#include <clocale>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <curses.h>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace busb {
namespace {
std::atomic<int> stopped{0};
void stop(int) { stopped.store(1); }
struct Signals {
    using Handler = void (*)(int);
    Handler old_int, old_term;
    Signals() : old_int(std::signal(SIGINT, stop)), old_term(std::signal(SIGTERM, stop)) { stopped.store(0); }
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
enum class Layout { Overview, Device, Transfers };
struct UiState {
    Layout layout{Layout::Overview};
    std::string selected, query;
    bool all{true}, dfu{}, errors{}, hex{true}, paused{}, help{}, detail{}, hide_idle{}, querying{};
    int type{-1}, direction{}, endpoint{-1}, interval_ms{500};
    size_t cursor{};
    std::optional<Model> frozen;
};
struct LogRow {
    Transfer transfer;
    uint32_t count{1};
};
const char *kind_names[] = {"ISO", "INTR", "CTRL", "BULK"};
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
std::string lower_copy(std::string text) {
    for (char &c : text)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return text;
}
std::string pace(int ms) { return ms % 1000 == 0 ? std::to_string(ms / 1000) + "s" : std::to_string(ms) + "ms"; }
std::string percent_of(double rate, double ceiling) {
    if (ceiling <= 0)
        return {};
    double pct = 100.0 * rate / ceiling;
    if (rate > 0 && pct < 1)
        return "<1%";
    return fixed(pct, pct < 10 ? 1 : 0) + "%";
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
double shared_peak(const Counters &c) {
    return std::max(peak_rate(c.in_history), peak_rate(c.out_history));
}
double link_bytes(const Device &device) { return device.megabits > 0 ? device.megabits * 1e6 / 8.0 : 0; }
void graph(int y, int x, int height, int width, const std::deque<double> &history, int color, bool inverted,
           double peak, bool baseline = true) {
    if (height < 1 || width < 1)
        return;
    if (peak < 1)
        peak = 1;
    // The retained window always spans the widget. Partial history stays on the right.
    const int window = int(rate_samples);
    int slots = width * 2;
    static const unsigned bits[2][4] = {{6, 2, 1, 0}, {7, 5, 4, 3}};
    for (int column = 0; column < width; ++column) {
        double values[2]{};
        bool available = false;
        for (int side = 0; side < 2; ++side) {
            int slot = column * 2 + side;
            int begin = slot * window / slots;
            int end = (slot + 1) * window / slots;
            if (end <= begin)
                end = begin + 1;
            double value = 0;
            bool any = false;
            for (int pos = begin; pos < end; ++pos) {
                long long index = static_cast<long long>(history.size()) - window + pos;
                if (index >= 0 && index < static_cast<long long>(history.size())) {
                    value = std::max(value, history[size_t(index)]);
                    any = true;
                }
            }
            if (any) {
                values[side] = value / peak * height * 4;
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
            else if (baseline && row == 0)
                put(screen_row, x + column, "╌", color == 6 ? 6 : 7, 1);
            else if (baseline && available && column % 8 == 0)
                put(screen_row, x + column, "·", 7, 1);
        }
    }
}
void meter(int y, int x, int width, double ratio, int color) {
    int filled = std::clamp(int(std::round(ratio * width)), 0, width);
    for (int i = 0; i < width; ++i)
        put(y, x + i, i < filled ? "━" : "─", i < filled ? color : 7, 1, i < filled);
}
std::string stamp_of(double timestamp, bool demo) {
    long long ms = std::llround(timestamp * 1000.0);
    if (ms < 0)
        ms = 0;
    std::ostringstream out;
    out << std::setfill('0');
    if (demo) {
        long long milli = ms % 1000, secs = ms / 1000;
        long long h = secs / 3600, m = (secs / 60) % 60, s = secs % 60;
        out << '+';
        if (h)
            out << h << ':' << std::setw(2);
        out << m << ':' << std::setw(2) << s << '.' << std::setw(3) << milli;
        return out.str();
    }
    time_t sec = time_t(ms / 1000);
    std::tm tm{};
    localtime_r(&sec, &tm);
    out << std::setw(2) << tm.tm_hour << ':' << std::setw(2) << tm.tm_min << ':' << std::setw(2) << tm.tm_sec
        << '.' << std::setw(3) << (ms % 1000);
    return out.str();
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
    uint32_t kernel_drops{};
    bool eof{false};
    Engine(Source &s, Model &m, Recorder *r, const Options &o)
        : source(s), model(m), recorder(r), options(o) {
        topology(model, options, true);
    }
    // usbmon's dropped counter is cumulative (and some versions reset on read).
    void note_drops() {
        uint32_t kernel = source.dropped();
        uint32_t delta = kernel >= kernel_drops ? kernel - kernel_drops : kernel;
        kernel_drops = kernel;
        model.interval_dropped = delta;
        model.dropped += delta;
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
            note_drops();
            sampled = now;
        }
        eof = source.finished();
    }
    bool done() const {
        return stopped.load() || (options.duration > 0 && std::chrono::duration<double>(Clock::now() - start)
                                                                .count() >= options.duration);
    }
};
bool recent(const Port &port) {
    auto hot = [](const std::deque<double> &history) {
        size_t n = std::min<size_t>(6, history.size());
        for (size_t i = history.size() - n; i < history.size(); ++i)
            if (history[i] > 0)
                return true;
        return false;
    };
    return port.counters.transfer_rate > 0 || port.counters.last_error != 0 || hot(port.counters.in_history) ||
           hot(port.counters.out_history);
}
std::string kind_label(int type) { return type >= 0 && type < 4 ? kind_names[type] : ""; }
bool query_in(const std::string &hay, const std::string &query) {
    return query.empty() || lower_copy(hay).find(query) != std::string::npos;
}
std::vector<std::string> ranked_ports(const Model &model, const UiState &ui) {
    std::string query = lower_copy(ui.query);
    std::vector<std::string> keys;
    for (const auto &[key, port] : model.ports) {
        if (key.starts_with("bus") && !port.counters.transfers)
            continue;
        if (ui.hide_idle && key != ui.selected && !recent(port))
            continue;
        const auto &device = port.device;
        if (!query_in(key + " " + device.name + " " + device.vendor + ":" + device.product + " " +
                          device.class_summary + " " + device.serial,
                      query))
            continue;
        keys.push_back(key);
    }
    std::stable_sort(keys.begin(), keys.end(), [&](const std::string &a, const std::string &b) {
        const auto &pa = model.ports.at(a).counters;
        const auto &pb = model.ports.at(b).counters;
        double ba = pa.in_rate + pa.out_rate, bb = pb.in_rate + pb.out_rate;
        if (ba != bb)
            return ba > bb;
        if (pa.errors != pb.errors)
            return pa.errors > pb.errors;
        return a < b;
    });
    return keys;
}
bool transfer_match(const Transfer &t, const Model &model, const UiState &ui) {
    if (!ui.all && t.port != ui.selected)
        return false;
    if (ui.dfu && !t.dfu)
        return false;
    if (ui.errors && !(t.complete && t.status < 0))
        return false;
    if (ui.type >= 0 && t.kind != kind_label(ui.type))
        return false;
    if (ui.direction == 1 && !t.input)
        return false;
    if (ui.direction == 2 && t.input)
        return false;
    if (ui.endpoint >= 0 && t.endpoint != ui.endpoint)
        return false;
    if (!ui.query.empty()) {
        std::string name;
        auto it = model.ports.find(t.port);
        if (it != model.ports.end())
            name = it->second.device.name + " " + it->second.device.vendor + ":" + it->second.device.product +
                   " " + it->second.device.class_summary;
        if (!query_in(t.port + " " + name + " " + t.description + " " + t.kind + " " + t.payload + " ep" +
                          std::to_string(t.endpoint) + " " + status_name(t.status),
                      lower_copy(ui.query)))
            return false;
    }
    return true;
}
bool mergeable(const Transfer &a, const Transfer &b) {
    if (!a.complete || !b.complete || a.kind == "CTRL" || b.kind == "CTRL")
        return false;
    if (a.status < 0 || b.status < 0 || a.dfu || b.dfu)
        return false;
    return a.port == b.port && a.endpoint == b.endpoint && a.input == b.input && a.length == b.length &&
           a.status == b.status && a.kind == b.kind && a.description == b.description;
}
std::vector<LogRow> coalesce(const Model &model, const UiState &ui) {
    std::vector<LogRow> rows;
    std::set<std::pair<int, uint64_t>> completed;
    for (auto it = model.transfers.rbegin(); it != model.transfers.rend(); ++it) {
        const auto &t = *it;
        auto id = std::make_pair(t.bus, t.id);
        if (t.complete)
            completed.insert(id);
        else if (completed.erase(id))
            continue;
        // Hidden rows do not break a run. A visible control transfer or error does,
        // because those rows are never mergeable with the one above them.
        if (!transfer_match(t, model, ui))
            continue;
        if (!rows.empty() && mergeable(rows.back().transfer, t)) {
            ++rows.back().count;
            continue;
        }
        rows.push_back({t, 1});
    }
    return rows;
}
std::string filter_text(const UiState &ui) {
    std::string text;
    auto add = [&](const std::string &part) {
        if (part.empty())
            return;
        if (!text.empty())
            text += " · ";
        text += part;
    };
    if (ui.dfu)
        add("DFU");
    if (ui.errors)
        add("errors");
    add(kind_label(ui.type));
    if (ui.direction == 1)
        add("IN");
    else if (ui.direction == 2)
        add("OUT");
    if (ui.endpoint >= 0)
        add("ep" + std::to_string(ui.endpoint));
    if (!ui.query.empty())
        add("/" + ui.query);
    if (ui.hide_idle)
        add("idle hidden");
    return text.empty() ? "all" : text;
}
void set_layout(UiState &ui, Layout layout) {
    ui.layout = layout;
    if (layout != Layout::Transfers)
        ui.detail = false;
}
void cycle_layout(UiState &ui, int delta) {
    int index = int(ui.layout) + delta;
    index = (index % 3 + 3) % 3;
    set_layout(ui, Layout(index));
}
void move_port(UiState &ui, const std::vector<std::string> &keys, int delta) {
    if (keys.empty())
        return;
    auto pos = std::find(keys.begin(), keys.end(), ui.selected);
    int index = pos == keys.end() ? 0 : int(pos - keys.begin());
    index = std::clamp(index + delta, 0, int(keys.size()) - 1);
    ui.selected = keys[index];
    ui.all = false;
    ui.cursor = 0;
}
bool handle_key(int key, UiState &ui, const std::vector<std::string> &keys) {
    if (ui.querying) {
        if (key == 27) {
            ui.query.clear();
            ui.querying = false;
        } else if (key == '\n' || key == KEY_ENTER)
            ui.querying = false;
        else if (key == KEY_BACKSPACE || key == 127 || key == '\b') {
            if (!ui.query.empty())
                ui.query.pop_back();
        } else if (key >= 32 && key < 127 && ui.query.size() < 40)
            ui.query.push_back(char(key));
        ui.cursor = 0;
        return false;
    }
    if (key == 27) {
        if (ui.help)
            ui.help = false;
        else if (ui.detail)
            ui.detail = false;
        else
            return true;
        return false;
    }
    if (key == 'q') {
        if (ui.help)
            ui.help = false;
        else
            return true;
        return false;
    }
    if (key == '?')
        ui.help = !ui.help;
    else if (key == '1')
        set_layout(ui, Layout::Overview);
    else if (key == '2')
        set_layout(ui, Layout::Device);
    else if (key == '3')
        set_layout(ui, Layout::Transfers);
    else if (key == '\t')
        cycle_layout(ui, 1);
    else if (key == KEY_BTAB)
        cycle_layout(ui, -1);
    else if (key == '\n' || key == KEY_ENTER) {
        if (ui.layout == Layout::Transfers)
            ui.detail = !ui.detail;
        else if (!ui.selected.empty())
            set_layout(ui, Layout::Device);
    } else if (key == 'a') {
        ui.all = !ui.all;
        ui.cursor = 0;
    } else if (key == 'h')
        ui.hex = !ui.hex;
    else if (key == 'd') {
        ui.dfu = !ui.dfu;
        ui.cursor = 0;
    } else if (key == 'e') {
        ui.errors = !ui.errors;
        ui.cursor = 0;
    } else if (key == 'i')
        ui.hide_idle = !ui.hide_idle;
    else if (key == 't') {
        const int order[] = {-1, 2, 3, 1, 0};
        int index = 0;
        for (int i = 0; i < 5; ++i)
            if (order[i] == ui.type)
                index = i;
        ui.type = order[(index + 1) % 5];
        ui.cursor = 0;
    } else if (key == 'm') {
        ui.direction = (ui.direction + 1) % 3;
        ui.cursor = 0;
    } else if (key == 'f') {
        ui.endpoint = ui.endpoint >= 15 ? -1 : ui.endpoint + 1;
        ui.cursor = 0;
    } else if (key == '/' )
        ui.querying = true;
    else if (key == ' ')
        ui.paused = !ui.paused;
    else if (key == '+' || key == '=') {
        const int steps[] = {200, 500, 1000, 2000};
        int best = 0;
        for (int i = 0; i < 4; ++i)
            if (std::abs(steps[i] - ui.interval_ms) < std::abs(steps[best] - ui.interval_ms))
                best = i;
        ui.interval_ms = steps[std::min(3, best + 1)];
    } else if (key == '-' || key == '_') {
        const int steps[] = {200, 500, 1000, 2000};
        int best = 0;
        for (int i = 0; i < 4; ++i)
            if (std::abs(steps[i] - ui.interval_ms) < std::abs(steps[best] - ui.interval_ms))
                best = i;
        ui.interval_ms = steps[std::max(0, best - 1)];
    } else if (key == KEY_HOME)
        ui.cursor = 0;
    else if (key == KEY_PPAGE)
        ui.cursor += ui.layout == Layout::Transfers ? 8 : 0;
    else if (key == KEY_NPAGE)
        ui.cursor = ui.cursor > 8 ? ui.cursor - 8 : 0;
    else if (key == KEY_UP || key == 'k' || key == KEY_DOWN || key == 'j') {
        int delta = key == KEY_UP || key == 'k' ? -1 : 1;
        if (ui.layout == Layout::Transfers) {
            if (delta < 0 && ui.cursor > 0)
                --ui.cursor;
            else if (delta > 0)
                ++ui.cursor;
        } else
            move_port(ui, keys, delta);
    }
    return false;
}
void draw_header(int width, const Options &o, bool eof, const UiState &ui) {
    put(0, 2, "busb", 1, 6, true);
    std::string capture = o.demo ? "synthetic" : !o.replay.empty() ? "replay" : o.bus ? "bus " + std::to_string(o.bus) : "all buses";
    std::string layout = ui.layout == Layout::Overview ? "overview" : ui.layout == Layout::Device ? "device" : "transfers";
    put(0, 8, "USB activity · " + capture + " · " + layout, 7, std::max(0, width - 54));
    auto time = std::time(nullptr);
    char clock[12]{};
    std::strftime(clock, sizeof(clock), "%H:%M:%S", std::localtime(&time));
    const std::string mode = ui.paused ? "Ⅱ FROZEN" : eof ? "○ EOF" : o.demo ? "● DEMO" : !o.replay.empty() ? "▷ REPLAY" : "● LIVE";
    if (width >= 80) {
        put(0, width - 44, mode, ui.paused ? 5 : 2, 12, true);
        put(0, width - 31, pace(ui.interval_ms), 7, 6, true);
        if (!o.record.empty())
            put(0, width - 22, "● REC", 4, 6, true);
        put(0, width - 10, clock, 1, 8, true);
    }
}
void draw_traffic(int y, int x, int h, int w, const Counters &counters, const Device *device,
                  const std::string &scope, size_t pending) {
    if (h < 6 || w < 24)
        return;
    box(y, x, h, w, "traffic · " + scope);
    std::string window = "90s";
    put(y, x + w - int(window.size()) - 4, " " + window + " ", 7, int(window.size()) + 2);
    bool side = w >= 74 && h >= 7;
    int stats_w = side ? 30 : 0;
    int chart_x = x + 2, chart_w = std::max(8, w - 4 - (side ? stats_w + 1 : 0));
    int top = y + 1, bottom = y + h - 2;
    put(top, chart_x, "↓ device → host", 2, chart_w / 2, true);
    double peak = std::max(1.0, shared_peak(counters));
    std::string scale = "scale " + human(peak) + "/s";
    put(top, chart_x + std::max(0, chart_w - int(scale.size())), scale, 7, int(scale.size()));
    int plot_y = top + 1, plot_h = std::max(3, bottom - plot_y);
    int upper = std::max(1, (plot_h - 1) / 2), lower = std::max(1, plot_h - upper - 1);
    int axis = plot_y + upper;
    graph(plot_y, chart_x, upper, chart_w, counters.in_history, 2, false, peak);
    for (int col = 0; col < chart_w; ++col)
        put(axis, chart_x + col, "─", 8, 1);
    put(axis, chart_x + std::max(0, chart_w / 2 - 8), " IN ↓  ·  ↑ OUT ", 1, chart_w, true);
    graph(axis + 1, chart_x, lower, chart_w, counters.out_history, 3, true, peak);
    put(bottom, chart_x, "↑ host → device", 3, chart_w / 2, true);
    if (!side)
        return;
    int stats_x = x + w - stats_w - 1;
    box(y + 1, stats_x, h - 2, stats_w, "throughput", 7);
    int sx = stats_x + 2, sw = stats_w - 4, row = y + 2, limit = y + h - 3;
    double link = device ? link_bytes(*device) : 0;
    double ceiling = link > 0 ? link : peak;
    auto rate_line = [&](const char *arrow, double rate, int color) {
        if (row > limit)
            return;
        put(row++, sx, std::string(arrow) + " " + human(rate) + "/s", color, sw, true);
        if (link > 0 && row <= limit)
            put(row++, sx, percent_of(rate, link) + " of " + device->speed + " Mb/s", 7, sw);
        if (h >= 12 && row <= limit)
            meter(row++, sx, sw, rate / std::max(1.0, ceiling), color);
    };
    rate_line("↓", counters.in_rate, 2);
    rate_line("↑", counters.out_rate, 3);
    if (row <= limit)
        put(row++, sx, fixed(counters.transfer_rate, 0) + " URBs/s", 1, sw, true);
    if (row <= limit) {
        std::string errors = std::to_string(counters.errors) + " errors";
        if (counters.last_error)
            errors += " · " + status_name(counters.last_error);
        else
            errors += " · " + std::to_string(pending) + " pending";
        put(row++, sx, errors, counters.errors ? 4 : 7, sw);
    }
    if (row <= limit) {
        std::string latency = counters.window_max_latency_ms > 0
                                  ? "latency " + fixed(counters.window_latency_ms) + " / " +
                                        fixed(counters.window_max_latency_ms) + " max"
                                  : "latency —";
        put(row, sx, latency, 7, sw);
    }
}
void draw_ports(int y, int x, int h, int w, const Model &model, const std::vector<std::string> &keys,
                const std::string &selected) {
    if (h < 3 || w < 20)
        return;
    box(y, x, h, w, "ports · " + std::to_string(keys.size()));
    // Rates first, then errors. The sparkline uses whatever width is left.
    int right = x + w - 2;
    auto take = [&](int width) {
        right -= width;
        return right;
    };
    bool show_err = w >= 68, show_last = w >= 88, show_urb = w >= 104;
    int last_x = show_last ? take(10) : 0;
    int err_x = show_err ? take(5) : 0;
    int urb_x = show_urb ? take(8) : 0;
    int out_x = take(10);
    int in_x = take(10);
    int text_w = right - (x + 3);
    bool spark = text_w >= 28;
    int spark_x = spark ? take(8) : 0;
    int port_x = x + 3;
    int port_w = std::min(12, std::max(6, text_w / 4));
    int class_w = text_w >= 40 ? 14 : 0;
    int class_x = port_x + port_w;
    int name_x = class_x + class_w;
    int name_w = std::max(0, (spark ? spark_x : in_x) - name_x - 1);
    auto header = [&](int col, const char *text, int width, int color) {
        if (width > 0)
            put(y + 1, col, text, color, width, true);
    };
    header(port_x, "PORT", port_w, 1);
    header(class_x, "CLASS", class_w, 1);
    header(name_x, "DEVICE", name_w, 1);
    header(in_x, "      IN/s", 10, 2);
    header(out_x, "     OUT/s", 10, 3);
    header(urb_x, "  URBs/s", show_urb ? 8 : 0, 1);
    header(err_x, " ERR", show_err ? 6 : 0, 4);
    header(last_x, "LAST", show_last ? 10 : 0, 4);
    size_t capacity = size_t(std::max(1, h - 3));
    auto found = std::find(keys.begin(), keys.end(), selected);
    size_t selected_index = found == keys.end() ? 0 : size_t(found - keys.begin());
    size_t offset = found != keys.end() && selected_index >= capacity ? selected_index + 1 - capacity : 0;
    if (keys.empty())
        put(y + 2, x + 3, "Waiting for USB devices…", 7, w - 6);
    for (size_t i = offset; i < keys.size() && i - offset < capacity; ++i) {
        const auto &port = model.ports.at(keys[i]);
        const auto &device = port.device;
        int row = y + 2 + int(i - offset);
        bool chosen = keys[i] == selected;
        int color = chosen ? 6 : !device.connected || device.hub ? 7 : 9;
        if (chosen)
            put(row, x + 1, std::string(size_t(std::max(0, w - 2)), ' '), 6, w - 2);
        put(row, x + 1, chosen ? "▸" : port.counters.transfer_rate > 0 ? "●" : "·",
            chosen ? 6 : port.counters.transfer_rate > 0 ? 2 : 7, 1);
        put(row, port_x, device.port.empty() ? keys[i] : device.port, color, port_w - 1, chosen);
        if (class_w)
            put(row, class_x, device.class_summary, chosen ? 6 : 7, class_w - 1, chosen);
        std::string name = (!device.connected && !keys[i].starts_with("bus") ? "[gone] " : "") + device.name;
        if (!class_w && !device.class_summary.empty())
            name = device.class_summary + " · " + name;
        put(row, name_x, name, color, name_w, chosen);
        if (spark) {
            std::deque<double> history;
            size_t n = std::min(port.counters.in_history.size(), port.counters.out_history.size());
            for (size_t sample = 0; sample < n; ++sample)
                history.push_back(port.counters.in_history[sample] + port.counters.out_history[sample]);
            double ceiling = link_bytes(device);
            if (ceiling <= 0)
                ceiling = std::max(1.0, peak_rate(history));
            graph(row, spark_x, 1, 7, history, chosen ? 6 : 2, false, ceiling, false);
        }
        auto rate = [&](double value, int col, int hue) {
            std::string label = human(value);
            put(row, col, std::string(size_t(std::max(0, 10 - int(label.size()))), ' ') + label,
                chosen ? 6 : value > 0 ? hue : 7, 10, chosen);
        };
        rate(port.counters.in_rate, in_x, 2);
        rate(port.counters.out_rate, out_x, 3);
        if (show_urb) {
            std::string label = fixed(port.counters.transfer_rate, 0);
            put(row, urb_x, std::string(size_t(std::max(0, 8 - int(label.size()))), ' ') + label,
                chosen ? 6 : 1, 8, chosen);
        }
        if (show_err)
            put(row, err_x, std::to_string(port.counters.errors),
                chosen ? 6 : port.counters.errors ? 4 : 7, 6, chosen);
        if (show_last)
            put(row, last_x, port.counters.last_error ? status_name(port.counters.last_error) : "·",
                chosen ? 6 : port.counters.last_error ? 4 : 7, 10, chosen);
    }
    if (keys.size() > capacity)
        put(y + h - 1, x + 3,
            " " + std::to_string(std::min(selected_index + 1, keys.size())) + "/" + std::to_string(keys.size()) +
                " ",
            7, w - 6);
}
void draw_device(int y, int x, int h, int w, const Port *port, const std::string &selected) {
    if (h < 3 || w < 16)
        return;
    box(y, x, h, w, "device · " + (selected.empty() ? "none" : selected));
    if (!port) {
        put(y + 2, x + 2, "Waiting for a port", 7, w - 4);
        return;
    }
    const auto &device = port->device;
    const auto &counters = port->counters;
    int left = x + 2, room = w - 4, row = y + 1, limit = y + h - 2;
    auto line = [&](const std::string &text, int color, bool bold = false) {
        if (row <= limit)
            put(row++, left, text, color, room, bold);
    };
    line(device.name, 1, true);
    if (!device.class_summary.empty())
        line(device.class_summary, 7);
    double link = link_bytes(device);
    std::string presence = (device.connected ? "● connected  " : "○ disconnected  ") + device.speed + " Mb/s";
    if (link > 0)
        presence += "  ·  " + percent_of(std::max(counters.in_rate, counters.out_rate), link) + " of link";
    line(presence, device.connected ? 2 : 7);
    line(device.vendor + ":" + device.product + "  bus " + std::to_string(device.bus) + ":" +
             std::to_string(device.address),
         7);
    line(fixed(counters.transfer_rate, 0) + " URBs/s  ·  " + std::to_string(counters.errors) + " errors",
         counters.errors ? 4 : 1);
    line(counters.window_max_latency_ms > 0 ? "latency " + fixed(counters.window_latency_ms) + "ms / " +
                                                   fixed(counters.window_max_latency_ms) + "ms window"
                                             : "latency —",
         7);
    if (counters.last_error)
        line("last error " + status_name(counters.last_error), 4, true);
    line("total ↓ " + human(counters.in) + "  ↑ " + human(counters.out), 7);
    line("reenumerations " + std::to_string(port->reconnects), 7);
    if (!device.interfaces_text.empty())
        line(device.interfaces_text, 7);
    if (!port->dfu_status.empty())
        line(port->dfu_status, 5);
    else if (!device.serial.empty())
        line("serial " + device.serial, 7);
    if (row > limit || port->endpoints.empty())
        return;
    line("ENDPOINT     RATE       URBs  ERR", 1);
    std::vector<std::pair<int, const Counters *>> endpoints;
    for (const auto &[ep, counters_ep] : port->endpoints)
        endpoints.push_back({ep, &counters_ep});
    std::sort(endpoints.begin(), endpoints.end(), [](const auto &a, const auto &b) {
        double ra = a.second->in_rate + a.second->out_rate, rb = b.second->in_rate + b.second->out_rate;
        if (ra != rb)
            return ra > rb;
        return a.first < b.first;
    });
    for (const auto &[ep, counters_ep] : endpoints) {
        if (row > limit)
            break;
        bool input = ep & 128;
        double rate = input ? counters_ep->in_rate : counters_ep->out_rate;
        std::ostringstream label;
        label << "ep" << (ep & 15) << (input ? " IN  " : " OUT ") << std::setw(10) << human(rate) << "/s  "
              << counters_ep->transfers;
        if (counters_ep->errors)
            label << "  " << counters_ep->errors;
        put(row++, left, label.str(), counters_ep->errors ? 4 : input ? 2 : 3, room);
    }
}
void draw_log(int y, int x, int h, int w, const std::vector<LogRow> &rows, const UiState &ui, bool selectable,
              bool demo) {
    if (h < 3 || w < 20)
        return;
    std::string scope = ui.all ? "all ports" : ui.selected.empty() ? "no port" : ui.selected;
    std::string title = "transfers · " + scope;
    std::string chips = filter_text(ui);
    if (chips != "all")
        title += " · " + chips;
    box(y, x, h, w, title);
    std::string corner = std::string(ui.cursor ? "history" : "live") + (ui.hex ? " · hex" : "");
    put(y, x + w - int(corner.size()) - 4, " " + corner + " ", 7, int(corner.size()) + 2);
    bool compact = w < 110;
    int time_w = 12;
    int port_x = x + 2 + time_w + 1;
    int port_w = compact ? 10 : 12;
    int ep_x = port_x + port_w, dir_x = ep_x + 3, type_x = dir_x + (compact ? 5 : 6);
    int bytes_x = type_x + 5, result_x = bytes_x + 10, latency_x = result_x + (compact ? 8 : 10);
    int detail_x = latency_x + (compact ? 8 : 9);
    int header = y + 1;
    put(header, x + 2, "TIME", 1, time_w, true);
    put(header, port_x, "PORT", 1, port_w, true);
    put(header, ep_x, "EP", 1, 2, true);
    put(header, dir_x, "DIR", 1, 4, true);
    put(header, type_x, "TYPE", 1, 4, true);
    put(header, bytes_x, "     BYTES", 1, 10, true);
    put(header, result_x, "RESULT", 1, compact ? 8 : 10, true);
    put(header, latency_x, "LATENCY", 1, compact ? 8 : 9, true);
    if (detail_x < x + w - 4)
        put(header, detail_x, "REQUEST / PAYLOAD", 1, x + w - detail_x - 2, true);
    int line = y + 2, max_line = y + h - 1;
    size_t visible = size_t(std::max(1, max_line - line));
    size_t begin = 0;
    if (selectable && !rows.empty()) {
        size_t cursor = std::min(ui.cursor, rows.size() - 1);
        if (cursor >= visible)
            begin = cursor + 1 - visible;
    }
    if (rows.empty())
        put(line, x + 3, "Waiting for USB transfers…", 7, w - 6);
    for (size_t i = begin; i < rows.size() && line < max_line; ++i, ++line) {
        const auto &row = rows[i];
        const auto &t = row.transfer;
        bool chosen = selectable && i == ui.cursor;
        auto color_of = [&](int color) { return chosen ? 6 : color; };
        if (chosen)
            put(line, x + 1, std::string(size_t(std::max(0, w - 2)), ' '), 6, w - 2);
        put(line, x + 2, stamp_of(t.timestamp, demo), color_of(7), time_w);
        put(line, port_x, t.port, color_of(9), port_w - 1, chosen);
        put(line, ep_x, std::to_string(t.endpoint), color_of(7), 2);
        put(line, dir_x, t.input ? "IN ↓" : "OUT ↑", color_of(t.input ? 2 : 3), compact ? 4 : 5);
        put(line, type_x, t.kind, color_of(7), 4);
        std::string bytes = human(t.length);
        put(line, bytes_x, std::string(size_t(std::max(0, 10 - int(bytes.size()))), ' ') + bytes, color_of(9), 10);
        std::string result = row.count > 1 ? "×" + std::to_string(row.count)
                                            : t.complete ? status_name(t.status) : "SUBMIT";
        int result_color = row.count > 1 ? 1 : t.complete && t.status < 0 ? 4 : t.complete ? 2 : 7;
        put(line, result_x, result, color_of(result_color), compact ? 8 : 10, chosen);
        put(line, latency_x, t.latency_ms < 0 ? "—" : fixed(t.latency_ms) + "ms", color_of(7), compact ? 8 : 9);
        if (detail_x < x + w - 4) {
            std::string detail = t.description;
            if (ui.hex && !t.payload.empty())
                detail += (detail.empty() ? "" : "  │  ") + t.payload;
            put(line, detail_x, detail, color_of(t.dfu ? 5 : 7), x + w - detail_x - 2);
        }
    }
}
void draw_detail(int y, int x, int h, int w, const LogRow *row, bool demo) {
    if (h < 4 || w < 20)
        return;
    box(y, x, h, w, "transfer");
    if (!row) {
        put(y + 1, x + 2, "No transfer in the current filter.", 7, w - 4);
        return;
    }
    const auto &t = row->transfer;
    int left = x + 2, room = w - 4, row_y = y + 1, limit = y + h - 2;
    auto line = [&](const std::string &text, int color, bool bold = false) {
        if (row_y <= limit)
            put(row_y++, left, text, color, room, bold);
    };
    line(stamp_of(t.timestamp, demo) + "  " + t.port + "  ep" + std::to_string(t.endpoint) + "  " +
             (t.input ? "IN ↓" : "OUT ↑") + "  " + t.kind + (row->count > 1 ? "  ×" + std::to_string(row->count) : ""),
         1, true);
    line(human(t.length) + "  " + (t.complete ? status_name(t.status) : "SUBMIT") + "  latency " +
             (t.latency_ms < 0 ? "—" : fixed(t.latency_ms) + "ms"),
         t.status < 0 ? 4 : 2);
    if (!t.description.empty())
        line(t.description, t.dfu ? 5 : 7);
    if (t.has_setup) {
        unsigned value = t.setup[2] | (unsigned(t.setup[3]) << 8);
        unsigned index = t.setup[4] | (unsigned(t.setup[5]) << 8);
        unsigned length = t.setup[6] | (unsigned(t.setup[7]) << 8);
        std::ostringstream setup;
        setup << "setup bm=0x" << std::hex << std::setw(2) << std::setfill('0') << unsigned(t.setup[0])
              << " req=0x" << std::setw(2) << unsigned(t.setup[1]) << std::dec << " value=" << value
              << " index=" << index << " len=" << length;
        line(setup.str(), 7);
    }
    int per = room >= 68 ? 16 : 8;
    for (size_t offset = 0; offset < t.bytes.size() && row_y <= limit; offset += size_t(per)) {
        std::ostringstream hex;
        std::string ascii;
        hex << std::hex << std::setfill('0');
        for (int i = 0; i < per; ++i) {
            if (offset + size_t(i) >= t.bytes.size())
                break;
            unsigned byte = t.bytes[offset + size_t(i)];
            if (i)
                hex << ' ';
            hex << std::setw(2) << byte;
            ascii.push_back(byte >= 32 && byte < 127 ? char(byte) : '.');
        }
        line(hex.str() + "  " + ascii, 7);
    }
    if (t.bytes.empty() && row_y <= limit)
        line("No captured payload.", 7);
}
void draw_help(int height, int width) {
    const std::vector<std::string> lines = {
        "1 / 2 / 3     Overview, device, or transfer log",
        "Tab           Cycle those layouts",
        "↑/↓ or j/k    Select a port, or a transfer in the log",
        "Enter         Open the device, or the selected transfer",
        "a             All ports / the selected port",
        "i             Hide ports idle for the last 3 seconds",
        "/             Filter by name, port, VID, class, or request",
        "d / e         DFU and errors; the two combine",
        "t / m / f     Cycle type, direction, and endpoint",
        "h             Payload hex on log rows",
        "+ / -         Frame interval, 200 ms to 2 s",
        "Space         Freeze the display; capture keeps running",
        "PgUp/PgDn     Older transfers; Home follows live",
        "Esc           Close the open panel",
        "? / q         Help / quit",
        "",
        "IN and OUT share one scale across the 90 second window.",
        "A device meter is that port's share of its link speed.",
        "Repeated bulk, interrupt, and iso rows collapse.",
        "Control transfers and errors each stay on their own row.",
        "Log times are local. Demo times are elapsed."};
    int box_h = int(lines.size()) + 2, box_w = std::min(78, width - 4);
    int box_y = std::max(1, (height - box_h) / 2), box_x = std::max(0, (width - box_w) / 2);
    for (int row = box_y; row < box_y + box_h && row < height; ++row)
        mvhline(row, box_x, ' ', box_w);
    box(box_y, box_x, box_h, box_w, "busb · controls");
    for (size_t i = 0; i < lines.size(); ++i)
        put(box_y + 1 + int(i), box_x + 2, lines[i], 0, box_w - 4);
}
void render(const Model &model, bool eof, const Options &o, UiState &ui) {
    erase();
    int height, width;
    getmaxyx(stdscr, height, width);
    draw_header(width, o, eof, ui);
    if (width < 80 || height < 24) {
        put(3, 2, "Resize to at least 80 columns × 24 rows. Capture continues.", 5);
        put(5, 2, "q quit   Space freeze display", 1);
        refresh();
        return;
    }
    auto keys = ranked_ports(model, ui);
    if (ui.selected.empty() && !keys.empty())
        ui.selected = keys.front();
    auto rows = coalesce(model, ui);
    if (!rows.empty() && ui.cursor >= rows.size())
        ui.cursor = rows.size() - 1;
    if (rows.empty())
        ui.cursor = 0;
    int connected = 0, active = 0;
    for (const auto &[_, port] : model.ports) {
        if (port.device.connected)
            ++connected;
        if (port.counters.transfer_rate > 0)
            ++active;
    }
    auto port_it = model.ports.find(ui.selected);
    const Port *port = port_it == model.ports.end() ? nullptr : &port_it->second;
    bool one = !ui.all && port;
    const Counters &scope_counters = one ? port->counters : model.total;
    std::string scope = ui.all ? "all ports" : ui.selected.empty() ? "waiting" : ui.selected;
    int usable = height - 3;
    if (ui.layout == Layout::Overview) {
        int log_h = std::clamp(usable / 5, 6, 9);
        int graph_h = std::clamp(usable / 4, 7, 12);
        int ports_h = usable - log_h - graph_h;
        if (ports_h < 6) {
            int need = 6 - ports_h;
            int take = std::min(need, std::max(0, graph_h - 7));
            graph_h -= take;
            need -= take;
            log_h -= std::min(need, std::max(0, log_h - 6));
            ports_h = usable - log_h - graph_h;
        }
        draw_traffic(1, 0, graph_h, width, scope_counters, one ? &port->device : nullptr, scope, model.pending());
        draw_ports(1 + graph_h, 0, ports_h, width, model, keys, ui.selected);
        std::vector<LogRow> interesting;
        for (const auto &row : rows)
            if (row.transfer.status < 0 || row.transfer.dfu || row.transfer.kind == "CTRL" ||
                row.transfer.kind == "BULK")
                interesting.push_back(row);
        if (interesting.empty())
            interesting = rows;
        UiState overview = ui;
        overview.cursor = 0;
        draw_log(1 + graph_h + ports_h, 0, log_h, width, interesting, overview, false, o.demo);
    } else if (ui.layout == Layout::Device) {
        int graph_h = std::clamp(usable / 3, 7, 14);
        int lower = usable - graph_h;
        const Device *shown = port ? &port->device : nullptr;
        const Counters &device_counters = port ? port->counters : model.total;
        std::string device_scope = port ? ui.selected : scope;
        draw_traffic(1, 0, graph_h, width, device_counters, shown, device_scope, model.pending());
        int split = std::clamp(width * 2 / 5, 32, 56);
        draw_ports(1 + graph_h, 0, lower, split, model, keys, ui.selected);
        draw_device(1 + graph_h, split, lower, width - split, port, ui.selected);
    } else {
        int detail_h = 0;
        if (ui.detail)
            detail_h = std::min(usable - 6, std::clamp(usable / 3, 9, 14));
        int log_h = usable - detail_h;
        draw_log(1, 0, log_h, width, rows, ui, true, o.demo);
        if (detail_h) {
            size_t index = rows.empty() ? 0 : std::min(ui.cursor, rows.size() - 1);
            draw_detail(1 + log_h, 0, detail_h, width, rows.empty() ? nullptr : &rows[index], o.demo);
        }
    }
    std::string drops = "dropped " + std::to_string(model.dropped);
    if (model.interval_dropped)
        drops += " (+" + std::to_string(model.interval_dropped) + ")";
    put(height - 2, 2,
        std::to_string(connected) + " connected · " + std::to_string(active) + " active  │  " + drops +
            "  unpaired " + std::to_string(model.unpaired) + "  expired " + std::to_string(model.expired) +
            (model.lifecycle.empty() ? "" : "  │  " + model.lifecycle.back()),
        model.dropped ? 4 : 7, width - 4);
    std::string hint = ui.querying || !ui.query.empty()
                           ? "/" + ui.query + (ui.querying ? "_" : "")
                           : "1/2/3 layout  tab cycle  ↑↓ move  enter open  a scope  / find  d DFU  e errors  "
                             "t type  m dir  f ep  i idle  +/- rate  ? help  q quit";
    put(height - 1, 2, hint, 1, width - 4);
    if (ui.help)
        draw_help(height, width);
    refresh();
}
} // namespace
void run_ui(Source &source, Model &model, Recorder *recorder, const Options &options) {
    Terminal terminal;
    Signals signals;
    Engine engine(source, model, recorder, options);
    UiState ui;
    ui.selected = options.port;
    ui.all = options.port.empty();
    // A remote pty falls behind a full frame; start slower there and let +/- tighten it.
    if (std::getenv("SSH_CONNECTION") || std::getenv("SSH_CLIENT") || std::getenv("SSH_TTY"))
        ui.interval_ms = 1000;
    std::mutex mu;
    std::exception_ptr error;
    std::thread worker([&] {
        try {
            while (!engine.done()) {
                {
                    std::lock_guard lock(mu);
                    engine.tick();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        } catch (...) {
            std::lock_guard lock(mu);
            error = std::current_exception();
            stopped.store(1);
        }
    });
    struct Join {
        std::thread &thread;
        ~Join() {
            stopped.store(1);
            if (thread.joinable())
                thread.join();
        }
    } join{worker};
    auto drawn = Clock::now() - std::chrono::seconds(1);
    while (!stopped.load() && !engine.done()) {
        Model snap;
        bool eof = false;
        {
            std::lock_guard lock(mu);
            if (error)
                break;
            snap = ui.paused && ui.frozen ? *ui.frozen : model;
            eof = engine.eof;
        }
        bool redraw = false;
        for (int key = getch(); key != ERR; key = getch()) {
            redraw = true;
            bool pausing = key == ' ';
            bool was_paused = ui.paused;
            if (handle_key(key, ui, ranked_ports(snap, ui))) {
                stopped.store(1);
                break;
            }
            if (pausing && ui.paused && !was_paused)
                ui.frozen = snap;
            else if (pausing && !ui.paused)
                ui.frozen.reset();
        }
        if (stopped.load())
            break;
        auto now = Clock::now();
        if (redraw || now - drawn >= std::chrono::milliseconds(ui.interval_ms)) {
            if (!ui.paused) {
                std::lock_guard lock(mu);
                if (error)
                    break;
                snap = model;
                eof = engine.eof;
            }
            render(snap, eof, options, ui);
            drawn = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    stopped.store(1);
    join.thread.join();
    {
        std::lock_guard lock(mu);
        if (!error)
            engine.note_drops();
    }
    if (error)
        std::rethrow_exception(error);
}
void run_headless(Source &source, Model &model, Recorder *recorder, const Options &options) {
    Signals signals;
    Engine engine(source, model, recorder, options);
    while (!engine.done() && !source.finished()) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    engine.note_drops();
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
