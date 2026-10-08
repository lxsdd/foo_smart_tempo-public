#include "stdafx.h"
#include <cstdlib> // std::strtod

#include "file_info_filter_scale_bpm.h"
#include "format_bpm.h"
#include "globals.h"

file_info_filter_scale_bpm::file_info_filter_scale_bpm(const char* p_bpm_tag, double p_scale)
	: m_bpm_tag(p_bpm_tag)
	, m_scale(p_scale)
{}

bool file_info_filter_scale_bpm::apply_filter(metadb_handle_ptr p_track, t_filestats p_stats, file_info& p_info)
{
	(void)p_track; (void)p_stats;
	const char* str = p_info.meta_get(m_bpm_tag, 0);
	if (str == nullptr) return false;

	// Fix: parse as double (not float) to preserve 2 d.p. precision
	char* parseEnd = nullptr;
	const double bpm = std::strtod(str, &parseEnd) * m_scale;
	if (parseEnd == str || bpm <= 0.0) return false;

	p_info.meta_set(m_bpm_tag, format_bpm(bpm, get_write_precision()));
	return true;
}
