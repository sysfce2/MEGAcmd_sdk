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
    std::map<std::string, JSONSplitter::FilterCallback> mFilters;
    std::optional<JSONSplitter::FiltersChain::iterator> mFiltersIt = std::nullopt;
    std::function<void()> mPreAction = nullptr;

    handle mOriginatingUser;

    const int mNotify = 1;

    // For node list
    bool mFirstNode;
    CommandPutNodes* mPutNodesCmd;
    bool mHasAnyNode;
    handle mPreviousHandleForAlert;
    NodeManager::MissingParentNodes mMissingParentNodes;
    bool mNodeError;
#ifdef ENABLE_SYNC
    set<NodeHandle> mAllParents;
#endif

    // For user list
    bool mUserError;

    void initFilters();
    void addFilters(JSONSplitter::FiltersChain& filtersChain);
    void removeFilters(JSONSplitter::FiltersChain& filtersChain);

    void execPreActionOnce();
    void getPutNodesCmdOnce();
    void readNode(JSON* json);
    void postReadNodes();
    void readUser(JSON* json);

    void clearData();
    void clearNodeListData();
    void clearUserListData();

public:
    TreeFilters(MegaClient& client);

    bool isStarted();

    void start(JSONSplitter::FiltersChain& filtersChain, std::function<void()> preAction);
    void end(JSONSplitter::FiltersChain& filtersChain);
    void clear(JSONSplitter::FiltersChain& filtersChain);
};

}