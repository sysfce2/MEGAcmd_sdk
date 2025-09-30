#include <mega/file_service/file_context_badge.h>
#include <mega/file_service/file_info.h>
#include <mega/file_service/file_info_context.h>
#include <mega/file_service/file_service_context_badge.h>

namespace mega
{
namespace file_service
{

FileInfo::FileInfo(FileContextBadge, FileInfoContextPtr context):
    mContext(std::move(context))
{}

FileInfo::FileInfo(FileServiceContextBadge, FileInfoContextPtr context):
    mContext(std::move(context))
{}

FileInfo::~FileInfo() = default;

FileEventObserverID FileInfo::addObserver(FileEventObserver observer)
{
    return mContext->addObserver(std::move(observer));
}

std::int64_t FileInfo::accessed() const
{
    return mContext->accessed();
}

std::uint64_t FileInfo::allocatedSize() const
{
    return mContext->allocatedSize();
}

bool FileInfo::dirty() const
{
    return mContext->dirty();
}

NodeHandle FileInfo::handle() const
{
    return mContext->handle();
}

FileID FileInfo::id() const
{
    return mContext->id();
}

std::int64_t FileInfo::modified() const
{
    return mContext->modified();
}

void FileInfo::removeObserver(FileEventObserverID id)
{
    mContext->removeObserver(id);
}

std::uint64_t FileInfo::reportedSize() const
{
    return mContext->reportedSize();
}

std::uint64_t FileInfo::size() const
{
    return mContext->size();
}

} // file_service
} // mega
