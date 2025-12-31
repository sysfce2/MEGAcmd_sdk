#include "mega/sc_streaming_parser.h"

#include "mega/megaclient.h"

namespace mega
{

ScStreamingParser::ScStreamingParser(MegaClient& client):
    mClient(client)
{
    clear();
}

void ScStreamingParser::init()
{
    // Start of parsing
    mFilters.emplace("",
                     [this](JSON*)
                     {
                         mOriginalAC = mClient.actionpacketsCurrent;
                         mClient.actionpacketsCurrent = false;

                         return JSONSplitter::CallbackResult::SUCCESS;
                     });

    // Parsing of chunk started
    mFilters.emplace("<",
                     [this](JSON*)
                     {
                         assert(!mNodeTreeIsChanging.owns_lock());
                         mNodeTreeIsChanging =
                             std::unique_lock<recursive_mutex>(mClient.nodeTreeMutex);

                         // Start timer
                         mCcst = std::make_unique<CodeCounter::ScopeTimer>(
                             mClient.performanceStats.scProcessingTime);

                         return JSONSplitter::CallbackResult::SUCCESS;
                     });

    // Action packets (one by one)

    mFilters.emplace("{[a{\"a",
                     [this](JSON* json)
                     {
                         mActionName = json->getnameidvalue();
                         return JSONSplitter::CallbackResult::SUCCESS;
                     });

    mFilters.emplace("{[a{\"i",
                     [this](JSON* json)
                     {
                         string jsonSid;
                         json->storeobject(&jsonSid);

                         mIsSelfOriginating =
                             !memcmp(jsonSid.data(), mClient.sessionid, sizeof(mClient.sessionid));

                         return JSONSplitter::CallbackResult::SUCCESS;
                     });

    mFilters.emplace("{[a{\"st",
                     [this](JSON* json)
                     {
                         json->storeobject(&mSeqTag);

                         if (!mClient.sc_checkSequenceTag(mSeqTag))
                         {
                             return JSONSplitter::CallbackResult::PAUSED;
                         }

                         return JSONSplitter::CallbackResult::SUCCESS;
                     });

    mFilters.emplace("{[a{",
                     [this](JSON* json)
                     {
                         // 'st' is not present
                         if (mSeqTag.empty())
                         {
                             const bool ret =
                                 mClient.sc_checkActionPacketWithoutSt(mActionName,
                                                                       mLastAPDeletedNode.get());
                             assert(ret);
                         }

                         if (mActionName == 0)
                         {
                             mLastAPDeletedNode.reset();
                             return JSONSplitter::ResultFromBool(json->leaveobject());
                         }

                         mClient.sc_procActionPacketWithoutCommonTags(*json,
                                                                      mActionName,
                                                                      mIsSelfOriginating,
                                                                      mLastAPDeletedNode);

                         return JSONSplitter::ResultFromBool(json->leaveobject());
                     });

    // End of action packets
    mFilters.emplace("{[a",
                     [this](JSON* json)
                     {
                         // No more Actions Packets. Force it to advance and process all
                         // the remaining command responses until a new "st" is found, if
                         // any. It will also process the latest command response
                         // associated (by the Sequence Tag) with the latest AP processed
                         // here.
                         mClient.sc_checkSequenceTag(string());

                         // This is intended to consume the '[' character if the array
                         // was empty and an empty array arrives here "[]".
                         // If the array was not empty, we receive here only the
                         // remaining character ']' and this call doesn't have any
                         // effect.
                         json->enterarray();

                         clearActionPacketData();

                         return JSONSplitter::ResultFromBool(json->leavearray());
                     });

    mFilters.emplace("{\"w",
                     [this](JSON* json)
                     {
                         return JSONSplitter::ResultFromBool(
                             json->storeobject(&mClient.scnotifyurl));
                     });

    mFilters.emplace("{\"ir",
                     [this](JSON* json)
                     {
                         // when spoonfeeding is in action, there may still be more
                         // actionpackets to be delivered.
                         mClient.insca_notlast = json->getint() == 1;
                         return JSONSplitter::CallbackResult::SUCCESS;
                     });

    mFilters.emplace("{\"sn",
                     [this](JSON* json)
                     {
                         mClient.sc_storeSn(*json);
                         return JSONSplitter::CallbackResult::SUCCESS;
                     });

    // Parsing of sc packet finished
    mFilters.emplace("{",
                     [&, this](JSON*)
                     {
                         mClient.sc_procEoo(mNodeTreeIsChanging, mOriginalAC);
                         return JSONSplitter::CallbackResult::SUCCESS;
                     });

    // Parsing of chunk finished
    mFilters.emplace(">",
                     [this](JSON*)
                     {
                         mCcst.reset();

                         if (mNodeTreeIsChanging.owns_lock())
                         {
                             mNodeTreeIsChanging.unlock();
                         }
                         return JSONSplitter::CallbackResult::SUCCESS;
                     });
}

m_off_t ScStreamingParser::process(const char* data)
{
    return mJsonSplitter.processChunk(&mFilters, data);
}

bool ScStreamingParser::hasStarted()
{
    return !mJsonSplitter.isStarting();
}

bool ScStreamingParser::isFinished()
{
    return mJsonSplitter.hasFinished();
}

bool ScStreamingParser::isFailed()
{
    return mJsonSplitter.hasFailed();
}

bool ScStreamingParser::isPaused()
{
    return mJsonSplitter.hasPaused();
}

bool ScStreamingParser::isLastReceived()
{
    return mLast;
}

void ScStreamingParser::setLastReceived()
{
    mLast = true;
}

void ScStreamingParser::clear()
{
    mJsonSplitter.clear();
    mLast = false;
    mLastAPDeletedNode = nullptr;

    clearActionPacketData();

    if (mCcst)
    {
        mCcst.reset();
    }
}

void ScStreamingParser::clearActionPacketData()
{
    mActionName = 0;
    mIsSelfOriginating = false;
    mSeqTag.clear();
}

} // namespace