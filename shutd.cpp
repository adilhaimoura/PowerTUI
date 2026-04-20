/*
 * PowerTUI (shutd) - A High-Precision Universal Shutdown/Reboot Wrapper
 * MIT License
 * Copyright (c) 2026 Adil Haimoura
 * * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * * email: adilhaimoura@gmail.com
 */

#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <cstdlib>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cctype>
#include <termios.h>
#include <sys/select.h>
#include <sstream>
#include <vector>
#include <cstdio>
#include <memory>

// Version Constant
constexpr const char* VERSION = "v1.1.0";

// ANSI Colors
constexpr const char* GREEN  = "\033[32m";
constexpr const char* YELLOW = "\033[33m";
constexpr const char* RED    = "\033[31m";
constexpr const char* RESET  = "\033[0m";
constexpr const char* BLINK  = "\033[5m";

// Background Colors
constexpr const char* BG_WHITE = "\033[47m";
constexpr const char* BG_GRAY  = "\033[100m";
constexpr const char* BG_RED   = "\033[41m";

std::atomic<bool> interrupted(false);

// Function to detect inhibitors (Systemd-aware with Generic Fallback)
std::string get_inhibitor_reason() {
    char buffer[512];
    auto deleter = [](FILE* f) { if (f) pclose(f); };

    // --- STEP 1: D-Bus (Cinnamon/GNOME/KDE aware) ---
    // Best for catching "Browsers are downloading or streaming"
    if (std::system("command -v dbus-send >/dev/null 2>&1") == 0) {
        std::string dbus_cmd = "dbus-send --print-reply --dest=org.gnome.SessionManager "
                               "/org/gnome/SessionManager org.gnome.SessionManager.GetInhibitors 2>/dev/null | "
                               "grep 'object path' | head -n 1 | cut -d'\"' -f2";
        
        std::string path;
        {
            std::unique_ptr<FILE, decltype(deleter)> pipe(popen(dbus_cmd.c_str(), "r"), deleter);
            if (pipe && fgets(buffer, sizeof(buffer), pipe.get())) {
                path = buffer;
                if (!path.empty() && path.back() == '\n') path.pop_back();
            }
        }

        if (!path.empty()) {
            std::string detail_cmd = "dbus-send --print-reply --dest=org.gnome.SessionManager " + path + 
                                     " org.gnome.SessionManager.Inhibitor.GetAppId 2>/dev/null | grep 'string' | cut -d'\"' -f2";
            std::unique_ptr<FILE, decltype(deleter)> detail_pipe(popen(detail_cmd.c_str(), "r"), deleter);
            if (detail_pipe && fgets(buffer, sizeof(buffer), detail_pipe.get())) {
                std::string app = buffer;
                if (!app.empty()) {
                    if (app.back() == '\n') app.pop_back();
                    return app + " (via Session Manager)";
                }
            }
        }
    }

    // --- STEP 2: Systemd Fallback ---
    if (std::system("command -v systemd-inhibit >/dev/null 2>&1") == 0) {
        std::string sysd_cmd = "systemd-inhibit --list --no-pager 2>/dev/null | grep 'shutdown' | tail -n 1 | awk '{print $5}'";
        std::unique_ptr<FILE, decltype(deleter)> pipe(popen(sysd_cmd.c_str(), "r"), deleter);
        if (pipe && fgets(buffer, sizeof(buffer), pipe.get())) {
            std::string res = buffer;
            if (res.length() > 1) {
                if (res.back() == '\n') res.pop_back();
                return res + " (systemd-lock)";
            }
        }
    }

    // --- STEP 3: Non-Systemd / Generic Fallback ---
    // If no system-wide inhibitor is found, check for high-priority media/work apps
    // This works on Void, Gentoo, Artix, etc.
    std::string fallback = "pgrep -l 'ffmpeg|vlc|obs|steam|render|iso-writer' | head -n 1";
    std::unique_ptr<FILE, decltype(deleter)> pipe(popen(fallback.c_str(), "r"), deleter);
    if (pipe && fgets(buffer, sizeof(buffer), pipe.get())) {
        std::string res = buffer;
        if (!res.empty()) {
            if (res.back() == '\n') res.pop_back();
            return "Process active: " + res;
        }
    }

    return "";
}
void set_raw_mode(bool enable) {
    static struct termios oldt;
    static bool saved = false;
    struct termios newt;
    if (enable) {
        if (!saved) { tcgetattr(STDIN_FILENO, &oldt); saved = true; }
        newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO | ISIG); 
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
        std::cout << "\033[?25l" << std::flush;
    } else if (saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        std::cout << "\033[?25h" << std::flush;
    }
}

