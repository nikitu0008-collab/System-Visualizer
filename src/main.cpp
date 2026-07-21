#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <algorithm>
#include <iomanip>
#include <ncurses.h>
#include <clocale>
#include <ctime>
#include <deque>
#include <cstdint>

//TODO ASCII-сердце
const std::vector<std::string> HEART_ART = {
    "       .....           .....",
    "   ,ad8PPPP88b,     ,d88PPPP8ba,",
    "  d8P\"      \"Y8b, ,d8P\"      \"Y8b",
    " dP'           \"8a8\"           `Yd",
    " 8(              \"              )8",
    " I8                             8I",
    "  Yb,                         ,dP",
    "   \"8a,                     ,a8\"",
    "     \"8a,                 ,a8\"",
    "       \"Yba             adP\"",
    "         `Y8a         a8P'",
    "           `88,     ,88'",
    "             \"8b   d8\"",
    "              \"8b d8\"",
    "               `888'",
    "                 \""
};

//TODO Структуры данных
struct ProcessInfo {
    std::string user;
    int32_t pid;
    float cpu;
    float mem;
    std::string cmd;
};

struct DiskStats {
    std::string name;
    int64_t read_kb;
    int64_t write_kb;
};

//TODO Сглаживание и история
template<typename T>
class Smoother {
public:
    explicit Smoother(float alpha = 0.3f) noexcept
        : value_(static_cast<T>(0)), alpha_(alpha) {}

    auto update(T raw) noexcept -> T {
        value_ = alpha_ * raw + (static_cast<T>(1.0f) - alpha_) * value_;
        return value_;
    }

    [[nodiscard]] auto get() const noexcept -> T {
        return value_;
    }

private:
    T value_;
    float alpha_;
};

class RingBuffer {
public:
    explicit RingBuffer(size_t max_size = 60) noexcept
        : max_size_(max_size) {}

    auto push(float val) noexcept -> void {
        data_.push_back(val);
        if (data_.size() > max_size_) {
            data_.pop_front();
        }
    }

    [[nodiscard]] auto get() const noexcept -> const std::deque<float>& {
        return data_;
    }

    [[nodiscard]] auto get_last() const noexcept -> float {
        return data_.empty() ? 0.0f : data_.back();
    }

private:
    std::deque<float> data_;
    size_t max_size_;
};

RingBuffer cpu_history(60);
RingBuffer mem_history(60);

//TODO Чтение метрик

auto read_cpu_usage() noexcept -> float {
    static uint64_t prev_idle = 0;
    static uint64_t prev_total = 0;

    std::ifstream stat("/proc/stat");
    if (!stat.is_open()) {
        return 0.0f;
    }

    std::string line;
    if (!std::getline(stat, line)) {
        return 0.0f;
    }

    std::istringstream iss(line);
    std::string cpu;
    uint64_t user = 0, nice = 0, system = 0, idle = 0;
    uint64_t iowait = 0, irq = 0, softirq = 0, steal = 0;
    iss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

    const uint64_t total = user + nice + system + idle + iowait + irq + softirq + steal;
    const uint64_t idle_time = idle + iowait;

    float usage = 0.0f;
    if (prev_total != 0) {
        const uint64_t delta_total = total - prev_total;
        const uint64_t delta_idle = idle_time - prev_idle;
        if (delta_total != 0) {
            const float ratio = static_cast<float>(delta_idle) / static_cast<float>(delta_total);
            usage = 100.0f * (1.0f - ratio);
        }
    }

    prev_idle = idle_time;
    prev_total = total;
    return usage;
}

