#pragma once

#include "json.h"

namespace mega
{

using FiltersType = std::map<std::string, JSONSplitter::FilterCallback>;

class MEGA_API ScStreamingParser
{
private:
    JSONSplitter jsonSplitter;
    FiltersType filters;
    bool last;

public:
    // For one action packet
    nameid actionName;
    bool isSelfOriginating;
    string seqTag;

    // For inter action packets
    std::unique_lock<recursive_mutex> nodeTreeIsChanging;
    bool originalAC;
    std::unique_ptr<CodeCounter::ScopeTimer> ccst;
    std::shared_ptr<Node> lastAPDeletedNode;

    ScStreamingParser();

    std::pair<FiltersType::iterator, bool> addFilter(std::string&& key,
                                                     JSONSplitter::FilterCallback&& filter);
    void removeFilter(std::string key);
    m_off_t process(const char* data);

    bool isInProgress();
    bool isFinished();
    bool isPaused();
    bool isFailed();

    // Check if all chunks are received
    bool isLastReceived();

    // Indicate that all chunks are received
    void setLastReceived();

    void clear();
    void clearActionPacketData();
};

}