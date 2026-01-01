#pragma once

#include "json.h"
#include "nodemanager.h"

namespace mega
{

class CommandPutNodes;
class MegaClient;

// Class to process server-client packets in streaming using JSONSplitter.
class MEGA_API TreeFilters
{
private:
    MegaClient& mClient;
    std::function<void()> mPreAction;
    bool mStarted;

    handle mOriginatingUser;

    const int mNotify = 1;

    // For node list
    bool mFirstNode;
    CommandPutNodes* mPutNodesCmd;
    handle mPreviousHandleForAlert;
    NodeManager::MissingParentNodes mMissingParentNodes;
    bool mNodeError;
#ifdef ENABLE_SYNC
    set<NodeHandle> mAllParents;
#endif

    // For user list
    bool mUserError;

    void setFilters(std::map<std::string, JSONSplitter::FilterCallback>& filters);
    void clearFilters(std::map<std::string, JSONSplitter::FilterCallback>& filters);

    void readNode(JSON* json);
    void postReadNodes();
    void readUser(JSON* json);

    void clear();
    void clearNodeListData();
    void clearUserListData();

public:
    TreeFilters(MegaClient& client);

    bool isStarted();

    void start(std::map<std::string, JSONSplitter::FilterCallback>& filters,
               std::function<void()> preAction);
    void end(std::map<std::string, JSONSplitter::FilterCallback>& filters);
    void clear(std::map<std::string, JSONSplitter::FilterCallback>& filters);
};

}