auto read_cpu_cores() -> std::vector<float> {
    std::vector<float> core_usages;
    static std::vector<uint64_t> prev_idle;
    static std::vector<uint64_t> prev_total;

    std::ifstream stat("/proc/stat");
    if (!stat.is_open()) {
        return core_usages;
    }

    std::string line;
    int32_t idx = 0;
    while (std::getline(stat, line)) {
        if (line.compare(0, 3, "cpu") != 0) {
            continue;
        }
        if (line[3] == ' ') {
            continue; // skip total cpu line
        }

        std::istringstream iss(line);
        std::string cpu;
        uint64_t user = 0, nice = 0, system = 0, idle = 0;
        uint64_t iowait = 0, irq = 0, softirq = 0, steal = 0;
        iss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

        const uint64_t total = user + nice + system + idle + iowait + irq + softirq + steal;
        const uint64_t idle_time = idle + iowait;

        if (static_cast<size_t>(idx) >= prev_idle.size()) {
            prev_idle.push_back(0);
            prev_total.push_back(0);
        }

        float usage = 0.0f;
        if (prev_total[idx] != 0) {
            const uint64_t delta_total = total - prev_total[idx];
            const uint64_t delta_idle = idle_time - prev_idle[idx];
            if (delta_total != 0) {
                const float ratio = static_cast<float>(delta_idle) / static_cast<float>(delta_total);
                usage = 100.0f * (1.0f - ratio);
            }
        }

        prev_idle[idx] = idle_time;
        prev_total[idx] = total;
        core_usages.push_back(usage);
        ++idx;
    }

    return core_usages;
}

auto read_memory_usage() noexcept -> float {
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) {
        return 0.0f;
    }

    uint64_t total = 0;
    uint64_t available = 0;
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.compare(0, 9, "MemTotal:") == 0) {
            std::istringstream iss(line.substr(10));
            iss >> total;
        } else if (line.compare(0, 13, "MemAvailable:") == 0) {
            std::istringstream iss(line.substr(14));
            iss >> available;
        }
    }

    if (total == 0) {
        return 0.0f;
    }
    const uint64_t used = total - available;
    return 100.0f * (static_cast<float>(used) / static_cast<float>(total));
}

auto read_tcp_connections() noexcept -> int32_t {
    std::ifstream tcp("/proc/net/tcp");
    if (!tcp.is_open()) {
        return 0;
    }

    std::string line;
    int32_t count = 0;
    std::getline(tcp, line); // skip header
    while (std::getline(tcp, line)) {
        ++count;
    }
    return count;
}

auto read_top_processes(int32_t n = 5) -> std::vector<ProcessInfo> {
    std::vector<ProcessInfo> procs;
    const std::string cmd = "ps -eo user,pid,pcpu,pmem,comm --sort=-pcpu | head -n " +
                            std::to_string(static_cast<int>(n) + 1) + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return procs;
    }

    char buffer[256];
    // skip header
    if (fgets(buffer, sizeof(buffer), pipe) == nullptr) {
        pclose(pipe);
        return procs;
    }

    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line(buffer);
        std::istringstream iss(line);
        ProcessInfo p;
        iss >> p.user >> p.pid >> p.cpu >> p.mem >> p.cmd;
        if (p.cmd.empty()) {
            continue;
        }
        procs.push_back(p);
        if (static_cast<int32_t>(procs.size()) >= n) {
            break;
        }
    }
    pclose(pipe);
    return procs;
}

auto read_disk_stats() -> std::vector<DiskStats> {
    std::vector<DiskStats> disks;
    std::ifstream diskstats("/proc/diskstats");
    if (!diskstats.is_open()) {
        return disks;
    }

    std::string line;
    while (std::getline(diskstats, line)) {
        std::istringstream iss(line);
        int32_t major = 0, minor = 0;
        std::string name;
        uint64_t rd_ios = 0, rd_merges = 0, rd_sectors = 0, rd_ticks = 0;
        uint64_t wr_ios = 0, wr_merges = 0, wr_sectors = 0, wr_ticks = 0;
        iss >> major >> minor >> name
            >> rd_ios >> rd_merges >> rd_sectors >> rd_ticks
            >> wr_ios >> wr_merges >> wr_sectors >> wr_ticks;

        if (name.find("sd") == 0 || name.find("nvme") == 0) {
            DiskStats d;
            d.name = name;
            d.read_kb = static_cast<int64_t>((rd_sectors * 512U) / 1024U);
            d.write_kb = static_cast<int64_t>((wr_sectors * 512U) / 1024U);
            disks.push_back(d);
        }
    }
    return disks;
}

