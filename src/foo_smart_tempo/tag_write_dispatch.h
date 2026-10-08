#ifndef __TAG_WRITE_DISPATCH_H__
#define __TAG_WRITE_DISPATCH_H__

#include <atomic>
#include <functional>
#include <memory>

#include "foobar2000/SDK/foobar2000.h"

namespace smart_tempo::tag_write {

bool safe_update_info_async(metadb_handle_list_cref items,
                            service_ptr_t<file_info_filter> filter,
                            const char* contextLabel,
                            HWND parentWindow = NULL,
                            bool showPopupOnFailure = false,
                            std::shared_ptr<std::atomic<uint64_t>> changedItemCounter = nullptr,
                            std::function<void(unsigned)> completionCallback = {});

} // namespace smart_tempo::tag_write

#endif // __TAG_WRITE_DISPATCH_H__
