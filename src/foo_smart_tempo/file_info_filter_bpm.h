#ifndef __FILE_INFO_FILTER_BPM_H__
#define __FILE_INFO_FILTER_BPM_H__

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "foobar2000/SDK/foobar2000.h"

class file_info_filter_bpm : public file_info_filter
{
public:
	file_info_filter_bpm(const metadb_handle_list & p_tracks, const char * p_bpm_tag,
		const std::vector<double> & p_bpm_results,
		const std::vector<double> * p_confidence_results = nullptr,
		std::shared_ptr<std::atomic<uint64_t>> p_changed_item_counter = nullptr,
		const std::vector<uint8_t> * p_suppress_write_results = nullptr);
	file_info_filter_bpm(metadb_handle_ptr p_track, const char * p_bpm_tag, double p_bpm_result,
		double p_confidence_result = -1.0,
		std::shared_ptr<std::atomic<uint64_t>> p_changed_item_counter = nullptr,
		bool p_suppress_write_result = false);
	bool apply_filter(metadb_handle_ptr p_track, t_filestats p_stats, file_info & p_info);

private:
	metadb_handle_list m_tracks;
	std::vector<double> m_bpm_results;
	std::vector<double> m_confidence_results;
	std::vector<uint8_t> m_suppress_write_results;
	std::shared_ptr<std::atomic<uint64_t>> m_changed_item_counter;
	pfc::string8 m_bpm_tag;
};

#endif // __FILE_INFO_FILTER_BPM_H__