auto read_gpu_usage() noexcept -> float {
    std::ifstream f("/proc/driver/nvidia/gpu0/info");
    if (!f.is_open()) {
        return -1.0f;
    }

    std::string line;
    while (std::getline(f, line)) {
        if (line.find("Utilization") != std::string::npos) {
            std::istringstream iss(line);
            std::string token;
            iss >> token; // "Utilization:"
            iss >> token; // "X%"
            if (token.find('%') != std::string::npos) {
                const std::string num = token.substr(0, token.find('%'));
                return std::stof(num);
            }
        }
    }
    return -1.0f;
}

auto read_cpu_temp() noexcept -> float {
    std::ifstream f("/sys/class/thermal/thermal_zone0/temp");
    if (!f.is_open()) {
        return -1.0f;
    }
    long temp = 0;
    f >> temp;
    return static_cast<float>(temp) / 1000.0f;
}

auto read_gpu_temp() noexcept -> float {
    std::ifstream f("/proc/driver/nvidia/gpu0/temperature");
    if (!f.is_open()) {
        return -1.0f;
    }
    float temp = 0.0f;
    f >> temp;
    return temp;
}

// --------------------- Отрисовка сердца ---------------------
auto draw_heart(int32_t start_y, int32_t start_x, int32_t color_pair, bool beat) noexcept -> void {
    attron(COLOR_PAIR(color_pair) | (beat ? A_BOLD : A_NORMAL));
    for (const auto& line : HEART_ART) {
        if (line.empty()) {
            continue;
        }
        mvprintw(start_y, start_x, "%s", line.c_str());
        ++start_y;
    }
    attroff(COLOR_PAIR(color_pair) | A_BOLD);
}

//TODO Рамка (универсальная)
auto draw_box(int32_t y, int32_t x, int32_t height, int32_t width, const std::string& title, int32_t color_pair) noexcept -> void {
    if ((height < 2) || (width < 2)) {
        return;
    }
    attron(COLOR_PAIR(color_pair) | A_BOLD);
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + width - 1, ACS_URCORNER);
    for (int32_t i = 1; i < width - 1; ++i) {
        mvaddch(y, x + i, ACS_HLINE);
    }
    mvaddch(y + height - 1, x, ACS_LLCORNER);
    mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);
    for (int32_t i = 1; i < width - 1; ++i) {
        mvaddch(y + height - 1, x + i, ACS_HLINE);
    }
    for (int32_t i = 1; i < height - 1; ++i) {
        mvaddch(y + i, x, ACS_VLINE);
        mvaddch(y + i, x + width - 1, ACS_VLINE);
    }
    if (!title.empty()) {
        attron(COLOR_PAIR(color_pair) | A_REVERSE);
        mvprintw(y, x + 2, " %s ", title.c_str());
        attroff(COLOR_PAIR(color_pair) | A_REVERSE);
    }
    attroff(COLOR_PAIR(color_pair) | A_BOLD);
}

