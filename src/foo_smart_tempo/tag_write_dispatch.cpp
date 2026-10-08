#include "stdafx.h"

#include "tag_write_dispatch.h"

#include "foo_smart_tempo.h"

namespace smart_tempo::tag_write {

namespace {

bool is_known_embedded_cue_carrier(const char* path) noexcept {
  const char* extension = pfc::string_extension(path != nullptr ? path : "");
  return pfc::stricmp_ascii(extension, "mp3") == 0 ||
         pfc::stricmp_ascii(extension, "flac") == 0 ||
         pfc::stricmp_ascii(extension, "wv") == 0;
}

}  // namespace

target_safety classify_target(const playable_location& location) noexcept {
  if (location.get_subsong() != 0 &&
      is_known_embedded_cue_carrier(location.get_path())) {
    return target_safety::blocked_unsafe_virtual_subsong;
  }
  return target_safety::safe;
}

bool selection_is_safe(metadb_handle_list_cref items,
                       pfc::string_base* firstBlockedPath,
                       uint32_t* firstBlockedSubsong) noexcept {
  for (const auto& item : items) {
    if (!item.is_valid()) {
      continue;
    }
    if (classify_target(item->get_location()) !=
        target_safety::blocked_unsafe_virtual_subsong) {
      continue;
    }
    if (firstBlockedPath != nullptr) {
      *firstBlockedPath = item->get_path();
    }
    if (firstBlockedSubsong != nullptr) {
      *firstBlockedSubsong = item->get_subsong_index();
    }
    return false;
  }
  return true;
}

bool safe_update_info_async(metadb_handle_list_cref items,
                            service_ptr_t<file_info_filter> filter,
                            const char* contextLabel,
                            HWND parentWindow,
                            bool showPopupOnFailure,
                            std::shared_ptr<std::atomic<uint64_t>> changedItemCounter,
                            std::function<void(unsigned)> completionCallback) {
  const char* const safeContext =
      (contextLabel != nullptr && contextLabel[0] != '\0') ? contextLabel : "tag-write";
  const uint64_t queuedItems = static_cast<uint64_t>(items.get_count());
  (void)parentWindow;
  // update_info_async may outlive short-lived component dialogs. Use the stable
  // foobar2000 main window instead of a result/progress dialog that is commonly
  // destroyed immediately after queueing the write.
  const HWND dispatchParent = core_api::get_main_window();

  pfc::string8 blockedPath;
  uint32_t blockedSubsong = 0;
  if (!selection_is_safe(items, &blockedPath, &blockedSubsong)) {
    FB2K_console_formatter()
        << "foo_smart_tempo: [Tag Write] blocked context=" << safeContext
        << ", reason=unsafe-embedded-cue-virtual-subsong"
        << ", path=" << blockedPath.get_ptr()
        << ", subsong=" << blockedSubsong
        << ", items=" << queuedItems;
    if (showPopupOnFailure) {
      pfc::string_formatter message;
      message
          << "Smart Tempo did not write BPM tags because the selection contains "
             "an embedded-Cue virtual track.\n\n"
          << "Direct generic metadata writes to MP3, FLAC or WavPack virtual "
             "subsongs can rewrite the embedded Cue Sheet and unrelated physical "
             "metadata.\n\n"
          << "Analyze the track if desired, then write BPM only to a qualified "
             "physical target or use a dedicated virtual-metadata editor.";
      popup_message::g_show(message.get_ptr(), "Smart Tempo: write blocked");
    }
    return false;
  }

  try {
    pfc::string_formatter queuedLine;
    queuedLine << "foo_smart_tempo: [Tag Write] queued context=" << safeContext
               << ", items=" << queuedItems;
    if (changedItemCounter) {
      queuedLine << ", changed_items=pending";
    }
    queuedLine << ", async=1";
    FB2K_console_formatter() << queuedLine.get_ptr();
    const pfc::string8 notifyContext(safeContext);
    const bool notifyPopup = showPopupOnFailure;
    completion_notify::ptr notify = fb2k::makeCompletionNotify(
        [notifyContext, notifyPopup, queuedItems, changedItemCounter,
         completionCallback = std::move(completionCallback)](
            unsigned code) noexcept {
          try {
            const char* state = "success";
            if (code == metadb_io::update_info_aborted) {
              state = "aborted";
            } else if (code == metadb_io::update_info_errors) {
              state = "errors";
            } else if (code != metadb_io::update_info_success) {
              state = "unknown";
            }
            pfc::string_formatter completedLine;
            completedLine
                << "foo_smart_tempo: [Tag Write] completed context="
                << notifyContext.get_ptr()
                << ", items=" << queuedItems;
            if (changedItemCounter) {
              completedLine
                  << ", changed_items="
                  << changedItemCounter->load(std::memory_order_relaxed);
            }
            completedLine
                << ", status=" << state
                << ", code=" << code;
            FB2K_console_formatter() << completedLine.get_ptr();
            if (code != metadb_io::update_info_success && notifyPopup) {
              fb2k::inMainThread([notifyContext, code]() {
                pfc::string_formatter message;
                message << "Could not update all file tags.\n"
                        << "foobar2000 reported an asynchronous write error"
                        << " (context=" << notifyContext.get_ptr()
                        << ", code=" << code << ").\n\n"
                        << "One or more files may be read-only, locked, or unavailable.";
                popup_message::g_show(message.get_ptr(), "Smart Tempo");
              });
            }
            if (completionCallback) {
              completionCallback(code);
            }
          } catch (...) {
          }
        });
    fb2k::std_api_get<metadb_io_v3>()->update_info_async(
        items, filter, dispatchParent,
        metadb_io_v2::op_flag_background | metadb_io_v2::op_flag_delay_ui,
        notify);
    return true;
  } catch (const pfc::exception& e) {
    FB2K_console_formatter()
        << "foo_smart_tempo: update_info_async failed (" << safeContext
        << ", pfc::exception): " << e.what();
  } catch (...) {
    try {
      throw;
    } catch (const std::exception& e) {
      FB2K_console_formatter()
          << "foo_smart_tempo: update_info_async failed (" << safeContext
          << ", std::exception): " << e.what();
    } catch (...) {
      FB2K_console_formatter()
          << "foo_smart_tempo: update_info_async failed (" << safeContext
          << ", unknown exception)";
    }
  }

  if (showPopupOnFailure) {
    popup_message::g_show(
        "Could not update file tags. File might be read-only or in use by another application.",
        "Smart Tempo");
  }
  return false;
}

} // namespace smart_tempo::tag_write
