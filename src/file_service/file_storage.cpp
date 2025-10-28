#include <mega/common/client.h>
#include <mega/common/node_info.h>
#include <mega/file_service/file_id.h>
#include <mega/file_service/file_storage.h>
#include <mega/file_service/logging.h>

#include <megafs.h>

namespace mega
{
namespace file_service
{

using namespace common;

FileAccessPtr FileStorage::openFile(const LocalPath& path, bool mustCreate)
{
    // So we can access the filesystem.
    auto file = mFilesystem->newfileaccess(false);

    // Sanity.
    assert(file);

    // Should never happen but hey, just in case.
    if (!file)
        throw FSError1("Couldn't create file access instance");

    // Vulnerable to TOCTOU race.
    if (file->isfile(path) != !mustCreate || !file->fopen(path, true, true, FSLogging::noLogging))
        throw FSErrorF("Couldn't %s file: %s",
                       mustCreate ? "create" : "open",
                       path.toPath(false).c_str());

    // Mark the file as a sparse file if supported.
    file->setSparse();

    // Return the file to our caller.
    return file;
}

FileStorage::FileStorage(const Client& client):
    mFilesystem(std::make_unique<FSACCESS_CLASS>()),
    mStorageDirectory(*mFilesystem, logger(), "file-service", client.dbRootPath()),
    mUserStorageDirectory(*mFilesystem, logger(), client.sessionID(), mStorageDirectory),
    mUserCacheDirectory(*mFilesystem, logger(), "cache", mUserStorageDirectory),
    mFolderLocker(mUserCacheDirectory.path().asPlatformEncoded(true))
{}

FileStorage::~FileStorage() = default;

FileAccessPtr FileStorage::addFile(FileID id)
{
    return openFile(userFilePath(id), true);
}

LocalPath FileStorage::databasePath() const
{
    static const auto name = LocalPath::fromRelativePath("metadata");

    auto path = mUserStorageDirectory.path();

    path.appendWithSeparator(name, true);

    return path;
}

FileAccessPtr FileStorage::getFile(FileID id)
{
    return openFile(userFilePath(id), false);
}

void FileStorage::removeFile(FileID id)
{
    // Compute the file's path.
    auto path = userFilePath(id);

    // File was removed from storage.
    if (mFilesystem->unlinklocal(path))
        return;

    // Couldn't remove the file from storage.
    throw FSErrorF("Couldn't remove file: %s", path.toPath(false).c_str());
}

LocalPath FileStorage::userFilePath(FileID id) const
{
    auto name = LocalPath::fromRelativePath(toString(id));
    auto path = mUserCacheDirectory.path();

    path.appendWithSeparator(name, false);

    return path;
}

} // file_service
} // mega
