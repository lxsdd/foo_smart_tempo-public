#include "stdafx.h"
#include "bpm_contextmenu_item.h"
#include "guid.h"
#include "foo_smart_tempo.h"
#include "bpm_auto_analysis_thread.h"
#include "bpm_manual_dialog.h"
#include "file_info_filter_scale_bpm.h"
#include "tag_write_dispatch.h"

#include <memory>

static contextmenu_group_popup_factory g_bpm_context_group(
    guid_bpm_context_group, contextmenu_groups::root, "Smart Tempo", 0);


GUID bpm_contextmenu_item::get_parent()
{
	return guid_bpm_context_group;
}

unsigned bpm_contextmenu_item::get_num_items()
{
	return mnuTotal;
}

void bpm_contextmenu_item::get_item_name(unsigned p_index, pfc::string_base & p_out)
{
	switch (p_index)
	{
	    case mnuAutoAnalysis:
		    p_out = "Smart Tempo Analysis";
			break;
		case mnuMeasuredCandidates:
			p_out = "Measured BPM candidates";
			break;
		case mnuManualAnalysis:
			p_out = "Manually tap BPM for current track";
			break;
		case mnuDoubleBPM:
			p_out = "Double selected BPMs";
			break;
		case mnuHalveBPM:
			p_out = "Halve selected BPMs";
			break;
		default:
			break;
	}
}

void bpm_contextmenu_item::context_command(unsigned p_index, metadb_handle_list_cref p_data, const GUID& p_caller)
{
	switch (p_index)
	{
	    case mnuAutoAnalysis:
			run_auto_analysis(p_data);
			break;
		case mnuMeasuredCandidates:
			run_measured_candidate_analysis(p_data);
			break;
		case mnuManualAnalysis:
			run_manual_analysis(p_data);
			break;
		case mnuDoubleBPM:
			run_scale_bpm(p_data, 2.0);
			break;
		case mnuHalveBPM:
			run_scale_bpm(p_data, 0.5);
			break;
		default:
			break;
	}
}

GUID bpm_contextmenu_item::get_item_guid(unsigned p_index)
{
	switch (p_index)
	{
	    case mnuAutoAnalysis:
			return guid_auto_bpm;
		case mnuMeasuredCandidates:
			return guid_measured_bpm_candidates;
		case mnuManualAnalysis:
			return guid_manual_bpm;
		case mnuDoubleBPM:
			return guid_double_bpm;
		case mnuHalveBPM:
			return guid_halve_bpm;
		default:
			break;
	}

	return pfc::guid_null;
}

bool bpm_contextmenu_item::get_item_description(unsigned p_index, pfc::string_base & p_out)
{
	switch (p_index)
	{
	    case mnuAutoAnalysis:
	        p_out = "Run Smart Tempo analysis for the selected tracks.";
			break;
		case mnuMeasuredCandidates:
			p_out = "Analyze one selected track and open its measured BPM candidates for inspection.";
			break;
		case mnuManualAnalysis:
			p_out = "Manually tap the BPM of the selected track.";
			break;
		case mnuDoubleBPM:
			p_out = "Double the BPM of the selected tracks.";
			break;
		case mnuHalveBPM:
			p_out = "Halve the BPM of the selected tracks.";
			break;
		default:
			return false;
			break;
	}

	return true;
}

void bpm_contextmenu_item::run_auto_analysis(metadb_handle_list_cref p_data)
{
	auto thread = std::make_unique<bpm_auto_analysis_thread>(p_data);
	if (thread->start()) {
		// Ownership is transferred to the modeless dialog lifecycle.
		thread.release();
	}
}

void bpm_contextmenu_item::run_measured_candidate_analysis(
	metadb_handle_list_cref p_data)
{
	if (p_data.get_count() != 1) {
		popup_message::g_show(
			"Select exactly one track to inspect its measured BPM candidates.",
			"Smart Tempo");
		return;
	}

	auto thread = std::make_unique<bpm_auto_analysis_thread>(p_data, true);
	if (thread->start()) {
		// Ownership is transferred to the modeless dialog lifecycle.
		thread.release();
	}
}

void bpm_contextmenu_item::run_manual_analysis(metadb_handle_list_cref p_data)
{
	(void)p_data;
	show_manual_bpm_dialog();
}

void bpm_contextmenu_item::run_scale_bpm(metadb_handle_list_cref p_data, double p_scale)
{
	if (!smart_tempo::tag_write::safe_update_info_async(
		p_data,
		new service_impl_t<file_info_filter_scale_bpm>(bpm_config_bpm_tag.get().get_ptr(), p_scale),
		"scale-bpm-context-menu", NULL, true)) {
		FB2K_console_formatter()
			<< "foo_smart_tempo: Scale BPM action aborted for "
			<< static_cast<uint64_t>(p_data.get_count())
			<< " items due to tag write dispatch failure.";
	}
}

static contextmenu_item_factory_t<bpm_contextmenu_item> contextmenu_factory;







