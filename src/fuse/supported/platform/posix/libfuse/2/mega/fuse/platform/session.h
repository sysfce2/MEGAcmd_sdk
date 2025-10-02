#pragma once

#include <mega/fuse/platform/session_base.h>

namespace mega
{
namespace fuse
{
namespace platform
{

// How we communicate with FUSE.
class Session: public SessionBase
{
    class ChannelDeleter
    {
        Mount* mMount;

    public:
        ChannelDeleter(Mount& mount);

        void operator()(fuse_chan* channel);
    }; // ChannelDeleter

    using ChannelPtr = std::unique_ptr<fuse_chan, ChannelDeleter>;

    static void forget(fuse_req_t request, fuse_ino_t inode, unsigned long num);

    // Retrieve the next request from FUSE.
    std::string nextRequest();

    void populateOperations(fuse_lowlevel_ops& operations) override;

    static void rename(fuse_req_t request,
                       fuse_ino_t sourceParent,
                       const char* sourceName,
                       fuse_ino_t targetParent,
                       const char* targetName);

    ChannelPtr mChannel;

public:
    Session(Mount& mount);

    ~Session() = default;

    // What descriptor is the session using to communicate with FUSE?
    int descriptor() const override;

    // Dispatch a request received from FUSE.
    void dispatch() override;

    // Invalidate an inode's data.
    void invalidateData(MountInodeID id, off_t offset, off_t size) override;

    // Invalidate a specific directory entry.
    void invalidateEntry(const std::string& name, MountInodeID child, MountInodeID parent) override;

    void invalidateEntry(const std::string& name, MountInodeID parent) override;
}; // Session

} // platform
} // fuse
} // mega
