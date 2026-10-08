#include "stdafx.h"
#include "preferences.h"
#include "analysis_window_config.h"

namespace {
static const char* kDefaultGenreRules =
    R"(Hardstyle = center 150.00, spread 12.00
Hard Techno = center 152.50, spread 16.00
Hardcore; Gabber; Mainstream Hardcore = center 180.00, spread 16.00
Frenchcore = center 195.00, spread 18.00
Rave; Oldskool Rave; Bouncy Techno = center 140.00, spread 20.00
Happy Hardcore = center 170.00, spread 16.00
Drum & Bass; Drum And Bass; Drum n Bass; Drum'n'Bass; DnB; D&B; Jungle; Neurofunk; Liquid DnB; Liquid Funk; Jump Up = center 174.00, spread 12.00
Breakcore; Digital Hardcore = center 210.00, spread 18.00
Breakbeat; Breaks; Nu-Skool Breaks; Progressive Breaks = center 130.00, spread 15.00
Happy Breakbeat; Breakbeat Hardcore; Hardcore Breaks = center 165.00, spread 18.00
Big Beat; Chemical Breaks = center 125.00, spread 18.00
Lo-Fi Beats; Boom Bap Beats = center 85.00, spread 25.00
Beat; 60s Beat; Beats = center 120.00, spread 35.00
Eurodance; Hands Up; Italo Dance = center 138.00, spread 15.00
Dance Pop 90s = center 135.00, spread 20.00
Uplifting Trance; Melodic Trance; Club-Trance = center 140.00, spread 14.00
Vocal Trance; Progressive-Vocal-Trance; Vocal-Club-Trance = center 136.00, spread 16.00
Trance = center 138.00, spread 20.00
Progressive Trance; Psy-Prog; Progressive Psytrance = center 135.00, spread 16.00
Psytrance; Psy-Trance; Goa; Goa Trance; Psy = center 144.00, spread 14.00
Euro-House; Euro House; Hip-House; Hip House; Garage House; 90s House = center 125.00, spread 16.00
House; Deep House; Progressive House; Melodic House; Tech House; Acid House; Club-House; Electro House; Future House; Italo House; Minimal-House = center 127.00, spread 14.00
Disco; Nu Disco; Boogie; Italo Disco = center 120.00, spread 18.00
Minimal-Techno; Minimal Techno = center 125.00, spread 12.00
Techno; Peak Time Techno; Hardgroove = center 134.00, spread 14.00
Dubstep; Grime = center 140.00, spread 14.00
Trap = center 145.00, spread 20.00
Halftime; Halftime DnB = center 86.00, spread 12.00
UK Garage; 2-Step = center 130.00, spread 12.00
Bassline; Speed Garage = center 139.00, spread 12.00
Phonk; Drift Phonk; Phonk House = center 135.00, spread 22.00
Amapiano = center 111.00, spread 12.00
Afro House; Gqom = center 122.00, spread 14.00
Hip-Hop; Rap; R&B; RnB; Boom Bap = center 92.50, spread 25.00
Reggaeton; Dancehall; Dembow = center 107.50, spread 18.00
Tribal; Tribal House; Tribal Trance = center 125.00, spread 18.00
Salsa = center 180.00, spread 20.00
Merengue = center 165.00, spread 18.00
Bachata; Cumbia = center 120.00, spread 18.00
Downtempo; Lounge = center 90.00, spread 22.00
Chill; Chillout; Easy Listening = center 85.00, spread 30.00
Dance; Electro; Club; Electronic; EDM = center 125.00, spread 20.00)";
constexpr const char* kDefaultRoutingScript =
    "$if(%genre%,%genre%[; %style%],$if2(%style%,?))";
constexpr const char* kDefaultGenericAnchors =
    "Beat; Beats; Club; Dance; EDM; Electro; Electronic";
constexpr int kDefaultBpmWritePrecision = (int)BPM_PRECISION_2DP;
constexpr int kDefaultAnalysisWorkerMode = (int)ANALYSIS_WORKER_MAX_THROUGHPUT;
}

