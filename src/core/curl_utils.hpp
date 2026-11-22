#pragma once

#include <curl/curl.h>

namespace opendm {
namespace net {

// Applies Windows Native CA trust store (CURLSSLOPT_NATIVE_CA) and bundled fallback CA certificate if present.
void configure_curl_ssl(CURL* curl);

} // namespace net
} // namespace opendm