void handle_signal(int) { interrupted.store(true); }

int term_width() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) return w.ws_col;
    return 80;
}

std::string strip_ansi(const std::string& s) {
    std::string result;
    bool in_escape = false;
    for (char c : s) {
        if (!in_escape && c == '\033') { in_escape = true; continue; }
        if (!in_escape) result += c;
        else if (c == 'm') in_escape = false;
    }
    return result;
}

bool is_positive(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit((unsigned char)c)) return false;
    return std::stoll(s) > 0;
}

void draw_buttons(int width, bool focus_shutdown, bool is_reboot) {
    int inner = width - 4;
    if (inner < 20) inner = 20;
    std::string b1 = is_reboot ? "[  Reboot now  ]" : "[ Shutdown now ]";
    std::string b2 = "[    Cancel    ]";
    int total_len = (int)b1.size() + 3 + (int)b2.size();
    int left_pad = (inner - total_len) / 2;
    if (left_pad < 0) left_pad = 0;
    std::cout << "│ ";
    for (int i = 0; i < left_pad; ++i) std::cout << " ";
    if (focus_shutdown) std::cout << BG_WHITE << RED << b1 << RESET; else std::cout << b1;
    std::cout << "   ";
    if (!focus_shutdown) std::cout << BG_WHITE << RED << b2 << RESET; else std::cout << b2;
    int used = left_pad + total_len;
    for (int i = 0; i < inner - used; ++i) std::cout << " ";
    std::cout << " │\n";
}

void draw_ui(int width, const std::string& timer_line, int total, int remaining, bool focus_shutdown, bool is_reboot) {
    int inner = width - 4;
    if (inner < 20) inner = 20;
    int filled = (total > 0) ? ((total - remaining) * inner) / total : 0;
    bool alert = (remaining <= 10);
    std::string clean = strip_ansi(timer_line);
    int pad = inner - static_cast<int>(clean.size());
    if (pad < 0) pad = 0;
    std::cout << "┌";
    for (int i = 0; i < width - 2; ++i) std::cout << "─";
    std::cout << "┐\n";
    std::cout << "│ " << timer_line << std::string(pad, ' ') << " │\n";
    std::cout << "│ ";
    for (int i = 0; i < inner; ++i) {
        if (i < filled) std::cout << (alert ? BG_RED : BG_WHITE) << " ";
        else std::cout << BG_GRAY << " ";
    }
    std::cout << RESET << " │\n";
    draw_buttons(width, focus_shutdown, is_reboot);
    std::cout << "└";
    for (int i = 0; i < width - 2; ++i) std::cout << "─";
    std::cout << "┘\n";
}

