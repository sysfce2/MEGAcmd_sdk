#include "mega/sc_streaming_parser.h"

namespace mega
{

ScStreamingParser::ScStreamingParser()
{
    clear();
}

std::pair<FiltersType::iterator, bool>
    ScStreamingParser::addFilter(std::string&& key, JSONSplitter::FilterCallback&& filter)
{
    return filters.emplace(std::move(key), std::move(filter));
}

void ScStreamingParser::removeFilter(std::string key)
{
    filters.erase(key);
}

m_off_t ScStreamingParser::process(const char* data)
{
    return jsonSplitter.processChunk(&filters, data);
}

bool ScStreamingParser::isInProgress()
{
    return !jsonSplitter.isStarting();
}

bool ScStreamingParser::isFinished()
{
    return jsonSplitter.hasFinished();
}

bool ScStreamingParser::isFailed()
{
    return jsonSplitter.hasFailed();
}

bool ScStreamingParser::isPaused()
{
    return jsonSplitter.hasPaused();
}

bool ScStreamingParser::isLastReceived()
{
    return last;
}

void ScStreamingParser::setLastReceived()
{
    last = true;
}

void ScStreamingParser::clear()
{
    jsonSplitter.clear();
    last = false;
    lastAPDeletedNode = nullptr;

    clearActionPacketData();

    if (ccst)
    {
        ccst.reset();
    }
}

void ScStreamingParser::clearActionPacketData()
{
    actionName = 0;
    isSelfOriginating = false;
    seqTag.clear();
}

} // namespace