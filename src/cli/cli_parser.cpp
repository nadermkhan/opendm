#include "cli_parser.hpp"
#include <iostream>
#include <cstring>
#include <cstdlib>

namespace opendm {

CliOptions CliParser::parse(int argc, char* argv[]) {
    CliOptions opts;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-d" || arg == "/d" || arg == "--download") {
            if (i + 1 < argc) {
                opts.url = argv[++i];
            }
        } else if (arg == "-p" || arg == "/p" || arg == "--path") {
            if (i + 1 < argc) {
                opts.path = argv[++i];
            }
        } else if (arg == "-f" || arg == "/f" || arg == "--filename") {
            if (i + 1 < argc) {
                opts.filename = argv[++i];
            }
        } else if (arg == "-s" || arg == "/s" || arg == "--start-queue") {
            opts.start_queue = true;
        } else if (arg == "-q" || arg == "/q" || arg == "--quit-after") {
            opts.exit_after_download = true;
        } else if (arg == "-a" || arg == "/a" || arg == "--add-only") {
            opts.add_only = true;
        } else if (arg == "-n" || arg == "/n" || arg == "--silent") {
            opts.silent_mode = true;
            opts.show_gui = false;
        } else if (arg == "-c" || arg == "--connections") {
            if (i + 1 < argc) {
                opts.connections = std::strtoul(argv[++i], nullptr, 10);
            }
        } else if (arg == "-l" || arg == "--speed-limit") {
            if (i + 1 < argc) {
                opts.speed_limit_kb = std::strtoul(argv[++i], nullptr, 10);
            }
        } else if (arg == "-i" || arg == "/i" || arg == "--import") {
            if (i + 1 < argc) {
                opts.import_file = argv[++i];
            }
        } else if (arg == "--daemon") {
            opts.is_daemon = true;
            opts.show_gui = false;
        } else if (arg == "--no-gui") {
            opts.show_gui = false;
        } else if (arg == "-h" || arg == "--help" || arg == "/?") {
            opts.show_help = true;
        } else if (arg == "-v" || arg == "--version") {
            opts.show_version = true;
        } else if (arg.rfind("http://", 0) == 0 || arg.rfind("https://", 0) == 0 || arg.rfind("ftp://", 0) == 0) {
            // Positional URL
            if (opts.url.empty()) {
                opts.url = arg;
            }
        }
    }

    return opts;
}

void CliParser::print_help(const char* program_name) {
    std::cout << "OpenDM - Linux High-Speed Download Accelerator\n"
              << "Compatible with Internet Download Manager (IDM) CLI switches\n\n"
              << "Usage: " << program_name << " [OPTIONS] [URL]\n\n"
              << "Options:\n"
              << "  -d, /d <URL>             Download URL\n"
              << "  -p, /p <PATH>            Destination directory path\n"
              << "  -f, /f <FILENAME>        Output file name\n"
              << "  -a, /a                   Add URL to download queue without starting\n"
              << "  -s, /s                   Start queue processing immediately\n"
              << "  -q, /q                   Exit OpenDM after download completes\n"
              << "  -n, /n                   Silent mode (suppress GUI dialogs)\n"
              << "  -c, --connections <N>    Number of concurrent connections/chunks (default: 8)\n"
              << "  -l, --speed-limit <KB/s> Speed limit in KB/s (0 = unlimited)\n"
              << "  -i, --import <FILE>      Import batch download list (.ef2 or text file)\n"
              << "  --daemon                 Run in background listening for IPC requests\n"
              << "  --no-gui                 Run in headless command-line mode\n"
              << "  -h, --help               Show this help message\n"
              << "  -v, --version            Display version information\n\n"
              << "Examples:\n"
              << "  " << program_name << " -d https://example.com/file.iso -p ~/Downloads\n"
              << "  " << program_name << " -d https://example.com/video.mp4 -f clip.mp4 -c 16\n"
              << "  " << program_name << " -a -d https://example.com/archive.zip\n"
              << "  " << program_name << " -s\n";
}

void CliParser::print_version() {
    std::cout << "OpenDM 1.0.0 (Linux x86_64, GTK4, libcurl)\n"
              << "Engine: IDM 6.43.15 Algorithm Reimplementation\n";
}

} // namespace opendm