int countdown(int total_seconds, bool reboot) {
    int width = term_width(); if (width < 45) width = 45;
    bool focus_shutdown = true;
    auto start_time = std::chrono::steady_clock::now();
    auto target_time = start_time + std::chrono::seconds(total_seconds);
    std::cout << "\n\n\n\n";
    set_raw_mode(true);
    while (!interrupted.load()) {
        auto now = std::chrono::steady_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::seconds>(target_time - now).count();
        int remaining = static_cast<int>(diff);
        if (remaining < 0) break;
        std::cout << "\033[5F"; 
        int min = remaining / 60;
        int sec = remaining % 60;
        bool alert = (remaining <= 10);
        std::ostringstream line;
        line << (alert ? RED : GREEN) << (reboot ? "Reboot in: " : "Shutdown in: ") << RESET
             << (alert ? BLINK : "") << (alert ? RED : YELLOW)
             << std::setw(2) << std::setfill('0') << min << ":"
             << std::setw(2) << std::setfill('0') << sec << RESET;
        draw_ui(width, line.str(), total_seconds, remaining, focus_shutdown, reboot);
        std::fflush(stdout);
        fd_set readfds; FD_ZERO(&readfds); FD_SET(STDIN_FILENO, &readfds);
        struct timeval tv = {0, 50000}; 
        if (select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &tv) > 0) {
            char ch;
            if (read(STDIN_FILENO, &ch, 1) == 1) {
                if (ch == '\033') { 
                    char n1, n2;
                    if (read(STDIN_FILENO, &n1, 1) == 1 && n1 == '[') {
                        if (read(STDIN_FILENO, &n2, 1) == 1 && (n2 == 'C' || n2 == 'D')) { 
                            focus_shutdown = !focus_shutdown; 
                        }
                    }
                } 
                else if (ch == '\t' || ch == ' ' ) { focus_shutdown = !focus_shutdown; }
                else if (ch == '\n' || ch == '\r') { set_raw_mode(false); return focus_shutdown ? 1 : 0; }
                else if (ch == 'c' || ch == 'C' || ch == 3) { interrupted.store(true); break; }
            }
        }
    }
    set_raw_mode(false);
    return (!interrupted.load()) ? 1 : 0;
}

void print_usage(const char* prog_name) {
    std::cout << GREEN << "PowerTUI (shutd) " << VERSION << RESET << " - High-precision shutdown/reboot wrapper\n"
              << "Copyright (c) 2026 Adil Haimoura <adilhaimoura@gmail.com>\n\n"
              << "Usage: " << prog_name << " [options] [minutes]\n\n"
              << "Options:\n"
              << "  -h, -s, --shutdown   Schedule a shutdown (default)\n"
              << "  -r, --reboot         Schedule a reboot\n"
              << "  --help, -?           Display this help message\n\n"
              << "Examples:\n"
              << "  " << prog_name << " 5          # Shutdown in 5 minutes\n"
              << "  " << prog_name << " -r 10      # Reboot in 10 minutes\n"
              << "  " << prog_name << " -s 1       # Shutdown in 1 minute\n\n";
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTSTP, handle_signal);

    bool reboot = false;
    int minutes = 1;

    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "--help" || arg1 == "-?" || arg1 == "--h" || arg1 == "-help") {
            print_usage(argv[0]);
            return 0;
        }
        if (argc == 2 && is_positive(arg1)) {
            minutes = std::stoi(arg1);
        } else if (argc == 3) {
            if (arg1 == "-r" || arg1 == "--reboot") reboot = true;
            else if (arg1 == "-s" || arg1 == "-h" || arg1 == "--shutdown") reboot = false;
            else {
                std::cerr << RED << "[" << VERSION << "] Error: Unknown flag '" << arg1 << "'" << RESET << "\n";
                print_usage(argv[0]);
                return 1;
            }
            if (is_positive(argv[2])) minutes = std::stoi(argv[2]);
            else {
                std::cerr << RED << "[" << VERSION << "] Error: '" << argv[2] << "' is not a valid number." << RESET << "\n";
                return 1;
            }
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    std::string blocker = get_inhibitor_reason();
    if (!blocker.empty()) {
        std::cout << RED << "[" << VERSION << " Critical Error]:" << RESET << " Power event blocked by: " << YELLOW << blocker << RESET << "\n";
        std::cout << "Terminating to prevent data loss or failed execution.\n";
        return 1; 
    }

    int total_seconds = minutes * 60;
    std::cout << GREEN << "PowerTUI " << VERSION << RESET << " | " << (reboot ? "Reboot" : "Shutdown") << " scheduled for " << minutes << " min\n";
    std::cout << "Arrows/Tab/Space: Switch | Enter: Confirm | 'c': Cancel\n";

    if (countdown(total_seconds, reboot)) {
        std::cout << "\nExecuting " << (reboot ? "Reboot" : "Shutdown") << " now...\n";
        std::system(reboot ? "reboot" : "shutdown now");
    } else {
        std::cout << (reboot ? "\n\033[31m Reboot Aborted.\033[0m\n" : "\n\033[31m Shutdown Aborted.\033[0m\n");
    }
    return 0;
}
