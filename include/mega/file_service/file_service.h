#pragma once

#include <mega/common/client_forward.h>
#include <mega/common/shared_mutex.h>
#include <mega/file_service/file_event_observer.h>
#include <mega/file_service/file_event_observer_id.h>
#include <mega/file_service/file_forward.h>
#include <mega/file_service/file_id_forward.h>
#include <mega/file_service/file_info_forward.h>
#include <mega/file_service/file_range_vector.h>
#include <mega/file_service/file_service_callbacks.h>
#include <mega/file_service/file_service_context_pointer.h>
#include <mega/file_service/file_service_forward.h>
#include <mega/file_service/file_service_options_forward.h>
#include <mega/file_service/file_service_result_forward.h>
#include <mega/file_service/file_service_result_or_forward.h>
#include <mega/types.h>

#include <string>

namespace mega
{
namespace file_service
{

class FileService
{
    FileServiceContextPtr mContext;
    common::SharedMutex mContextLock;

public:
    FileService();

    ~FileService();

    // Notify observer when a file changes.
    auto addObserver(FileEventObserver observer) -> FileServiceResultOr<FileEventObserverID>;

    // Create a new file.
    auto create(NodeHandle parent, const std::string& name) -> FileServiceResultOr<File>;

    // Deinitialize the file service.
    void deinitialize();

    // Retrieve information about a file managed by the file service.
    auto info(FileID id) -> FileServiceResultOr<FileInfo>;

    // Initialize the file service.
    auto initialize(common::Client& client, const FileServiceOptions& options) -> FileServiceResult;

    auto initialize(common::Client& client) -> FileServiceResult;

    // Open a file for reading or writing.
    auto open(NodeHandle parent, const std::string& name) -> FileServiceResultOr<File>;
    auto open(FileID id) -> FileServiceResultOr<File>;

    // Update the file service's options.
    auto options(const FileServiceOptions& options) -> FileServiceResult;

    // Retrieve the file service's current options.
    auto options() -> FileServiceResultOr<FileServiceOptions>;

    // Purge all files from storage.
    //
    // This function is intended to be used by integration tests.
    //
    // If you do happen to call it in a different context, be aware that
    // this function will block the caller until all file (or file info)
    // references have been dropped.
    auto purge() -> FileServiceResult;

    // Determine what ranges of a file are currently in storage.
    auto ranges(FileID id) -> FileServiceResultOr<FileRangeVector>;

    // Reclaim storage space.
    void reclaim(ReclaimCallback callback);

    // Remove a previously added file observer.
    auto removeObserver(FileEventObserverID id) -> FileServiceResult;

    // How much storage is the service using?
    auto storageUsed() -> FileServiceResultOr<std::uint64_t>;
}; // FileService

} // file_service
} // mega
