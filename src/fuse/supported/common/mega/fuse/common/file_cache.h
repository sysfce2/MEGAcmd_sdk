#pragma once

#include <mega/common/client_forward.h>
#include <mega/common/error_or_forward.h>
#include <mega/common/lockable.h>
#include <mega/common/task_executor_forward.h>
#include <mega/filesystem.h>
#include <mega/fuse/common/file_cache_forward.h>
#include <mega/fuse/common/file_extension_db_forward.h>
#include <mega/fuse/common/file_info_forward.h>
#include <mega/fuse/common/file_inode_forward.h>
#include <mega/fuse/common/file_io_context_forward.h>
#include <mega/fuse/common/inode_id_forward.h>
#include <mega/fuse/common/mount_forward.h>
#include <mega/fuse/platform/platform.h>
#include <mega/fuse/platform/service_context_forward.h>
#include <mega/fuse/platform/utility.h>

#include <condition_variable>

namespace mega
{
namespace common
{

template<>
struct LockableTraits<fuse::FileCache>:
    public LockableTraitsCommon<fuse::FileCache, std::recursive_mutex>
{}; // LockableTraits<fuse::FileCache>

} // common

namespace fuse
{

class FileCache: public common::Lockable<FileCache>
{
    friend class FileIOContext;
    friend class FileInfo;

    // Create a new file description based on the file at the specified path.
    //
    // If create is false, this function will return a description only if
    // the file already exists at the specified location.
    //
    // If create is true, a new file will be created at the specified location.
    common::ErrorOr<FileInfoRef> create(const FileExtension& extension,
                                        const LocalPath& path,
                                        InodeID id,
                                        FileAccessSharedPtr* fileAccess,
                                        bool create);

    // Get a reference to an inode's file info.
    //
    // If no info is currently associated with the specified inode,
    // a new file info instance will be created based on the values
    // contained in the specified file access instance.
    FileInfoRef info(const FileExtension& extension, const FileAccess& fileAccess, InodeID id);

    // Purge unreferenced files from the cache.
    void purge();

    // Remove context from the index.
    void remove(const FileIOContext& context, FileCacheLock lock);

    // Remove info from the index.
    void remove(const FileInfo& info, FileCacheLock lock);

    // Tracks which context is associated with what inode.
    mutable ToFileIOContextPtrMap<InodeID> mContextByID;

    // Tracks which info is associated with what inode.
    mutable ToFileInfoPtrMap<InodeID> mInfoByID;

    // Signalled when a context or info instance is removed.
    std::condition_variable_any mRemoved;

    WINDOWS_ONLY(platform::FolderLocker mFolderLocker);

public:
    explicit FileCache(platform::ServiceContext& context);

    ~FileCache();

    // Cancel pending uploads and wait for contexts to drain.
    void cancel();

    // What client are we using to transfer data?
    common::Client& client() const;

    // Retrieve a reference to an inode's file context.
    FileIOContextRef context(FileInodeRef file, bool inMemoryOnly = false) const;

    // Create a new file description based on a file already in the cache.
    common::ErrorOr<FileInfoRef> create(const FileExtension& extension,
                                        const LocalPath& path,
                                        InodeID id,
                                        FileAccessSharedPtr* fileAccess = nullptr);

    // Create an empty file and return its description.
    common::ErrorOr<FileInfoRef> create(const FileExtension& extension,
                                        InodeID id,
                                        FileAccessSharedPtr* fileAccess = nullptr,
                                        LocalPath* filePath = nullptr);

    // Called by the client when its view of the cloud is current.
    void current();

    // Who do we call when we want to execute something on another thread?
    common::TaskExecutor& executor() const;

    // Flush zero or more modified inodes to the cloud.
    void flush(const Mount& mount, FileInodeRefVector inodes);

    // Get a reference to an inode's file info.
    //
    // If inMemoryOnly is false and no info is currently associated with the
    // specified inode, a new file info instance will be created based on
    // the file representing this inode's cached content.
    FileInfoRef info(const FileExtension& extension, InodeID id, bool inMemoryOnly = false) const;

    // Where is an inode's local state located?
    LocalPath path(const FileExtension& extension, InodeID id) const;

    // Remove an inode's content from the cache.
    void remove(const FileExtension& extension, InodeID id);

    // Where is the cache storing its data?
    const LocalPath mCachePath;

    // Which context owns this cache?
    platform::ServiceContext& mContext;
}; // FileCache

} // fuse
} // mega
