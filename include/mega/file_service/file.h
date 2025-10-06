#pragma once

#include <mega/file_service/file_callbacks.h>
#include <mega/file_service/file_context_pointer.h>
#include <mega/file_service/file_event_observer.h>
#include <mega/file_service/file_event_observer_id.h>
#include <mega/file_service/file_forward.h>
#include <mega/file_service/file_info_forward.h>
#include <mega/file_service/file_range_forward.h>
#include <mega/file_service/file_range_vector.h>
#include <mega/file_service/file_service_context_badge_forward.h>

namespace mega
{

class LocalPath;
class NodeHandle;

namespace file_service
{

class File
{
    FileContextPtr mContext;

public:
    File(FileServiceContextBadge badge, FileContextPtr context);

    File(const File& other) = default;

    File(File&& other);

    ~File();

    File& operator=(const File& rhs) = default;

    File& operator=(File&& rhs);

    // Notify an observer when this file's information changes.
    FileEventObserverID addObserver(FileEventObserver observer);

    // Append data to the end of this file.
    void append(const void* buffer, FileAppendCallback callback, std::uint64_t length);

    // Fetch all of this file's data from the cloud.
    void fetch(FileFetchCallback callback);

    // Wait until all fetches in progress have completed.
    void fetchBarrier(FileFetchBarrierCallback callback);

    // Flush this file's local modifications to the cloud.
    void flush(FileFlushCallback callback);

    // Retrieve information about this file.
    FileInfo info() const;

    // Purge this file from the service.
    void purge(FilePurgeCallback callback);

    // What ranges of this file are currently in storage?
    FileRangeVector ranges() const;

    // Read data from this file.
    //
    // Be aware that this function does not guarantee that all data
    // requested will be provided in a single chunk.
    //
    // If you request 1MiB of data, you may get the entire 1MiB or you might
    // get much less. The information passed to the callback will specify
    // how much data you've received and it's your responsibility to request
    // the rest, if necessary.
    void read(FileReadCallback callback, std::uint64_t offset, std::uint64_t length);

    void read(FileReadCallback callback, const FileRange& range);

    // Reclaim this file's storage.
    void reclaim(FileReclaimCallback callback);

    // Remove the file.
    //
    // Like purge above but the cloud file is removed, too.
    void remove(FileRemoveCallback callback, bool replaced);

    // Remove a previously added observer.
    void removeObserver(FileEventObserverID id);

    // Update the file's modification time.
    void touch(FileTouchCallback callback, std::int64_t modified);

    // Truncate this file to a specified size.
    void truncate(FileTruncateCallback callback, std::uint64_t newSize);

    // Write data to this file.
    void write(const void* buffer,
               FileWriteCallback callback,
               std::uint64_t offset,
               std::uint64_t length);

    void write(const void* buffer, FileWriteCallback callback, const FileRange& range);
}; // File

} // file_service
} // mega
