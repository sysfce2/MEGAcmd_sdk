/**
 * @file tree_filters.h
 * @brief Action packet Tree (t) filters
 *
 * (c) 2026 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#pragma once

#include "json.h"
#include "nodemanager.h"

namespace mega
{

class CommandPutNodes;
class MegaClient;

// Filters for action packet Tree (t)
class TreeFilters
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

    void execPreAction();
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