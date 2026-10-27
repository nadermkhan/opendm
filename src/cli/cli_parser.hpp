#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace opendm {

struct CliOptions {
    std::string url;
    std::string path;
    std::string filename;
    bool start_queue{false};
    bool exit_after_download{false};
    bool add_only{false};       // IDM /a: add without starting
    bool silent_mode{false};     // IDM /n: silent mode
    size_t connections{8};
    uint64_t speed_limit_kb{0}; // 0 = unlimited
    std::string import_file;
    bool is_daemon{false};
    std::string screenshot_path;
    bool show_about{false};
    bool show_appreciation{false};
    bool show_help{false};
    bool show_version{false};
    bool show_gui{true};
};

class CliParser {
public:
    static CliOptions parse(int argc, char* argv[]);
    static void print_help(const char* program_name);
    static void print_version();
};

} // namespace opendm
