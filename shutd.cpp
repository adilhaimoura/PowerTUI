/*
 * shutd.cpp - Shutdown/Reboot wrapper with TUI, Arrow Keys, and Inhibitor Check
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

// Function to check if other apps are blocking shutdown
bool is_system_inhibited() {
    // We check for inhibitors that specifically block "shutdown"
    // redirecting stderr to dev/null to keep the TUI clean
    int status = std::system("systemd-inhibit --list --mode=block 2>/dev/null | grep -q 'shutdown'");
    return (status == 0);
}

// Raw mode handling
void set_raw_mode(bool enable) {
    static struct termios oldt;
    static bool saved = false;
    struct termios newt;

    if (enable) {
        if (!saved) {
            if (tcgetattr(STDIN_FILENO, &oldt) == -1) return;
            saved = true;
        }
        newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);
        newt.c_lflag &= ~(ISIG); 
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
        std::cout << "\033[?25l" << std::flush; // Hide cursor
    } else if (saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        std::cout << "\033[?25h" << std::flush; // Show cursor
    }
}

void handle_signal(int) {
    interrupted.store(true);
}

int term_width() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
    return 80;
}

std::string strip_ansi(const std::string& s) {
    std::string result;
    bool in_escape = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (!in_escape && s[i] == '\033') { in_escape = true; continue; }
        if (!in_escape) { result += s[i]; } 
        else if (s[i] == 'm') { in_escape = false; }
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

    std::string btn_action = is_reboot ? "[  Reboot now  ]" : "[ Shutdown now ]";
    std::string btn_cancel = "[    Cancel    ]";

    int total_len = btn_action.size() + 3 + btn_cancel.size();
    int left_pad = (inner - total_len) / 2;
    if (left_pad < 0) left_pad = 0;

    std::cout << "│ ";
    for (int i = 0; i < left_pad; ++i) std::cout << " ";

    if (focus_shutdown) std::cout << BG_WHITE << RED << btn_action << RESET;
    else std::cout << btn_action;

    std::cout << "   ";

    if (!focus_shutdown) std::cout << BG_WHITE << RED << btn_cancel << RESET;
    else std::cout << btn_cancel;

    int used = left_pad + total_len;
    int remaining = inner - used;
    for (int i = 0; i < remaining; ++i) std::cout << " ";
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

    std::cout << "│ " << timer_line;
    for (int i = 0; i < pad; ++i) std::cout << " ";
    std::cout << " │\n";

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
    int width = term_width();
    if (width < 40) width = 40;
    int initial = total_seconds;
    bool focus_shutdown = true;

    std::cout << "\n\n\n\n";
    set_raw_mode(true);

    while (total_seconds >= 0 && !interrupted.load()) {
        std::cout << "\033[5F"; // Move cursor up 5 lines

        int min = total_seconds / 60;
        int sec = total_seconds % 60;
        bool alert = (total_seconds <= 10);

        std::ostringstream line;
        line << (alert ? RED : GREEN) << (reboot ? "Reboot in: " : "Shutdown in: ") << RESET
             << (alert ? BLINK : "") << (alert ? RED : YELLOW)
             << std::setw(2) << std::setfill('0') << min << ":"
             << std::setw(2) << std::setfill('0') << sec << RESET;

        draw_ui(width, line.str(), initial, total_seconds, focus_shutdown, reboot);
        std::fflush(stdout);

        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < end) {
            fd_set readfds; FD_ZERO(&readfds); FD_SET(STDIN_FILENO, &readfds);
            struct timeval tv = {0, 100000}; // 100ms poll

            if (select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &tv) > 0) {
                char ch;
                if (read(STDIN_FILENO, &ch, 1) == 1) {
                    // Arrow Key Handling
                    if (ch == '\033') {
                        char n1, n2;
                        if (read(STDIN_FILENO, &n1, 1) == 1 && n1 == '[') {
                            if (read(STDIN_FILENO, &n2, 1) == 1) {
                                if (n2 == 'C' || n2 == 'D') { focus_shutdown = !focus_shutdown; break; }
                            }
                        }
                    } 
                    else if (ch == '\t' || ch == ' ' ) { focus_shutdown = !focus_shutdown; break; }
                    else if (ch == '\n' || ch == '\r') { set_raw_mode(false); return focus_shutdown ? 1 : 0; }
                    else if (ch == 'c' || ch == 'C' || ch == 3) { interrupted.store(true); set_raw_mode(false); return 0; }
                }
            }
        }
        --total_seconds;
    }
    set_raw_mode(false);
    return (!interrupted.load()) ? 1 : 0;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTSTP, handle_signal);

    bool reboot = false;
    int minutes = 1;

    if (argc == 3) {
        std::string mode = argv[1];
        if (is_positive(argv[2])) {
            reboot = (mode == "-r");
            minutes = std::stoi(argv[2]);
        }
    }

    int total_seconds = minutes * 60;

    std::cout << (reboot ? "Reboot" : "Shutdown") << " scheduled for " << minutes << " min\n";
    std::cout << "Arrows/Tab: Switch | Enter: Confirm | 'c': Cancel\n";

    int action = countdown(total_seconds, reboot);

    if (action && !interrupted.load()) {
        std::cout << "\nChecking for active inhibitors...\n";
        if (is_system_inhibited()) {
            std::cout << YELLOW << "![Warning]: SMPlayer, Chrome, or another app is flagging an inhibitor." << RESET << "\n";
            std::cout << "Override: Executing anyway in 2 seconds...\n";
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }

        std::cout << "Executing " << (reboot ? "Reboot" : "Shutdown") << " now...\n";
        std::system(reboot ? "reboot" : "shutdown now");
    } else {
        std::cout << (reboot ? "\n\033[31m Reboot Aborted.\033[0m\n" : "\n\033[31m Shutdown Aborted.\033[0m\n");
    }

    return 0;
}