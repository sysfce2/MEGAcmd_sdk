#pragma once

#include <mega/common/lock_forward.h>
#include <mega/fuse/common/ref_forward.h>

#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace mega
{
namespace fuse
{

class FileIOContext;

using FileIOContextLock = common::UniqueLock<const FileIOContext>;
using FileIOContextSharedLock = common::SharedLock<const FileIOContext>;

using FileIOContextPtr = std::unique_ptr<FileIOContext>;
using FileIOContextRef = Ref<FileIOContext>;

using FileIOContextRefVector = std::vector<FileIOContextRef>;

template<typename T>
using ToFileIOContextPtrMap = std::map<T, FileIOContextPtr>;

template<typename T>
using ToFileIOContextRawPtrMap = std::map<T, FileIOContext*>;

template<typename T>
using ToFileIOContextRawPtrMapIterator = typename ToFileIOContextRawPtrMap<T>::iterator;

// Interface to Ref<T>.
void doRef(RefBadge badge, FileIOContext& entry);

void doUnref(RefBadge badge, FileIOContext& entry);

} // fuse
} // mega
