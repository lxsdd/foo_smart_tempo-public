#include "stdafx.h"

#include "file_info_filter_bpm.h"

#include "analysis_telemetry.h"
#include "format_bpm.h"
#include "globals.h"
#include "preferences.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace {
bool should_write_formatted_bpm_tag(file_info& info, const char* tagName,
                                    const char* formattedValue) {
	if (formattedValue == nullptr) return false;
	const char* existing = info.meta_get(tagName, 0);
	if (existing == nullptr) return true;
	return std::strcmp(existing, formattedValue) != 0;
}

bool should_write_text_tag(file_info& info, const char* tagName, const char* newText) {
	const char* existing = info.meta_get(tagName, 0);
	if (existing == nullptr) return true;
	return std::strcmp(existing, newText) != 0;
}
} // namespace

file_info_filter_bpm::file_info_filter_bpm(const metadb_handle_list & p_tracks,
	const char * p_bpm_tag, const std::vector<double> & p_bpm_results,
	const std::vector<double> * p_confidence_results,
	std::shared_ptr<std::atomic<uint64_t>> p_changed_item_counter,
	const std::vector<uint8_t> * p_suppress_write_results)
	: m_changed_item_counter(std::move(p_changed_item_counter))
	, m_bpm_tag(p_bpm_tag)
{
	pfc::dynamic_assert(p_tracks.get_count() == p_bpm_results.size());
	if (p_confidence_results != nullptr) {
		pfc::dynamic_assert(p_tracks.get_count() == p_confidence_results->size());
	}
	if (p_suppress_write_results != nullptr) {
		pfc::dynamic_assert(p_tracks.get_count() == p_suppress_write_results->size());
	}
	pfc::array_t<t_size> order;
	order.set_size(p_tracks.get_count());
	order_helper::g_fill(order.get_ptr(), order.get_size());
	p_tracks.sort_get_permutation_t(pfc::compare_t<metadb_handle_ptr, metadb_handle_ptr>, order.get_ptr());
	m_tracks.set_count(order.get_size());
	m_bpm_results.resize(order.get_size());
	m_confidence_results.assign(order.get_size(), -1.0);
	m_suppress_write_results.assign(order.get_size(), 0);

	for(t_size n = 0; n < order.get_size(); n++)
	{
		m_tracks[n] = p_tracks[order[n]];
		m_bpm_results[n] = p_bpm_results[order[n]];
		if (p_confidence_results != nullptr) {
			m_confidence_results[n] = (*p_confidence_results)[order[n]];
		}
		if (p_suppress_write_results != nullptr) {
			m_suppress_write_results[n] = (*p_suppress_write_results)[order[n]];
		}
	}
}

file_info_filter_bpm::file_info_filter_bpm(metadb_handle_ptr p_track,
	const char * p_bpm_tag, double p_bpm_result, double p_confidence_result,
	std::shared_ptr<std::atomic<uint64_t>> p_changed_item_counter,
	bool p_suppress_write_result)
	: m_changed_item_counter(std::move(p_changed_item_counter))
	, m_bpm_tag(p_bpm_tag)
{
	m_tracks.add_item(p_track);
	m_bpm_results.push_back(p_bpm_result);
	m_confidence_results.push_back(p_confidence_result);
	m_suppress_write_results.push_back(p_suppress_write_result ? 1 : 0);
}

bool file_info_filter_bpm::apply_filter(metadb_handle_ptr p_track, t_filestats p_stats, file_info & p_info)
{
	t_size index;
	if (m_tracks.bsearch_t(pfc::compare_t<metadb_handle_ptr, metadb_handle_ptr>, p_track, index))
	{
		if (index < m_suppress_write_results.size() && m_suppress_write_results[index] != 0) {
			if (smart_tempo::verbose_console_logging_enabled()) {
				FB2K_console_formatter()
					<< "foo_smart_tempo: Skip tag write due to MIR primary review hold for "
					<< pfc::string_filename_ext(p_track->get_path());
			}
			return false;
		}
		bool changed = false;
		const double bpmRaw = m_bpm_results[index];
		const bool analysisSucceeded = std::isfinite(bpmRaw) && bpmRaw > 0.0;

		if (analysisSucceeded) {
			format_bpm bpm_value(bpmRaw, get_write_precision());
			if (should_write_formatted_bpm_tag(p_info, m_bpm_tag, bpm_value)) {
				if (smart_tempo::verbose_console_logging_enabled()) {
					FB2K_console_formatter() << "foo_smart_tempo: Writing tag %" << m_bpm_tag << "% = " << bpm_value << " (raw=" << bpmRaw << ", prec=" << (int)get_write_precision() << ")";
				}
				p_info.meta_set(m_bpm_tag, bpm_value);
				changed = true;
			}
		} else if (smart_tempo::verbose_console_logging_enabled()) {
			FB2K_console_formatter() << "foo_smart_tempo: Skip BPM tag write due to invalid analysis result (raw=" << bpmRaw << ")";
		}
		if (bpm_config_write_confidence_tag && index < m_confidence_results.size()) {
			const double rawConfidence = m_confidence_results[index];

			// Keep an owning string object so tag name memory stays valid.
			pfc::string8 tagNameStr = bpm_config_confidence_tag_name.get();
			if (tagNameStr.is_empty()) {
				tagNameStr = "BPM_CONFIDENCE";
			}

			double confidenceForWrite = std::isfinite(rawConfidence) ? rawConfidence : 0.0;
			if (!analysisSucceeded) {
				// Failed/invalid analysis must still write a deterministic zero value.
				confidenceForWrite = 0.0;
			}
			const double clampedConfidence =
			    (std::max)(0.0, (std::min)(100.0, confidenceForWrite));

			pfc::string8 confidenceValueText;
			if (clampedConfidence <= 0.0) {
				confidenceValueText = "0";
			} else {
				const int roundedConfidence = (int)std::lround(clampedConfidence);
				pfc::string_formatter confidenceValue;
				confidenceValue << roundedConfidence;
				confidenceValueText = confidenceValue;
				if (confidenceValueText.is_empty()) {
					confidenceValueText = "0";
				}
			}

			if (should_write_text_tag(p_info, tagNameStr.get_ptr(), confidenceValueText.get_ptr())) {
				p_info.meta_set(tagNameStr.get_ptr(), confidenceValueText.get_ptr());
				changed = true;
				if (smart_tempo::verbose_console_logging_enabled()) {
					FB2K_console_formatter() << "foo_smart_tempo: Writing confidence tag %"
					                         << tagNameStr.get_ptr()
					                         << "% = " << confidenceValueText.get_ptr();
				}
			}
		}
		if (changed && m_changed_item_counter) {
			m_changed_item_counter->fetch_add(1, std::memory_order_relaxed);
		}
		return changed;
	}
	else
	{
		return false;
	}
}


