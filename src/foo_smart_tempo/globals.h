#ifndef __GLOBALS_H__
#define __GLOBALS_H__

#include "foobar2000/SDK/foobar2000.h"
#include "guid.h"

// Config variables
// General
extern cfg_int bpm_config_bpm_write_precision;
extern cfg_string bpm_config_bpm_tag;
extern cfg_string bpm_config_confidence_tag_name;
extern cfg_bool bpm_config_auto_write_tag;
extern cfg_bool bpm_config_write_confidence_tag;
extern cfg_bool cfg_smart_tempo_verbose_logging;
extern cfg_string cfg_smart_tempo_playlist_name;
extern cfg_string cfg_smart_tempo_routing_tag;
extern cfg_string cfg_smart_tempo_genre_rules;
// Manual
extern cfg_int bpm_config_taps_to_average;
extern cfg_int bpm_config_seconds_to_reset_average;

#endif
