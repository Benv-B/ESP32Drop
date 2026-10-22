#pragma once

// Consumer refusals cross the task queue by value, alongside the matching token.
enum AdStreamError {
  AD_STREAM_ERROR_OUTPUT = 0,
  AD_STREAM_ERROR_LIMIT,
  AD_STREAM_ERROR_UNSUPPORTED,
  AD_STREAM_ERROR_INVALID
};

static inline const char *ad_stream_error_http(enum AdStreamError error) {
  switch (error) {
    case AD_STREAM_ERROR_LIMIT: return "413 Content Too Large";
    case AD_STREAM_ERROR_UNSUPPORTED: return "415 Unsupported Media Type";
    case AD_STREAM_ERROR_INVALID: return "400 Bad Request";
    default: return "502 Bad Gateway";
  }
}
