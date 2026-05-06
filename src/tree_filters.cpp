/**
 * @file tree_filters.cpp
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

#include "mega/tree_filters.h"

#include "mega/megaclient.h"

namespace mega
{

TreeFilters::TreeFilters(MegaClient& client):
    mClient(client)
{
    clearData();
    initFilters();
}

bool TreeFilters::isStarted()
{
    return mFiltersIt.has_value();
}

void TreeFilters::start(JSONSplitter::FiltersChain& filtersChain, std::function<void()> preAction)
{
    mPreAction = std::move(preAction);

    addFilters(filtersChain);

    if (!mClient.loggedIntoFolder())
    {
        mClient.useralerts.beginNotingSharedNodes();
    }
}

void TreeFilters::end(JSONSplitter::FiltersChain& filtersChain)
{
    mClient.mergenewshares(1);

    if (!mClient.loggedIntoFolder())
    {
        mClient.useralerts.convertNotedSharedNodes(true, mOriginatingUser);
    }

    clear(filtersChain);
}

void TreeFilters::clear(JSONSplitter::FiltersChain& filtersChain)
{
    removeFilters(filtersChain);
    clearData();
}

// Private

void TreeFilters::initFilters()
{
    // Nodes (one by one)
    mFilters.emplace("{[a{{t[f{",
                     [this](JSON* json)
                     {
                         execPreActionOnce();
                         getPutNodesCmdOnce();

                         mHasAnyNode = true;

                         readNode(json);
                         return JSONSplitter::ResultFromBool(json->leaveobject());
                     });

    // End of node array
    mFilters.emplace("{[a{{t[f",
                     [this](JSON* json)
                     {
                         execPreActionOnce();
                         getPutNodesCmdOnce();

                         if (*mPutNodesCmd)
                         {
                             (*mPutNodesCmd)->emptyResponse = !mHasAnyNode;
                         }

                         postReadNodes();

                         json->enterarray();
                         return JSONSplitter::ResultFromBool(json->leavearray());
                     });

    // Version nodes (one by one)
    mFilters.emplace("{[a{{t[f2{",
                     [this](JSON* json)
                     {
                         execPreActionOnce();
                         getPutNodesCmdOnce();

                         readNode(json);
                         return JSONSplitter::ResultFromBool(json->leaveobject());
                     });

    // End of version nodes array
    mFilters.emplace("{[a{{t[f2",
                     [this](JSON* json)
                     {
                         execPreActionOnce();
                         postReadNodes();

                         json->enterarray();
                         return JSONSplitter::ResultFromBool(json->leavearray());
                     });

    // Users (one by one)
    mFilters.emplace("{[a{{t[u{",
                     [this](JSON* json)
                     {
                         execPreActionOnce();
                         readUser(json);
                         return JSONSplitter::ResultFromBool(json->leaveobject());
                     });

    // End of user array
    mFilters.emplace("{[a{{t[u",
                     [this](JSON* json)
                     {
                         execPreActionOnce();
                         clearUserListData();

                         json->enterarray();
                         return JSONSplitter::ResultFromBool(json->leavearray());
                     });

    // End of tree object
    mFilters.emplace("{[a{{t",
                     [](JSON* json)
                     {
                         return JSONSplitter::ResultFromBool(json->leaveobject());
                     });

    mFilters.emplace("{[a{\"ou",
                     [this](JSON* json)
                     {
                         execPreActionOnce();
                         mOriginatingUser = json->gethandle(MegaClient::USERHANDLE);
                         return JSONSplitter::CallbackResult::SUCCESS;
                     });
}

void TreeFilters::addFilters(JSONSplitter::FiltersChain& filtersChain)
{
    mFiltersIt = filtersChain.emplace(std::end(filtersChain), &mFilters);
}

void TreeFilters::removeFilters(JSONSplitter::FiltersChain& filtersChain)
{
    if (mFiltersIt.has_value())
    {
        filtersChain.erase(*mFiltersIt);
        mFiltersIt = std::nullopt;
    }
}

void TreeFilters::execPreActionOnce()
{
    if (mPreAction)
    {
        mPreAction();
        mPreAction = nullptr;
    }
}

void TreeFilters::getPutNodesCmdOnce()
{
    if (!mPutNodesCmd.has_value())
    {
        mPutNodesCmd = dynamic_cast<CommandPutNodes*>(
            mClient.reqs.getCurrentCommand(mClient.mCurrentSeqtagSeen));
    }
}

void TreeFilters::readNode(JSON* json)
{
    if (mNodeError)
    {
        return;
    }

    int e = mClient.readnode(json,
                             mNotify,
                             (*mPutNodesCmd) ? (*mPutNodesCmd)->source : PUTNODES_APP,
                             (*mPutNodesCmd) ? &(*mPutNodesCmd)->nn : nullptr,
                             (*mPutNodesCmd) ? (*mPutNodesCmd)->tag != 0 : false,
                             (*mPutNodesCmd) ? true : false,
                             mMissingParentNodes,
                             mPreviousHandleForAlert,
#ifdef ENABLE_SYNC
                             &mAllParents);
#else
                             nullptr);
#endif

    if (e != 1)
    {
        LOG_err << "Parsing error in readnodes: " << e;
        mNodeError = true;
    }
}

void TreeFilters::postReadNodes()
{
#ifdef ENABLE_SYNC
    mClient.postReadNodes(mNotify, mMissingParentNodes, &mAllParents);
#else
    mClient.postReadNodes(mNotify, mMissingParentNodes);
#endif

    clearNodeListData();
}

void TreeFilters::readUser(JSON* json)
{
    if (mUserError)
    {
        return;
    }

    int e = mClient.readuser(json, true);

    if (e != 1)
    {
        LOG_err << "Parsing error in readusers: " << e;
        mUserError = true;
    }
}

void TreeFilters::clearData()
{
    mPreAction = nullptr;
    mOriginatingUser = UNDEF;

    clearNodeListData();
    clearUserListData();
}

void TreeFilters::clearNodeListData()
{
    mPutNodesCmd.reset();
    mHasAnyNode = false;
    mPreviousHandleForAlert = UNDEF;
    mMissingParentNodes.clear();
    mNodeError = false;

#ifdef ENABLE_SYNC
    mAllParents.clear();
#endif
}

void TreeFilters::clearUserListData()
{
    mUserError = false;
}

} // namespace