//TODO График CPU (гистограмма)
auto draw_cpu_graph(int32_t y, int32_t x, int32_t width, int32_t height, const std::vector<float>& cores, int32_t frame) noexcept -> void {
    if ((width < 6) || (height < 4)) {
        return;
    }
    draw_box(y, x, height + 1, width, "CPU", 4);
    const int32_t inner_h = height - 1;
    const int32_t inner_w = width - 2;
    const int32_t num_cores = static_cast<int32_t>(cores.size());
    if (num_cores == 0) {
        return;
    }
    int32_t bar_width = (inner_w - 2) / num_cores;
    if (bar_width < 1) {
        bar_width = 1;
    }
    const int32_t max_bars = inner_h - 1;
    if (max_bars < 1) {
        return;
    }
    for (int32_t i = 0; (i < num_cores) && (i < 20); ++i) {
        float usage = cores[i];
        if (usage > 100.0f) {
            usage = 100.0f;
        }
        int32_t bar_height = static_cast<int32_t>((usage / 100.0f) * static_cast<float>(max_bars));
        if (bar_height > max_bars) {
            bar_height = max_bars;
        }
        int32_t color = 2; // green default
        if (usage > 70.0f) {
            color = 1;
        } else if (usage > 40.0f) {
            color = 3;
        }
        attron(COLOR_PAIR(color) | A_BOLD);
        for (int32_t row = 0; row < bar_height; ++row) {
            for (int32_t b = 0; b < bar_width; ++b) {
                const int32_t cy = y + inner_h - row - 1;
                const int32_t cx = x + 1 + i * bar_width + b;
                if ((cy >= y + 1) && (cy < y + inner_h + 1) && (cx < x + width - 1)) {
                    mvaddch(cy, cx, '#');
                }
            }
        }
        attroff(COLOR_PAIR(color) | A_BOLD);
        if ((i % 2 == 0) && (y + inner_h + 1 < LINES)) {
            mvprintw(y + inner_h + 1, x + 1 + i * bar_width, "%d", i);
        }
    }
}

//TODO График памяти (линия)
auto draw_memory_graph(int32_t y, int32_t x, int32_t width, int32_t height, const std::deque<float>& history) noexcept -> void {
    if ((width < 6) || (height < 4)) {
        return;
    }
    draw_box(y, x, height + 1, width, "RAM", 4);
    const int32_t inner_h = height - 1;
    const int32_t inner_w = width - 2;
    if (history.empty()) {
        return;
    }
    const int32_t points = static_cast<int32_t>(std::min(history.size(), static_cast<size_t>(inner_w)));
    const float max_val = 100.0f;
    for (int32_t i = 0; i < points; ++i) {
        float val = history[i];
        if (val > 100.0f) {
            val = 100.0f;
        }
        const int32_t col = inner_w - (points - i);
        if (col < 0) {
            continue;
        }
        int32_t row = static_cast<int32_t>((val / max_val) * static_cast<float>(inner_h));
        if (row > inner_h) {
            row = inner_h;
        }
        int32_t color = 2;
        if (val > 60.0f) {
            color = 1;
        } else if (val > 30.0f) {
            color = 3;
        }
        attron(COLOR_PAIR(color) | A_BOLD);
        mvaddch(y + 1 + (inner_h - row), x + 1 + col, '*');
        for (int32_t r = 1; r < row; ++r) {
            mvaddch(y + 1 + (inner_h - r), x + 1 + col, '.');
        }
        attroff(COLOR_PAIR(color) | A_BOLD);
    }
}

//TODO Процессы (адаптивный)
auto draw_processes(int32_t y, int32_t x, int32_t max_height, const std::vector<ProcessInfo>& procs) noexcept -> void {
    if (max_height < 4) {
        return;
    }
    int32_t width = 40;
    if (max_height < 10) {
        width = 30;
    }
    int32_t height = 2 + static_cast<int32_t>(procs.size()) + 1;
    if (height > max_height) {
        height = max_height;
    }
    draw_box(y, x, height, width, "PROC", 4);
    int32_t row = y + 1;
    attron(A_BOLD);
    mvprintw(row, x + 2, "USER      PID    CPU%");
    ++row;
    attroff(A_BOLD);
    for (const auto& p : procs) {
        int32_t cpu_col = 2;
        if (p.cpu > 60.0f) {
            cpu_col = 1;
        } else if (p.cpu > 30.0f) {
            cpu_col = 3;
        }
        attron(COLOR_PAIR(cpu_col) | A_BOLD);
        mvprintw(row, x + 2, "%-8s %6d  %6.1f", p.user.c_str(), p.pid, static_cast<double>(p.cpu));
        attroff(COLOR_PAIR(cpu_col) | A_BOLD);
        ++row;
        if (row >= y + height - 1) {
            break;
        }
    }
}

