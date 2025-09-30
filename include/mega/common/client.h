#pragma once

#include <mega/common/client_callbacks.h>
#include <mega/common/client_forward.h>
#include <mega/common/error_or_forward.h>
#include <mega/common/logger_forward.h>
#include <mega/common/node_event_observer_forward.h>
#include <mega/common/node_info_forward.h>
#include <mega/common/normalized_path_forward.h>
#include <mega/common/partial_download_callback_forward.h>
#include <mega/common/partial_download_forward.h>
#include <mega/common/task_queue_forward.h>
#include <mega/common/upload_callbacks.h>
#include <mega/common/upload_forward.h>
#include <mega/types.h>

#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <tuple>

namespace mega
{

struct MegaApp;

namespace common
{

// A high-level interface to MEGA's cloud storage.
class Client
{
protected:
    explicit Client(Logger& logger);

    // Who should we notify when something changes in the cloud?
    std::set<NodeEventObserver*> mEventObservers;

    // Serializes access to mEventObservers.
    std::recursive_mutex mEventObserversLock;

    // What logger should we use?
    Logger& mLogger;

public:
    virtual ~Client();

    // Notify observer when something changes in the cloud.
    void addEventObserver(NodeEventObserver& observer);

    // What application is our client bound to?
    virtual MegaApp& application() = 0;

    // Retrieve the names of a parent's children.
    virtual std::set<std::string> childNames(NodeHandle parent) const = 0;

    // Compute a suitable path for a database.
    virtual LocalPath dbPath(const std::string& name) const = 0;

    // Query where databases should be stored.
    virtual LocalPath dbRootPath() const = 0;

    // Deinitialize the client.
    virtual void deinitialize() = 0;

    // Remove a sync previously created with synchronize(...)
    virtual void desynchronize(mega::handle id) = 0;

    // Download a file from the cloud.
    virtual void download(DownloadCallback callback,
                          NodeHandle handle,
                          const LocalPath& logicalPath,
                          const LocalPath& physicalPath) = 0;

    // Execute a function for each child of a node.
    virtual void each(std::function<void(NodeInfo)> function,
                      NodeHandle handle) const = 0;

    // Execute some function on the client's thread.
    virtual Task execute(std::function<void(const Task&)> function) = 0;

    // Query whether a node exists in the cloud.
    virtual bool exists(NodeHandle handle) const = 0;

    // Request access the local filesystem.
    virtual FileSystemAccess& fsAccess() const = 0;

    // Retrieve a description of a specific node.
    virtual ErrorOr<NodeInfo> get(NodeHandle handle) const = 0;

    // Retrieve a description of a specific child.
    virtual ErrorOr<NodeInfo> get(NodeHandle parent,
                                  const std::string& name) const = 0;

    // Query what a child's node handle is.
    virtual NodeHandle handle(NodeHandle parent,
                              const std::string& name) const = 0;

    // Query whether a parent contains any children.
    virtual ErrorOr<bool> hasChildren(NodeHandle parent) const = 0;

    // Initialize the client for use.
    virtual void initialize() = 0;

    // Check whether a node is a file.
    virtual ErrorOr<bool> isFile(NodeHandle handle) const = 0;

    // What logger is this client using?
    Logger& logger() const;

    // Look up a cloud node by path.
    template<typename T>
    auto lookup(const T& path, NodeHandle parent)
      -> typename EnableIfPath<T, ErrorOr<NodeInfo>>::type;

    // Make a new directory in the cloud.
    virtual void makeDirectory(MakeDirectoryCallback callback,
                               const std::string& name,
                               NodeHandle parent) = 0;

    // Make a new directory in the cloud.
    ErrorOr<NodeInfo> makeDirectory(const std::string& name,
                                    NodeHandle parent);

    // Check if path is "mountable."
    //
    // That is, totally unrelated to any active sync.
    virtual bool mountable(const NormalizedPath& path) const = 0;

    // Rename source to name and move it to target.
    void move(MoveCallback callback,
              const std::string& name,
              NodeHandle source,
              NodeHandle target);

    Error move(const std::string& name,
               NodeHandle source,
               NodeHandle target);

    // Move source to target.
    virtual void move(MoveCallback callback,
                      NodeHandle source,
                      NodeHandle target) = 0;

    Error move(NodeHandle source,
               NodeHandle target);

    // Query who a node's parent is.
    virtual NodeHandle parentHandle(NodeHandle handle) const = 0;

    // Download part of a file from the cloud.
    virtual auto partialDownload(PartialDownloadCallback& callback,
                                 NodeHandle handle,
                                 std::uint64_t offset,
                                 std::uint64_t length) -> ErrorOr<PartialDownloadPtr> = 0;

    // What permissions are applicable to a node?
    virtual accesslevel_t permissions(NodeHandle handle) const = 0;

    // Remove a node.
    virtual void remove(RemoveCallback callback,
                        NodeHandle handle) = 0;

    Error remove(NodeHandle handle);

    // Remove all children of a node.
    Error removeAll(NodeHandle handle);

    // Don't send observer any further change notifications.
    void removeEventObserver(NodeEventObserver& observer);

    // Rename a node.
    virtual void rename(RenameCallback callback,
                        const std::string& name,
                        NodeHandle handle) = 0;

    Error rename(const std::string& name,
                 NodeHandle handle);

    // Replace target with source.
    Error replace(NodeHandle source,
                  NodeHandle target);

    // Retrieve the client's current session ID.
    virtual std::string sessionID() const = 0;

    // Retrieve storage statistics from the cloud.
    virtual void storageInfo(StorageInfoCallback callback) = 0;

    ErrorOr<StorageInfo> storageInfo();

    // Synchronize a local tree against some location in the cloud.
    virtual auto synchronize(const NormalizedPath& path, NodeHandle target)
      -> std::tuple<mega::handle, Error, SyncError> = 0;

    // Update a file's modification time.
    virtual void touch(TouchCallback callback,
                       NodeHandle handle,
                       m_time_t modified) = 0;

    Error touch(NodeHandle handle, m_time_t modified);

    // Upload a file to the cloud.
    virtual UploadPtr upload(const LocalPath& logicalPath,
                             const std::string& name,
                             NodeHandle parent,
                             const LocalPath& physicalPath) = 0;
}; // Client

} // common
} // mega

