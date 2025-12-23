#include "mega/sc_streaming_parser.h"

#include "mega/megaclient.h"

namespace mega
{

ScStreamingParser::ScStreamingParser(MegaClient* client):
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
                         mOriginalAC = mClient->actionpacketsCurrent;
                         mClient->actionpacketsCurrent = false;

                         // Start timer
                         mCcst = std::make_unique<CodeCounter::ScopeTimer>(
                             mClient->performanceStats.scProcessingTime);

                         return JSONSplitter::CallbackResult::SUCCESS;
                     });

    // Parsing of chunk started
    mFilters.emplace("<",
                     [this](JSON*)
                     {
                         assert(!mNodeTreeIsChanging.owns_lock());
                         mNodeTreeIsChanging =
                             std::unique_lock<recursive_mutex>(mClient->nodeTreeMutex);
                         return JSONSplitter::CallbackResult::SUCCESS;
                     });

    // Parsing of chunk finished
    mFilters.emplace(">",
                     [this](JSON*)
                     {
                         if (mNodeTreeIsChanging.owns_lock())
                         {
                             mNodeTreeIsChanging.unlock();
                         }
                         return JSONSplitter::CallbackResult::SUCCESS;
                     });

    mFilters.emplace("{\"w",
                     [this](JSON* json)
                     {
                         return JSONSplitter::ResultFromBool(
                             json->storeobject(&mClient->scnotifyurl));
                     });

    mFilters.emplace("{\"ir",
                     [this](JSON* json)
                     {
                         // when spoonfeeding is in action, there may still be more
                         // actionpackets to be delivered.
                         mClient->insca_notlast = json->getint() == 1;
                         return JSONSplitter::CallbackResult::SUCCESS;
                     });

    mFilters.emplace("{\"sn",
                     [this](JSON* json)
                     {
                         mClient->sc_storeSn(*json);
                         return JSONSplitter::CallbackResult::SUCCESS;
                     });

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

                         mIsSelfOriginating = std::string_view(mClient->sessionid) == jsonSid;

                         return JSONSplitter::CallbackResult::SUCCESS;
                     });

    mFilters.emplace("{[a{\"st",
                     [this](JSON* json)
                     {
                         json->storeobject(&mSeqTag);

                         if (!mClient->sc_checkSequenceTag(mSeqTag))
                         {
                             return JSONSplitter::CallbackResult::PAUSED;
                         }

                         return JSONSplitter::CallbackResult::SUCCESS;
                     });

    // Action packets (one by one)
    mFilters.emplace("{[a{",
                     [this](JSON* json)
                     {
                         // 'st' is not present
                         if (mSeqTag.empty())
                         {
                             mClient->sc_checkActionPacketWithoutSt(mActionName,
                                                                    mLastAPDeletedNode.get());
                         }

                         if (mActionName == 0)
                         {
                             mLastAPDeletedNode.reset();
                             return JSONSplitter::CallbackResult::SUCCESS;
                         }

                         mClient->sc_procActionPacketWithoutCommonTags(*json,
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
                         mClient->sc_checkSequenceTag(string());

                         // This is intended to consume the '[' character if the array
                         // was empty and an empty array arrives here "[]".
                         // If the array was not empty, we receive here only the
                         // remaining character ']' and this call doesn't have any
                         // effect.
                         json->enterarray();

                         clearActionPacketData();

                         return JSONSplitter::ResultFromBool(json->leavearray());
                     });

    // Parsing finished
    mFilters.emplace("{",
                     [&, this](JSON*)
                     {
                         mClient->sc_procEoo(mNodeTreeIsChanging, mOriginalAC);
                         mCcst->complete();
                         return JSONSplitter::CallbackResult::SUCCESS;
                     });
}

m_off_t ScStreamingParser::process(const char* data)
{
    return mJsonSplitter.processChunk(&mFilters, data);
}

bool ScStreamingParser::isInProgress()
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