const char* get_default_routing_script_text() noexcept {
  return kDefaultRoutingScript;
}
const char* get_default_genre_rules_text() noexcept { return kDefaultGenreRules; }
const char* get_default_generic_anchor_tokens_text() noexcept {
  return kDefaultGenericAnchors;
}
int get_default_bpm_write_precision() noexcept {
  return kDefaultBpmWritePrecision;
}
int get_default_analysis_seconds_to_read() noexcept {
  return smart_tempo::kDefaultAnalysisSecondsToRead;
}
int get_default_analysis_sample_passes() noexcept {
  return smart_tempo::kDefaultAnalysisSamplePasses;
}
int clamp_analysis_seconds_to_read(int value) noexcept {
  return smart_tempo::is_supported_analysis_seconds(value)
             ? value
             : get_default_analysis_seconds_to_read();
}
int clamp_analysis_sample_passes(int value) noexcept {
  return smart_tempo::is_supported_analysis_sample_passes(value)
             ? value
             : get_default_analysis_sample_passes();
}


int get_default_analysis_worker_mode() noexcept {
  return kDefaultAnalysisWorkerMode;
}
int clamp_bpm_write_precision(int value) noexcept {
  if (value < (int)BPM_PRECISION_1) return (int)BPM_PRECISION_1;
  if (value > (int)BPM_PRECISION_2DP) return (int)BPM_PRECISION_2DP;
  return value;
}
int clamp_analysis_worker_mode(int value) noexcept {
  if (value < (int)ANALYSIS_WORKER_MAX_THROUGHPUT ||
      value > (int)ANALYSIS_WORKER_CONSERVATIVE) {
    return get_default_analysis_worker_mode();
  }
  return value;
}

// General
cfg_int bpm_config_bpm_write_precision(guid_bpm_config_bpm_write_precision,
                                       get_default_bpm_write_precision());
cfg_string bpm_config_bpm_tag(guid_bpm_config_bpm_tag, "BPM");
cfg_string bpm_config_confidence_tag_name(guid_bpm_config_confidence_tag_name, "BPM_CONFIDENCE");
cfg_bool bpm_config_auto_write_tag(guid_bpm_config_auto_write_tag, false);
cfg_bool bpm_config_write_confidence_tag(guid_bpm_config_write_confidence_tag, false);
cfg_bool cfg_smart_tempo_verbose_logging(guid_cfg_smart_tempo_verbose_logging, false);
cfg_string cfg_smart_tempo_playlist_name(guid_smart_tempo_playlist_name, "Smart Tempo: Unmatched");
cfg_bool cfg_smart_tempo_create_unmatched_playlist(guid_smart_tempo_create_unmatched_playlist, true);
cfg_string cfg_smart_tempo_review_playlist_name(
    guid_smart_tempo_review_playlist_name, "Smart Tempo: Needs BPM Review");
cfg_bool cfg_smart_tempo_create_review_playlist(
    guid_smart_tempo_create_review_playlist, true);
cfg_string cfg_smart_tempo_routing_tag(guid_smart_tempo_routing_tag,
                                       get_default_routing_script_text());
cfg_string cfg_smart_tempo_genre_rules(guid_smart_tempo_genre_rules, get_default_genre_rules_text());
cfg_string cfg_smart_tempo_generic_anchor_tokens(
    guid_smart_tempo_generic_anchor_tokens,
    get_default_generic_anchor_tokens_text());

cfg_int bpm_config_analysis_seconds_to_read(guid_bpm_config_analysis_seconds_to_read,
                                            get_default_analysis_seconds_to_read());
cfg_int bpm_config_analysis_sample_passes(guid_bpm_config_analysis_sample_passes,
                                          get_default_analysis_sample_passes());
cfg_int bpm_config_worker_mode(guid_bpm_config_worker_mode,
                               get_default_analysis_worker_mode());

// Manual
cfg_int bpm_config_taps_to_average(guid_bpm_config_taps_to_average, 30);
cfg_int bpm_config_seconds_to_reset_average(guid_bpm_config_seconds_to_reset_average, 5);

