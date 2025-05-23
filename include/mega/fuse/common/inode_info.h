#pragma once

#include <string>

#include <mega/common/node_info_forward.h>
#include <mega/fuse/common/inode_id.h>
#include <mega/fuse/common/inode_info_forward.h>

#include <mega/types.h>

namespace mega
{
namespace fuse
{

struct InodeInfo
{
    InodeInfo() = default;

    explicit InodeInfo(common::NodeInfo info);

    InodeInfo(InodeID id, common::NodeInfo info);

    InodeID mID;
    bool mIsDirectory;
    m_time_t mModified;
    std::string mName;
    InodeID mParentID;
    accesslevel_t mPermissions;
    m_off_t mSize;
}; // InodeInfo

} // fuse
} // mega

