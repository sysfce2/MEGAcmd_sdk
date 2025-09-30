#pragma once

#include <mega/file_service/file_callbacks.h>
#include <mega/file_service/file_reclaim_request_forward.h>
#include <mega/file_service/file_request_tags.h>

#include <cstdint>

namespace mega
{
namespace file_service
{

struct FileReclaimRequest
{
    // What kind of request is this?
    using Type = FileWriteRequestTag;

    // This request's human readable name.
    static const char* name()
    {
        return "reclaim";
    }

    // How much space was the file taking when this request was queued?
    std::uint64_t mAllocatedSize;

    // Who should we call when this request has completed?
    FileReclaimCallback mCallback;
}; // FileReclaimRequest

} // file_service
} // mega
