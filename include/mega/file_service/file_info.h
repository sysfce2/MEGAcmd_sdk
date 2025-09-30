#pragma once

#include <mega/file_service/file_context_badge_forward.h>
#include <mega/file_service/file_event_observer.h>
#include <mega/file_service/file_event_observer_id.h>
#include <mega/file_service/file_id_forward.h>
#include <mega/file_service/file_info_context_pointer.h>
#include <mega/file_service/file_info_forward.h>
#include <mega/file_service/file_location_forward.h>
#include <mega/file_service/file_service_context_badge_forward.h>

#include <cstdint>

namespace mega
{

class NodeHandle;

namespace file_service
{

class FileInfo
{
    FileInfoContextPtr mContext;

public:
    FileInfo(FileContextBadge badge, FileInfoContextPtr context);

    FileInfo(FileServiceContextBadge badge, FileInfoContextPtr context);

    ~FileInfo();

    // Does rhs describe the same file as we do?
    bool operator==(const FileInfo& rhs) const
    {
        return mContext == rhs.mContext;
    }

    // Does rhs describe a different file than we do?
    bool operator!=(const FileInfo& rhs) const
    {
        return !operator==(rhs);
    }

    // When was this file last accessed?
    std::int64_t accessed() const;

    // Notify an observer when this file's information changes.
    FileEventObserverID addObserver(FileEventObserver observer);

    // How much disk space has been allocated to this file?
    std::uint64_t allocatedSize() const;

    // Has this file been locally modified?
    bool dirty() const;

    // What node is this file associated with?
    NodeHandle handle() const;

    // What node is this file associated with?
    FileID id() const;

    // Where is this file located in the cloud?
    FileLocation location() const;

    // When was this file last modified?
    std::int64_t modified() const;

    // Remove a previously added observer.
    void removeObserver(FileEventObserverID id);

    // How large does the filesystem say this file is?
    std::uint64_t reportedSize() const;

    // How large is this file conceptually?
    std::uint64_t size() const;
}; // FileInfo

} // file_service
} // mega
