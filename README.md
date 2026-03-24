⚡ PowerTUI
==========

**A lightweight, high-performance C++ wrapper for Linux shutdown and reboot commands.**

Featuring a real-time Terminal UI, interactive safety buttons, and inhibitor awareness.

![License](https://img.shields.io/badge/license-MIT-blue.svg) ![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg) ![Language](https://img.shields.io/badge/C++-11-blue.svg)

* * *

📋 Table of Contents
--------------------

*   [✨ Features](#features)
*   [🚀 Installation](#installation)
*   [💻 Usage](#usage)
*   [⌨️ Controls](#controls)
*   [🧠 Why PowerTUI?](#why)
*   [📄 License](#license)

✨ Features
----------

*   **Interactive TUI:** A clean, ANSI-powered progress bar and button interface directly in your terminal.
*   **Safety First:** Prevents accidental shutdowns with a "Cancel" focus by default and an interactive confirmation loop.
*   **Inhibitor Aware:** Automatically checks if applications (like **SMPlayer**, **Chrome**, or **Firefox**) are blocking shutdown via `systemd-inhibit` and warns you before proceeding.
*   **Zero Dependencies:** Pure C++11. No `ncurses`, no Python, no heavy libraries. Just compile and run.
*   **Keyboard Optimized:** Full support for `Tab`, `Space`, `Enter`, and `Arrow Keys` without aborting the sequence.

🚀 Installation
---------------

### 1\. Clone & Compile

Clone the repository and compile the source code using `g++`:

    git clone https://github.com/YOUR_USERNAME/PowerTUI.git
    cd PowerTUI
    g++ shutd.cpp -o shutd

### 2\. Install Globally

Move the binary to your local bin folder to run it from anywhere:

    sudo mv shutd /usr/local/bin/
    sudo chmod +x /usr/local/bin/shutd

💻 Usage
--------

Run a standard 1-minute shutdown countdown:

    shutd

### Options

Command

Description

`shutd`

Schedules a shutdown in **1 minute** (default).

`shutd -r <min>`

Schedules a **reboot** in `<min>` minutes.

`shutd -h <min>`

Schedules a **shutdown** in `<min>` minutes.

**Examples:**

    # Reboot in 10 minutes
    shutd -r 10
    
    # Shutdown in 5 minutes
    shutd -h 5

⌨️ Controls
-----------

The interface is designed for quick keyboard interaction.

Key

Action

`Tab` / `Arrows`

Toggle between **Action** and **Cancel**.

`Enter`

Confirm selection.

`C` or `Ctrl+C`

Immediate Abort.

🧠 Why PowerTUI?
----------------

Standard `shutdown +m` commands are "set and forget," often leading to lost work if you forget a timer is running in the background.

**PowerTUI** solves this by staying in your terminal, providing constant visual feedback. If a background process (like a large download, a render, or a media player) is inhibiting shutdown, PowerTUI will detect it and alert you before the final execution, ensuring you never lose unsaved work accidentally.

📄 License
----------

Distributed under the MIT License. See `LICENSE` for more information.
