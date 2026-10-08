#include "stdafx.h"
#include "file_info_filter_scale_bpm.h"
#include "format_bpm.h"
#include "globals.h"

#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace {

bool try_parse_positive_bpm_strict(const char* text, double& outBpm) noexcept {
  if (text == nullptr) {
    return false;
  }

  errno = 0;
  char* parseEnd = nullptr;
  const double parsed = std::strtod(text, &parseEnd);
  if (parseEnd == text || errno == ERANGE || !std::isfinite(parsed) ||
      !(parsed > 0.0)) {
    return false;
  }

  while (*parseEnd != '\0' &&
         std::isspace(static_cast<unsigned char>(*parseEnd)) != 0) {
    ++parseEnd;
  }
  if (*parseEnd != '\0') {
    return false;
  }

  outBpm = parsed;
  return true;
}

}  // namespace

file_info_filter_scale_bpm::file_info_filter_scale_bpm(const char* p_bpm_tag,
                                                       double p_scale)
    : m_bpm_tag(p_bpm_tag), m_scale(p_scale) {}

bool file_info_filter_scale_bpm::apply_filter(metadb_handle_ptr p_track,
                                              t_filestats p_stats,
                                              file_info& p_info) {
  (void)p_track;
  (void)p_stats;

  const char* text = p_info.meta_get(m_bpm_tag, 0);
  double sourceBpm = 0.0;
  if (!try_parse_positive_bpm_strict(text, sourceBpm) ||
      !std::isfinite(m_scale) || !(m_scale > 0.0)) {
    return false;
  }

  const double scaledBpm = sourceBpm * m_scale;
  if (!std::isfinite(scaledBpm) || !(scaledBpm > 0.0)) {
    return false;
  }

  p_info.meta_set(m_bpm_tag, format_bpm(scaledBpm, get_write_precision()));
  return true;
}
