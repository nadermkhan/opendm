#include "curl_utils.hpp"
#include "win32_unicode.hpp"

#include <filesystem>
#include <system_error>

namespace opendm {
namespace net {

void configure_curl_ssl(CURL* curl) {
    if (!curl) return;

#ifdef _WIN32
#ifdef CURLSSLOPT_NATIVE_CA
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#endif
    static const std::string bundled_ca = []() -> std::string {
        std::string exe_dir = sys::get_exe_dir_utf8();
        if (exe_dir.empty()) return {};
        std::filesystem::path ca_path = sys::path_from_utf8(exe_dir) / "ssl" / "certs" / "ca-bundle.crt";
        std::error_code ec;
        if (std::filesystem::exists(ca_path, ec)) {
            return sys::path_to_utf8(ca_path);
        }
        return {};
    }();
    if (!bundled_ca.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, bundled_ca.c_str());
    }
#endif
}

} // namespace net
} // namespace opendm
