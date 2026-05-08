/*
 * PowerTUI (shutd) - A High-Precision Universal Shutdown/Reboot Wrapper
 * MIT License
 * Copyright (c) 2026 Adil Haimoura
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * 
 * email: adilhaimoura@gmail.com
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
constexpr const char* VERSION = "v1.2.0";

// ANSI Colors
constexpr const char* GREEN   = "\033[32m";
constexpr const char* YELLOW  = "\033[33m";
constexpr const char* RED     = "\033[31m";
constexpr const char* BLUE    = "\033[34m";
constexpr const char* CYAN    = "\033[36m";
constexpr const char* MAGENTA = "\033[35m";
constexpr const char* RESET   = "\033[0m";
constexpr const char* BLINK   = "\033[5m";
constexpr const char* BOLD    = "\033[1m";

// Background Colors
constexpr const char* BG_WHITE = "\033[47m";
constexpr const char* BG_GRAY  = "\033[100m";
constexpr const char* BG_RED   = "\033[41m";
constexpr const char* BG_BLUE  = "\033[44m";

std::atomic<bool> interrupted(false);
std::chrono::steady_clock::time_point last_switch_toggle;

// ASCII Art - Power Button Only (without face)
const std::vector<std::string> POWER_BUTTON_OFF = {
    "         ╔══════════════╗",
    "         ║    POWER     ║",
    "         ║    BUTTON    ║",
    "         ║              ║",
    "         ║      ○       ║",
    "         ║     /│\\      ║",
    "         ║      │       ║",
    "         ║     / \\      ║",
    "         ╚══════════════╝",
    "                         ",
    "                         ",
};

const std::vector<std::string> POWER_BUTTON_ON = {
    "                         ",
    "                         ",
    "         ╔══════════════╗",
    "         ║    POWER     ║",
    "         ║    BUTTON    ║",
    "         ║   ███████    ║",
    "         ║   ███████    ║",
    "         ╚══════════════╝"
};

const std::vector<std::string> HAND_PRESSING = {
    "             ╭────╮",
    "          ╭──┘    └──╮",
    "          │    👇    │",
    "          ╰──╮    ╭──╯",
    "             ╰────╯"
};

// Function to detect inhibitors (Systemd-aware with Generic Fallback)
std::string get_inhibitor_reason() {
    char buffer[512];
    auto deleter = [](FILE* f) { if (f) pclose(f); };

    // --- STEP 1: D-Bus (Cinnamon/GNOME/KDE aware) ---
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

void handle_signal(int) { 
    interrupted.store(true); 
}

int term_width() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) return w.ws_col;
    return 80;
}

int term_height() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) return w.ws_row;
    return 24;
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

void clear_screen_area(int lines) {
    // Clear from current cursor position down for specified lines
    for (int i = 0; i < lines; ++i) {
        std::cout << "\033[2K";  // Clear current line
        if (i < lines - 1) std::cout << "\033[1B";  // Move down one line
    }
    // Move cursor back up
    std::cout << "\033[" << lines << "A";
}

void draw_ascii_art(int width, bool is_pressing) {
    int start_x = (width - 28) / 2;
    if (start_x < 0) start_x = 0;
    
    std::string indent(start_x, ' ');
    
    // Draw the power button
    std::vector<std::string> current_button = is_pressing ? POWER_BUTTON_ON : POWER_BUTTON_OFF;
    for (const auto& line : current_button) {
        std::cout << indent << (is_pressing ? RED : CYAN) << line << RESET << "\n";
    }
    
    // Draw hand pressing when animating
    if (is_pressing) {
        for (const auto& line : HAND_PRESSING) {
            std::cout << indent << YELLOW << line << RESET << "\n";
        }
    }
}

void draw_buttons(int width, bool focus_shutdown, bool is_reboot) {
    int inner = width - 4;
    
    std::string b1, b2;
    if (is_reboot) {
        b1 = std::string(BOLD) + "[  Reboot now  ]" + RESET;
        b2 = std::string(BOLD) + "[  Cancel  ]" + RESET;
    } else {
        b1 = std::string(BOLD) + "[  Shutdown now  ]" + RESET;
        b2 = std::string(BOLD) + "[  Cancel  ]" + RESET;
    }
    
    int b1_len = 18;  // Length without ANSI codes
    int b2_len = 12;
    int total_len = b1_len + 3 + b2_len;
    int left_pad = (inner - total_len) / 2;
    if (left_pad < 0) left_pad = 0;
    
    std::cout << "│ ";
    for (int i = 0; i < left_pad; ++i) std::cout << " ";
    
    if (focus_shutdown) std::cout << BG_WHITE << RED << b1 << RESET; 
    else std::cout << b1;
    
    std::cout << "   ";
    
    if (!focus_shutdown) std::cout << BG_WHITE << RED << b2 << RESET; 
    else std::cout << b2;
    
    int used = left_pad + total_len;
    for (int i = 0; i < inner - used; ++i) std::cout << " ";
    std::cout << " │\n";
}

void draw_ui(int width, int height, const std::string& timer_line, int total, int remaining, bool focus_shutdown, bool is_reboot, bool pressing) {
    // Clear from cursor position to bottom instead of full screen
    std::cout << "\033[J";  // Clear from cursor to end of screen
    std::cout << "\033[H";  // Move cursor to home
    
    // Calculate total height of content
    int art_height = POWER_BUTTON_OFF.size() + (pressing ? HAND_PRESSING.size() : 0);
    int total_height = art_height + 8;
    
    // Add top padding to center vertically
    int top_padding = (height - total_height) / 2;
    if (top_padding < 0) top_padding = 1;
    
    for (int i = 0; i < top_padding; ++i) std::cout << "\n";
    
    // Draw ASCII art
    draw_ascii_art(width, pressing);
    
    std::cout << "\n\n";
    
    int inner = width - 4;
    if (inner < 30) inner = 30;
    
    // Draw timer box - top border
    std::cout << "┌";
    for (int i = 0; i < width - 2; ++i) std::cout << "─";
    std::cout << "┐\n";
    
    // Timer line with proper padding
    std::string clean_timer = strip_ansi(timer_line);
    int timer_len = clean_timer.length();
    int timer_pad = (width - 2 - timer_len) / 2;
    if (timer_pad < 0) timer_pad = 0;
    
    std::cout << "│";
    for (int i = 0; i < timer_pad; ++i) std::cout << " ";
    std::cout << timer_line;
    for (int i = 0; i < width - 2 - timer_pad - timer_len; ++i) std::cout << " ";
    std::cout << "│\n";
    
    // Progress bar
    int filled = (total > 0) ? ((total - remaining) * inner) / total : 0;
    bool alert = (remaining <= 10);
    std::cout << "│ ";
    for (int i = 0; i < inner; ++i) {
        if (i < filled) {
            if (alert) std::cout << BG_RED << " " << RESET;
            else std::cout << BG_BLUE << " " << RESET;
        } else {
            std::cout << BG_GRAY << " " << RESET;
        }
    }
    std::cout << " │\n";
    
    // Buttons
    draw_buttons(width, focus_shutdown, is_reboot);
    
    // Bottom border
    std::cout << "└";
    for (int i = 0; i < width - 2; ++i) std::cout << "─";
    std::cout << "┘\n";
    
    // Instructions
    std::cout << "\n" << YELLOW << "  Controls: " << RESET;
    std::cout << "← → / Tab / Space  |  " << GREEN << "Enter" << RESET << "  |  ";
    std::cout << RED << "C" << RESET << " / Ctrl+C\n";
}

int countdown(int total_seconds, bool reboot) {
    int width = term_width(); 
    int height = term_height();
    if (width < 50) width = 50;
    
    bool focus_shutdown = true;
    bool pressing_animation = false;
    last_switch_toggle = std::chrono::steady_clock::now();
    
    auto start_time = std::chrono::steady_clock::now();
    auto target_time = start_time + std::chrono::seconds(total_seconds);
    
    // Save cursor position and clear screen
    std::cout << "\033[s";  // Save cursor position
    set_raw_mode(true);
    
    // Initial clear and draw
    std::cout << "\033[H\033[J";  // Clear entire screen
    std::cout.flush();
    
    int frame_count = 0;
    
    while (!interrupted.load()) {
        auto now = std::chrono::steady_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::seconds>(target_time - now).count();
        int remaining = static_cast<int>(diff);
        if (remaining < 0) break;
        
        // Handle switch animation timeout
        if (now - last_switch_toggle > std::chrono::milliseconds(300)) {
            pressing_animation = false;
        }
        
        int min = remaining / 60;
        int sec = remaining % 60;
        bool alert = (remaining <= 10);
        
        std::ostringstream line;
        line << (alert ? RED : GREEN) << BOLD;
        if (alert) line << "⚠️  ";
        line << (reboot ? "REBOOT IN: " : "SHUTDOWN IN: ");
        line << std::setw(2) << std::setfill('0') << min << ":"
             << std::setw(2) << std::setfill('0') << sec;
        if (alert) line << "  ⚠️";
        line << RESET;
        
        // Only redraw if something changed (reduce flicker)
        if (frame_count == 0 || pressing_animation || remaining % 2 == 0) {
            draw_ui(width, height, line.str(), total_seconds, remaining, 
                    focus_shutdown, reboot, pressing_animation);
        }
        
        std::fflush(stdout);
        
        fd_set readfds; 
        FD_ZERO(&readfds); 
        FD_SET(STDIN_FILENO, &readfds);
        struct timeval tv = {0, 50000};
        
        if (select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &tv) > 0) {
            char ch;
            if (read(STDIN_FILENO, &ch, 1) == 1) {
                if (ch == '\033') {  // Arrow keys
                    char n1, n2;
                    if (read(STDIN_FILENO, &n1, 1) == 1 && n1 == '[') {
                        if (read(STDIN_FILENO, &n2, 1) == 1 && (n2 == 'C' || n2 == 'D')) {
                            focus_shutdown = !focus_shutdown;
                            pressing_animation = true;
                            last_switch_toggle = std::chrono::steady_clock::now();
                        }
                    }
                } 
                else if (ch == '\t' || ch == ' ') {
                    focus_shutdown = !focus_shutdown;
                    pressing_animation = true;
                    last_switch_toggle = std::chrono::steady_clock::now();
                    // Force immediate redraw
                    draw_ui(width, height, line.str(), total_seconds, remaining, 
                            focus_shutdown, reboot, pressing_animation);
                    std::fflush(stdout);
                }
                else if (ch == '\n' || ch == '\r') { 
                    set_raw_mode(false);
                    std::cout << "\033[u\033[J";  // Restore cursor and clear
                    std::cout.flush();
                    return focus_shutdown ? 1 : 0;
                }
                else if (ch == 'c' || ch == 'C' || ch == 3) { 
                    interrupted.store(true); 
                    break;
                }
            }
        }
        
        frame_count++;
        if (frame_count > 100) frame_count = 0;
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    set_raw_mode(false);
    std::cout << "\033[u\033[J";  // Restore cursor position and clear
    std::cout.flush();
    return (!interrupted.load()) ? 1 : 0;
}

void print_usage(const char* prog_name) {
    std::cout << "\n" << BOLD << CYAN << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║  PowerTUI (shutd) " << VERSION << " - High-precision Power Wrapper  ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n" << RESET;
    std::cout << GREEN << "Copyright (c) 2026 Adil Haimoura <adilhaimoura@gmail.com>\n\n" << RESET;
    std::cout << YELLOW << "Usage: " << RESET << prog_name << " [options] [minutes]\n\n";
    std::cout << BOLD << "Options:\n" << RESET;
    std::cout << "  " << GREEN << "-h, -s, --shutdown" << RESET << "   Schedule a shutdown (default)\n";
    std::cout << "  " << GREEN << "-r, --reboot" << RESET << "         Schedule a reboot\n";
    std::cout << "  " << GREEN << "--help, -?" << RESET << "           Display this help message\n\n";
    std::cout << BOLD << "Examples:\n" << RESET;
    std::cout << "  " << CYAN << prog_name << " 5" << RESET << "          # Shutdown in 5 minutes\n";
    std::cout << "  " << CYAN << prog_name << " -r 10" << RESET << "      # Reboot in 10 minutes\n";
    std::cout << "  " << CYAN << prog_name << " -s 1" << RESET << "       # Shutdown in 1 minute\n\n";
    std::cout << BOLD << "Controls:\n" << RESET;
    std::cout << "  ← → / Tab / Space  - Switch between options\n";
    std::cout << "  Enter              - Confirm selection\n";
    std::cout << "  C / Ctrl+C         - Cancel operation\n\n";
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
        std::cout << RED << BOLD << "┌────────────────────────────────────────────────────────┐\n";
        std::cout << "│           CRITICAL ERROR: Power Event Blocked!         │\n";
        std::cout << "└────────────────────────────────────────────────────────┘\n" << RESET;
        std::cout << YELLOW << "Blocked by: " << RESET << blocker << "\n";
        std::cout << RED << "Terminating to prevent data loss or failed execution.\n" << RESET;
        return 1; 
    }

    int total_seconds = minutes * 60;
    
    // Show confirmation message
    std::cout << GREEN << BOLD << "✓ " << RESET << BOLD << "PowerTUI " << VERSION << RESET;
    if (reboot) {
        std::cout << " | " << MAGENTA << "REBOOT" << RESET;
    } else {
        std::cout << " | " << RED << "SHUTDOWN" << RESET;
    }
    std::cout << " scheduled in " << YELLOW << minutes << RESET << " minute" << (minutes > 1 ? "s" : "") << "\n";
    std::cout << CYAN << "Starting TUI interface...\n" << RESET;
    std::cout.flush();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (countdown(total_seconds, reboot)) {
        std::cout << "\n" << RED << BOLD << "╔════════════════════════════════════════╗\n";
        std::cout << "║     EXECUTING " << (reboot ? "REBOOT" : "SHUTDOWN") << " NOW...     ║\n";
        std::cout << "╚════════════════════════════════════════╝\n" << RESET;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::system(reboot ? "reboot" : "shutdown now");
    } else {
        std::cout << "\n" << YELLOW << BOLD << "┌────────────────────────────────────────┐\n";
        std::cout << "│          " << (reboot ? "REBOOT" : "SHUTDOWN") << " ABORTED!             │\n";
        std::cout << "└────────────────────────────────────────┘\n" << RESET;
    }
    return 0;
}
