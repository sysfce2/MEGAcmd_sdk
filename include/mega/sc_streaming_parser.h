/**
 * @file sc_streaming_parser.h
 * @brief Server-client packet streaming parser
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
#include "tree_filters.h"

namespace mega
{

class MegaClient;

// Class to process server-client packets in streaming using JSONSplitter.
class MEGA_API ScStreamingParser
{
private:
    MegaClient& mClient;
    JSONSplitter mJsonSplitter;
    std::map<std::string, JSONSplitter::FilterCallback> mFilters;
    bool mLast;

    // For one action packet
    nameid mActionName;
    bool mIsSelfOriginating;
    string mSeqTag;

    // For inter action packets
    std::unique_lock<recursive_mutex> mNodeTreeIsChanging;
    bool mOriginalAC;
    std::unique_ptr<CodeCounter::ScopeTimer> mCcst;
    std::shared_ptr<Node> mLastAPDeletedNode;

    TreeFilters mTreeFilters;

public:
    ScStreamingParser(MegaClient& client);

    void init();
    m_off_t process(const char* data);
    bool hasStarted();
    bool isFinished();
    bool isPaused();
    bool isFailed();

    // Check if all chunks are received
    bool isLastReceived();

    // Indicate that all chunks are received
    void setLastReceived();

    void clear();
    void clearActionPacketData();

private:
    // Operate locks
    void acquireLock();
    void releaseLock();

    void checkActionPacket();
};

}