//TODO Диски (адаптивный)
auto draw_disks(int32_t y, int32_t x, int32_t max_height, const std::vector<DiskStats>& disks) noexcept -> void {
    if (max_height < 4) {
        return;
    }
    const int32_t width = 35;
    int32_t height = 2 + static_cast<int32_t>(disks.size()) + 1;
    if (height > max_height) {
        height = max_height;
    }
    draw_box(y, x, height, width, "DISK", 4);
    int32_t row = y + 1;
    attron(A_BOLD);
    mvprintw(row, x + 2, "DEVICE   READ  WRITE");
    ++row;
    attroff(A_BOLD);
    for (const auto& d : disks) {
        mvprintw(row, x + 2, "%-6s %5ld %5ld", d.name.c_str(),
                 static_cast<long>(d.read_kb / 1024), static_cast<long>(d.write_kb / 1024));
        ++row;
        if (row >= y + height - 1) {
            break;
        }
    }
}

//TODO systemd (адаптивный)
auto draw_systemd(int32_t y, int32_t x, int32_t max_height, const std::vector<std::string>& units, int32_t frame) noexcept -> void {
    if (max_height < 4) {
        return;
    }
    const int32_t width = 30;
    int32_t height = 2 + static_cast<int32_t>(units.size()) + 1;
    if (height > max_height) {
        height = max_height;
    }
    draw_box(y, x, height, width, "SYSD", 4);
    const int32_t max_display = height - 3;
    const int32_t shown = static_cast<int32_t>(std::min(units.size(), static_cast<size_t>(max_display)));
    for (int32_t i = 0; i < shown; ++i) {
        std::string name = units[i];
        if (name.length() > 15) {
            name = name.substr(0, 15);
        }
        const char* dance = "oO0";
        const char ch = dance[(frame / 5 + i) % 3];
        const int32_t col = (i % 2) ? 5 : 6;
        attron(COLOR_PAIR(col));
        mvprintw(y + i + 1, x + 2, "[%c] %s", ch, name.c_str());
        attroff(COLOR_PAIR(col));
    }
}

// --------------------- Сеть (адаптивный) ---------------------
auto draw_network(int32_t y, int32_t x, int32_t width, int32_t height, int32_t conn, int32_t frame) noexcept -> void {
    if ((height < 4) || (width < 6)) {
        return;
    }
    draw_box(y, x, height + 1, width, "NET", 4);
    const int32_t inner_h = height - 1;
    const int32_t inner_w = width - 2;
    const int32_t max_stars = inner_w * inner_h / 2;
    const int32_t count = (conn > max_stars) ? max_stars : conn;
    for (int32_t i = 0; i < count; ++i) {
        const int32_t cx = x + 1 + (i * 17) % inner_w;
        const int32_t cy = y + 1 + (i * 13) % inner_h;
        const bool bright = ((i + frame) % 5) != 0;
        if (bright) {
            mvaddch(cy, cx, '*');
        } else {
            mvaddch(cy, cx, '.');
        }
    }
    mvprintw(y + height + 1, x + 2, "TCP: %d", conn);
}

//TODO Вспомогательные функции
auto read_load_avg(float& l1, float& l5, float& l15) noexcept -> void {
    std::ifstream f("/proc/loadavg");
    if (f.is_open()) {
        f >> l1 >> l5 >> l15;
    }
}

auto read_uptime_seconds() noexcept -> int64_t {
    std::ifstream f("/proc/uptime");
    double sec = 0.0;
    if (f.is_open()) {
        f >> sec;
    }
    return static_cast<int64_t>(sec);
}

