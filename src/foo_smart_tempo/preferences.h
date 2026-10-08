#pragma once

#include "resource.h"
#include "globals.h"
#include "guid.h"

#include "foobar2000/SDK/foobar2000.h"
#include "foobar2000/helpers/foobar2000+atl.h"

// ─── Enums ─────────────────────────────────────────────────────────────────────
// Plain enums for switch/case and cfg_int compatibility.
// Each enum has an explicit int base to avoid sign/size mismatch warnings.

enum bpm_precision_enum : int
{
	BPM_PRECISION_1   = 0,  ///< Round to nearest integer
	BPM_PRECISION_1DP = 1,  ///< 1 decimal place
	BPM_PRECISION_2DP = 2   ///< 2 decimal places
};

enum analysis_worker_mode_enum : int
{
	ANALYSIS_WORKER_MAX_THROUGHPUT = 0,
	ANALYSIS_WORKER_BALANCED = 1,
	ANALYSIS_WORKER_CONSERVATIVE = 2
};

// ─── Persistent settings (cfg_var_modern) ──────────────────────────────────────

// General
extern cfg_int bpm_config_bpm_write_precision;
extern cfg_string bpm_config_bpm_tag;
extern cfg_string bpm_config_confidence_tag_name;
extern cfg_bool bpm_config_auto_write_tag;
extern cfg_bool bpm_config_write_confidence_tag;
extern cfg_bool cfg_smart_tempo_verbose_logging;
extern cfg_string cfg_smart_tempo_playlist_name;
extern cfg_bool cfg_smart_tempo_create_unmatched_playlist;
extern cfg_string cfg_smart_tempo_review_playlist_name;
extern cfg_bool cfg_smart_tempo_create_review_playlist;
extern cfg_string cfg_smart_tempo_routing_tag;
extern cfg_string cfg_smart_tempo_genre_rules;
extern cfg_string cfg_smart_tempo_generic_anchor_tokens;

const char* get_default_routing_script_text() noexcept;
const char* get_default_genre_rules_text() noexcept;
const char* get_default_generic_anchor_tokens_text() noexcept;
int get_default_bpm_write_precision() noexcept;
int get_default_analysis_seconds_to_read() noexcept;
int get_default_analysis_sample_passes() noexcept;
int get_default_analysis_worker_mode() noexcept;
int clamp_bpm_write_precision(int value) noexcept;
int clamp_analysis_worker_mode(int value) noexcept;
extern cfg_int bpm_config_worker_mode;

// Manual
extern cfg_int bpm_config_taps_to_average;
extern cfg_int bpm_config_seconds_to_reset_average;
