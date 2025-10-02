#include <cassert>

#include <mega/common/badge.h>
#include <mega/common/query.h>
#include <mega/common/scoped_query.h>

namespace mega
{
namespace common
{

ScopedQuery::ScopedQuery()
  : mQuery(nullptr)
{
}

ScopedQuery::ScopedQuery(Badge<Transaction>, Query& query):
    mQuery(&query)
{
    query.clear();
}

ScopedQuery::ScopedQuery(ScopedQuery&& other)
  : mQuery(other.mQuery)
{
    other.mQuery = nullptr;
}

ScopedQuery::~ScopedQuery()
{
    if (mQuery)
        mQuery->reset();
}

ScopedQuery& ScopedQuery::operator=(ScopedQuery&& rhs)
{
    ScopedQuery temp(std::move(rhs));

    swap(temp);

    return *this;
}

ScopedQuery::operator bool() const
{
    assert(mQuery);

    return *mQuery;
}

ScopedQuery& ScopedQuery::operator++()
{
    assert(mQuery);

    ++*mQuery;

    return *this;
}

bool ScopedQuery::operator!() const
{
    assert(mQuery);

    return !*mQuery;
}

std::uint64_t ScopedQuery::changed() const
{
    assert(mQuery);

    return mQuery->changed();
}

void ScopedQuery::clear()
{
    assert(mQuery);

    mQuery->clear();
}

bool ScopedQuery::execute()
{
    assert(mQuery);

    return mQuery->execute();
}

Field ScopedQuery::field(const std::string& name)
{
    assert(mQuery);

    return mQuery->field(name);
}

Field ScopedQuery::field(const char* name)
{
    assert(mQuery);

    return mQuery->field(name);
}

std::uint64_t ScopedQuery::lastID() const
{
    assert(mQuery);

    return mQuery->lastID();
}

Parameter ScopedQuery::param(const std::string& name)
{
    assert(mQuery);

    return mQuery->param(name);
}

Parameter ScopedQuery::param(const char* name)
{
    assert(mQuery);

    return mQuery->param(name);
}

Query& ScopedQuery::query()
{
    assert(mQuery);

    return *mQuery;
}

void ScopedQuery::reset()
{
    assert(mQuery);

    mQuery->clear();
    mQuery->reset();
}

void ScopedQuery::swap(ScopedQuery& other)
{
    using std::swap;

    swap(mQuery, other.mQuery);
}

void swap(ScopedQuery& lhs, ScopedQuery& rhs)
{
    lhs.swap(rhs);
}

} // common
} // mega

