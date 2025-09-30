#pragma once

#include <mega/common/database.h>
#include <mega/common/task_executor.h>
#include <mega/fuse/common/file_cache.h>
#include <mega/fuse/common/file_extension_db.h>
#include <mega/fuse/common/inode_cache.h>
#include <mega/fuse/common/inode_db.h>
#include <mega/fuse/common/service_context.h>
#include <mega/fuse/common/service_flags.h>
#include <mega/fuse/platform/mount_db.h>
#include <mega/fuse/platform/service_context_forward.h>
#include <mega/fuse/platform/unmounter.h>
#include <mega/types.h>

namespace mega
{
namespace fuse
{
namespace platform
{

class ServiceContext: public fuse::ServiceContext
{
public:
    using fuse::ServiceContext::serviceFlags;

    ServiceContext(const ServiceFlags& flags, Service& service);

    ~ServiceContext();

    // Add a mount to the database.
    MountResult add(const MountInfo& info) override;

    // Check if a file exists in the cache.
    bool cached(common::NormalizedPath path) const override;

    // Called by the client when its view of the cloud is current.
    void current() override;

    // Describe the inode representing the file at the specified path.
    common::ErrorOr<InodeInfo> describe(const common::NormalizedPath& path) const override;

    // Disable an enabled mount.
    void disable(MountDisabledCallback callback, const std::string& name, bool remember) override;

    // Discard node events.
    MountResult discard(bool discard) override;

    // Downgrade the FUSE database to the specified version.
    MountResult downgrade(const LocalPath& path, std::size_t target) override;

    // Enable a disabled mount.
    MountResult enable(const std::string& name, bool remember) override;

    // Query whether a specified mount is enabled.
    bool enabled(const std::string& name) const override;

    // Execute a function on some thread.
    common::Task execute(std::function<void(const common::Task&)> function) override;

    // Update a mount's flags.
    MountResult flags(const std::string& name, const MountFlags& flags) override;

    // Query a mount's flags.
    MountFlagsPtr flags(const std::string& name) const override;

    // Get our hands on the client's filesystem access instance.
    FileSystemAccess& fsAccess() const;

    // Describe the mount associated with path.
    MountInfoPtr get(const std::string& name) const override;

    // Describe all (enabled) mounts.
    MountInfoVector get(bool onlyEnabled) const override;

    // Retrieve the path of the mounts associated with name.
    common::NormalizedPath path(const std::string& name) const override;

    // Remove a disabled mount from the database.
    MountResult remove(const std::string& name) override;

    // Update the service's flags.
    void serviceFlags(const ServiceFlags& flags) override;

    // Check whether the specified path is "syncable."
    bool syncable(const common::NormalizedPath& path) const override;

    // Called by the client when nodes have been changed in the cloud.
    void updated(common::NodeEventQueue& events) override;

    // Update the FUSE database to the specified version.
    MountResult upgrade(const LocalPath& path, std::size_t target) override;

    common::Database mDatabase;
    common::TaskExecutor mExecutor;
    FileExtensionDB mFileExtensionDB;
    InodeDB mInodeDB;
    FileCache mFileCache;
    InodeCache mInodeCache;
    Unmounter mUnmounter;
    MountDB mMountDB;
}; // ServiceContext

} // platform
} // fuse
} // mega
