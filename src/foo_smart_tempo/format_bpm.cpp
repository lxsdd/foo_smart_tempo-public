#include "stdafx.h"
#include "format_bpm.h"
#include "foo_smart_tempo.h"

#include <cmath>
#include <cstdio>
#include <cstdint>

#include "globals.h"

namespace {
	int sanitize_precision(int v) {
		if (v < BPM_PRECISION_1 || v > BPM_PRECISION_2DP) return (int)BPM_PRECISION_1;
		return v;
	}
}

format_bpm::format_bpm(double p_bpm)
{
	format(p_bpm, sanitize_precision((int)bpm_config_bpm_write_precision));
}

format_bpm::format_bpm(double p_bpm, bpm_precision_enum p_precision)
{
	format(p_bpm, sanitize_precision((int)p_precision));
}

void format_bpm::format(double p_bpm, int p_precision)
{
	m_formatter.reset();
	switch (sanitize_precision(p_precision))
	{
	case BPM_PRECISION_2DP:
		m_formatter << format_float_locale(p_bpm, 2);
		break;
	case BPM_PRECISION_1DP:
		m_formatter << format_float_locale(p_bpm, 1);
		break;
	case BPM_PRECISION_1:
	default:
		m_formatter << pfc::format_int(static_cast<std::int64_t>(std::round(p_bpm)));
		break;
	}
}
