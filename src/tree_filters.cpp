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
    clear();
}

bool TreeFilters::isStarted()
{
    return mStarted;
}

void TreeFilters::start(std::map<std::string, JSONSplitter::FilterCallback>& filters,
                        std::function<void()> preAction)
{
    mStarted = true;

    mPreAction = std::move(preAction);

    setFilters(filters);

    if (!mClient.loggedIntoFolder())
    {
        mClient.useralerts.beginNotingSharedNodes();
    }
}

void TreeFilters::end(std::map<std::string, JSONSplitter::FilterCallback>& filters)
{
    mClient.mergenewshares(1);

    if (!mClient.loggedIntoFolder())
    {
        mClient.useralerts.convertNotedSharedNodes(true, mOriginatingUser);
    }

    clear(filters);

    mStarted = false;
}

void TreeFilters::clear(std::map<std::string, JSONSplitter::FilterCallback>& filters)
{
    clearFilters(filters);
    clear();
}

// Private

void TreeFilters::setFilters(std::map<std::string, JSONSplitter::FilterCallback>& filters)
{
    // Nodes (one by one)
    filters.emplace("{[a{{t[f{",
                    [this](JSON* json)
                    {
                        execPreAction();

                        if (mFirstNode)
                        {
                            mFirstNode = false;
                            mPutNodesCmd = dynamic_cast<CommandPutNodes*>(
                                mClient.reqs.getCurrentCommand(mClient.mCurrentSeqtagSeen));

                            if (mPutNodesCmd)
                            {
                                mPutNodesCmd->emptyResponse = false;
                            }
                        }

                        readNode(json);
                        return JSONSplitter::ResultFromBool(json->leaveobject());
                    });

    // End of node array
    filters.emplace("{[a{{t[f",
                    [this](JSON* json)
                    {
                        execPreAction();

                        if (mFirstNode && mPutNodesCmd)
                        {
                            // 'f' is empty
                            mPutNodesCmd->emptyResponse = true;
                        }

                        postReadNodes();

                        json->enterarray();
                        return JSONSplitter::ResultFromBool(json->leavearray());
                    });

    // Version nodes (one by one)
    filters.emplace("{[a{{t[f2{",
                    [this](JSON* json)
                    {
                        execPreAction();

                        if (mFirstNode)
                        {
                            mFirstNode = false;
                            mPutNodesCmd = dynamic_cast<CommandPutNodes*>(
                                mClient.reqs.getCurrentCommand(mClient.mCurrentSeqtagSeen));
                        }

                        readNode(json);
                        return JSONSplitter::ResultFromBool(json->leaveobject());
                    });

    // End of version nodes array
    filters.emplace("{[a{{t[f2",
                    [this](JSON* json)
                    {
                        execPreAction();
                        postReadNodes();

                        json->enterarray();
                        return JSONSplitter::ResultFromBool(json->leavearray());
                    });

    // Users (one by one)
    filters.emplace("{[a{{t[u{",
                    [this](JSON* json)
                    {
                        execPreAction();
                        readUser(json);
                        return JSONSplitter::ResultFromBool(json->leaveobject());
                    });

    // End of user array
    filters.emplace("{[a{{t[u",
                    [this](JSON* json)
                    {
                        execPreAction();
                        clearUserListData();

                        json->enterarray();
                        return JSONSplitter::ResultFromBool(json->leavearray());
                    });

    filters.emplace("{[a{\"ou",
                    [this](JSON* json)
                    {
                        execPreAction();
                        mOriginatingUser = json->gethandle(MegaClient::USERHANDLE);
                        return JSONSplitter::CallbackResult::SUCCESS;
                    });
}

void TreeFilters::clearFilters(std::map<std::string, JSONSplitter::FilterCallback>& filters)
{
    filters.erase("{[a{{t[f{");
    filters.erase("{[a{{t[f");
    filters.erase("{[a{{t[f2{");
    filters.erase("{[a{{t[f2");
    filters.erase("{[a{{t[u{");
    filters.erase("{[a{{t[u");
    filters.erase("{[a{\"ou");
}

void TreeFilters::execPreAction()
{
    if (mPreAction)
    {
        mPreAction();
        mPreAction = nullptr;
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
                             mPutNodesCmd ? mPutNodesCmd->source : PUTNODES_APP,
                             mPutNodesCmd ? &mPutNodesCmd->nn : nullptr,
                             mPutNodesCmd ? mPutNodesCmd->tag != 0 : false,
                             mPutNodesCmd ? true : false,
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

void TreeFilters::clear()
{
    mPreAction = nullptr;
    mOriginatingUser = UNDEF;

    clearNodeListData();
    clearUserListData();
}

void TreeFilters::clearNodeListData()
{
    mFirstNode = true;
    mPutNodesCmd = nullptr;
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