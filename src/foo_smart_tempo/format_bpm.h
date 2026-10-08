#pragma once

#include <algorithm>  // std::clamp
#include "preferences.h"
#include "globals.h"
#include "pfc/pfc.h"

// ─── Canonical write-precision helper ────────────────────────────────────────
// Single authoritative source — do NOT copy this into individual .cpp files.
[[nodiscard]] inline bpm_precision_enum get_write_precision() noexcept
{
	const int v = static_cast<int>(bpm_config_bpm_write_precision);
	return static_cast<bpm_precision_enum>(
		std::clamp(v, static_cast<int>(BPM_PRECISION_1),
		              static_cast<int>(BPM_PRECISION_2DP)));
}

// ─── BPM string formatter ─────────────────────────────────────────────────────
// Converts a BPM double to a locale-safe string using the configured
// write or display precision.  Implicitly converts to const char*.
class format_bpm
{
public:
	/// Uses the write precision from preferences (bpm_config_bpm_write_precision)
	explicit format_bpm(double p_bpm);

	/// Uses an explicit precision setting
	format_bpm(double p_bpm, bpm_precision_enum p_precision);

	[[nodiscard]] const char* get_ptr() const noexcept { return m_formatter.get_ptr(); }
	[[nodiscard]] const char* toString() const noexcept { return m_formatter.get_ptr(); }
	operator const char*() const noexcept { return m_formatter.get_ptr(); }

private:
	void format(double p_bpm, int p_precision);
	pfc::string_formatter m_formatter;
};