//TODO Главный цикл
auto main() -> int {
    std::setlocale(LC_ALL, "");
    initscr();
    bkgd(COLOR_BLACK);

    start_color();
    init_pair(1, COLOR_RED, COLOR_BLACK);
    init_pair(2, COLOR_GREEN, COLOR_BLACK);
    init_pair(3, COLOR_YELLOW, COLOR_BLACK);
    init_pair(4, COLOR_CYAN, COLOR_BLACK);
    init_pair(5, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(6, COLOR_WHITE, COLOR_BLACK);

    cbreak();
    noecho();
    curs_set(0);
    timeout(80);

    Smoother<float> cpu_smooth(0.2f);
    Smoother<float> mem_smooth(0.2f);

    int32_t frame = 0;
    int32_t phase = 0;

    while (true) {
        clear();

        int32_t max_y = 0;
        int32_t max_x = 0;
        getmaxyx(stdscr, max_y, max_x);

        if ((max_y < 24) || (max_x < 80)) {
            mvprintw(0, 0, "Minimum size: 80x24. Please resize.");
            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        const float cpu_raw = read_cpu_usage();
        const float cpu = cpu_smooth.update(cpu_raw);
        const float mem_raw = read_memory_usage();
        const float mem = mem_smooth.update(mem_raw);
        const int32_t conn = read_tcp_connections();

        const std::vector<ProcessInfo> procs = read_top_processes(5);
        const std::vector<DiskStats> disks = read_disk_stats();
        const std::vector<float> cores = read_cpu_cores();

        cpu_history.push(cpu);
        mem_history.push(mem);

        std::vector<std::string> units;
        FILE* pipe = popen("systemctl list-units --type=service --state=running --no-legend --no-pager 2>/dev/null | awk '{print $1}'", "r");
        if (pipe) {
            char buf[128];
            while (fgets(buf, sizeof(buf), pipe) != nullptr) {
                std::string s(buf);
                if (!s.empty()) {
                    s.pop_back(); // remove newline
                    units.push_back(s);
                }
            }
            pclose(pipe);
        }

        const float gpu_usage = read_gpu_usage();
        const float cpu_temp = read_cpu_temp();
        const float gpu_temp = read_gpu_temp();

        // Пульсация сердца
        int32_t period = static_cast<int32_t>(50.0f - cpu * 0.35f);
        if (period < 12) {
            period = 12;
        }
        phase = (phase + 1) % period;
        const bool beat = (static_cast<float>(phase) > static_cast<float>(period) * 0.9f);

        int32_t heart_color = 2;
        if (cpu < 30.0f) {
            heart_color = 2;
        } else if (cpu < 60.0f) {
            heart_color = 3;
        } else {
            heart_color = 1;
        }

        float load1 = 0.0f, load5 = 0.0f, load15 = 0.0f;
        read_load_avg(load1, load5, load15);
        const int64_t uptime_sec = read_uptime_seconds();
        const int32_t days = static_cast<int32_t>(uptime_sec / 86400);
        const int32_t hours = static_cast<int32_t>((uptime_sec % 86400) / 3600);
        const int32_t minutes = static_cast<int32_t>((uptime_sec % 3600) / 60);

        const time_t now = time(nullptr);
        struct tm* tm_info = localtime(&now);
        char time_buf[20];
        strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);

        // -----------------------------------------------------------
        //  ДИНАМИЧЕСКАЯ КОМПОНОВКА
        // -----------------------------------------------------------
        const int32_t left_width = (max_x < 100) ? 35 : 40;
        const int32_t right_width = max_x - left_width - 2;

        const int32_t heart_height = static_cast<int32_t>(HEART_ART.size());
        const int32_t heart_y = 2;
        const int32_t heart_x = 2;

        const int32_t graph_y = 2;
        const int32_t graph_x = left_width + 2;
        int32_t graph_width = (right_width - 4) / 2;
        if (graph_width < 12) {
            graph_width = 12;
        }
        const int32_t graph_height = (max_y < 28) ? 5 : 8;

        const int32_t mem_graph_x = graph_x + graph_width + 2;
        int32_t mem_graph_width = max_x - mem_graph_x - 4;
        if (mem_graph_width < 10) {
            mem_graph_width = 10;
        }

        const int32_t proc_y = heart_y + heart_height + 2;
        int32_t proc_max_height = max_y - proc_y - 6;
        if (proc_max_height < 4) {
            proc_max_height = 4;
        }

        const int32_t disk_y = graph_y + graph_height + 3;
        const int32_t disk_x = graph_x;
        int32_t disk_max_height = max_y - disk_y - 6;
        if (disk_max_height < 3) {
            disk_max_height = 3;
        }

        int32_t net_y = disk_y;
        int32_t net_x = disk_x + 40;
        if (net_x + 30 >= max_x) {
            net_x = max_x - 34;
        }
        int32_t net_width = max_x - net_x - 4;
        if (net_width < 10) {
            net_width = 10;
        }
        int32_t net_height = max_y - net_y - 4;
        if (net_height < 4) {
            net_height = 4;
        }

        int32_t sysd_y = proc_y + proc_max_height + 2;
        if (sysd_y + 4 > max_y) {
            sysd_y = max_y - 6;
        }
        const int32_t sysd_x = 2;
        if (sysd_y + 4 > max_y) {
            sysd_y = disk_y + disk_max_height + 2;
            // sysd_x remains 2, but that may overlap, so adjust
            // For simplicity, keep it left, but it's better to place under disks
            // We'll just keep it left and reduce height if needed
        }

        //TODO Отрисовка
        draw_heart(heart_y, heart_x, heart_color, beat);
        draw_cpu_graph(graph_y, graph_x, graph_width, graph_height, cores, frame);
        draw_memory_graph(graph_y, mem_graph_x, mem_graph_width, graph_height, mem_history.get());
        draw_processes(proc_y, 2, proc_max_height, procs);
        draw_disks(disk_y, disk_x, disk_max_height, disks);
        draw_network(net_y, net_x, net_width, net_height, conn, frame);
        draw_systemd(sysd_y, sysd_x, max_y - sysd_y - 2, units, frame);

        // Нижняя строка
        std::string gpu_str = (gpu_usage >= 0.0f) ? (std::to_string(static_cast<int32_t>(gpu_usage)) + "%") : "N/A";
        std::string cpu_temp_str = (cpu_temp >= 0.0f) ? (std::to_string(static_cast<int32_t>(cpu_temp)) + "°C") : "N/A";
        std::string gpu_temp_str = (gpu_temp >= 0.0f) ? (std::to_string(static_cast<int32_t>(gpu_temp)) + "°C") : "N/A";
        mvprintw(max_y - 1, 2, "LOAD: %.2f %.2f %.2f  UPTIME: %dd %02dh %02dm  %s  GPU: %s  CPU_TEMP: %s  GPU_TEMP: %s",
                 static_cast<double>(load1), static_cast<double>(load5), static_cast<double>(load15),
                 days, hours, minutes, time_buf,
                 gpu_str.c_str(), cpu_temp_str.c_str(), gpu_temp_str.c_str());

        // Верхняя строка
        int32_t cpu_col = 2;
        if (cpu > 60.0f) {
            cpu_col = 1;
        } else if (cpu > 30.0f) {
            cpu_col = 3;
        }
        attron(COLOR_PAIR(cpu_col) | A_BOLD);
        mvprintw(0, 0, "CPU: %5.1f%%", static_cast<double>(cpu));
        attroff(COLOR_PAIR(cpu_col) | A_BOLD);

        int32_t mem_col = 2;
        if (mem > 60.0f) {
            mem_col = 1;
        } else if (mem > 30.0f) {
            mem_col = 3;
        }
        attron(COLOR_PAIR(mem_col) | A_BOLD);
        mvprintw(0, 20, "RAM: %5.1f%%", static_cast<double>(mem));
        attroff(COLOR_PAIR(mem_col) | A_BOLD);

        mvprintw(0, 40, "TCP: %4d", conn);
        mvprintw(0, 60, "BPM: %3d", static_cast<int32_t>(60.0f + cpu * 1.5f));

        refresh();

        const int ch = getch();
        if ((ch == 'q') || (ch == 'Q')) {
            break;
        }

        ++frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(70));
    }

    endwin();
    return EXIT_SUCCESS;
}
