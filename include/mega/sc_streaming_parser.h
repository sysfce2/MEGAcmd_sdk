#pragma once

#include "json.h"

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

    // Operate locks
    void acquireLock();
    void releaseLock();
};

}