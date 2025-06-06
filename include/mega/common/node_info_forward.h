#pragma once

#include <list>
#include <memory>
#include <vector>

namespace mega
{
namespace common
{

struct NodeInfo;

using NodeInfoList = std::list<NodeInfo>;
using NodeInfoPtr = std::unique_ptr<NodeInfo>;
using NodeInfoVector = std::vector<NodeInfo>;

} // common
} // mega

