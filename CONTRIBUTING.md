# Contributing to OpenDM

Thank you for your interest in contributing to OpenDM! OpenDM is an open-source, high-speed download accelerator for Linux created by **Nader Mahbub Khan**. We welcome bug reports, feature suggestions, optimizations, and pull requests from everyone.

---

## Code of Conduct & Attribution Policy

OpenDM is free and open-source software licensed under the **GNU General Public License v3.0 (GPL-3.0)** with Section 7 attribution terms.

### 1. Citation of the Real Developer
* OpenDM was conceived, designed, and developed by **Nader Mahbub Khan** ([@nadermkhan](https://github.com/nadermkhan)).
* When creating forks, tutorials, packages, or derivative projects, you **must prominently cite the original developer and repository**:
  > Based on OpenDM by Nader Mahbub Khan (https://github.com/nadermkhan/opendm)
* Preserving author copyright notices in all source files, READMEs, and documentation is mandatory.

### 2. Zero Tolerance for Code Theft & Plagiarism
* **No Stripping of Copyright/Attribution**: Do not delete or obscure copyright notices, author information in `AUTHORS`, `NOTICE`, or source file headers.
* **No Closed-Source Appropriation**: OpenDM is strictly copyleft (GPL-3.0). You may not incorporate OpenDM code into proprietary, closed-source, or paid commercial software that withholds source code.
* **No Misrepresentation**: Do not fork OpenDM, change the name, erase git history, and claim it as your own creation. Credit where credit is due.
* **Original Contributions Only**: All contributions you submit must be your own original work or licensed under terms compatible with GPL-3.0. Do not submit proprietary or copyrighted code belonging to third parties without authorization.

---

## How Can You Contribute?

You can contribute in many ways:
- Reporting bugs and submitting reproducible test cases.
- Adding package support for Linux distributions (Arch AUR, Debian/Ubuntu PPA, Fedora Copr, Flatpak, AppImage).
- Implementing UI enhancements and desktop environment integrations (KDE, GNOME, XFCE).
- Optimizing network socket throughput and concurrency.
- Translating the browser extension and desktop dialogs to new languages.
- Improving test coverage across different network topologies.

---

## Development Workflow

### 1. Environment Setup
Install the necessary build dependencies (Debian/Ubuntu example):
```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build g++ pkg-config \
    libgtk-4-dev libcurl4-openssl-dev zlib1g-dev libssl-dev
```

### 2. Fork & Clone
```bash
git clone https://github.com/<your-username>/opendm.git
cd opendm
git remote add upstream https://github.com/nadermkhan/opendm.git
```

### 3. Build & Test
Configure and build with Ninja:
```bash
cmake -B build -S . -G Ninja
cmake --build build
```

Run the automated test suite to ensure everything passes:
```bash
ctest --test-dir build --output-on-failure
```

### 4. Create a Branch
```bash
git checkout -b feature/your-feature-name
```

### 5. Coding Standards
* **C++ Standard**: Modern C++20. Use RAII, smart pointers, atomic primitives, and thread-safe locking disciplines.
* **GUI Framework**: Native GTK4 (avoid deprecated GTK3 APIs).
* **Code Formatting**: Clean, consistent indentation and descriptive symbol naming.
* **Tone & Aesthetics**: Zero AI-slop emojis in code, commits, or user interface dialogs. OpenDM preserves authentic, professional desktop application aesthetics.
* **Documentation**: Document rationale for complex networking and concurrency logic.

### 6. Submitting a Pull Request
1. Commit your changes with clear, descriptive commit messages (e.g. `feat(core): ...`, `fix(ui): ...`).
2. Verify all automated tests pass (`ctest --test-dir build --output-on-failure`).
3. Push your branch to your fork (`git push origin feature/your-feature-name`).
4. Open a Pull Request against `main` on the upstream repository [nadermkhan/opendm](https://github.com/nadermkhan/opendm).
5. Describe the problem your PR solves, what changes were made, and how you verified it.

---

## Contributor License Agreement

By submitting a pull request to OpenDM, you agree that your contributions will be licensed under the **GNU General Public License Version 3.0** with author attribution preservation terms. You retain copyright over your individual contributions while granting the project and community rights under the GPL-3.0.
