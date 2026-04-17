/**
 * @file megaclientprefs.cpp
 * @brief Private helpers for device-wide MegaClient preferences persistence
 */

#include "mega/megaclientprefs.h"

#include "mega/db.h"
#include "mega/megaclient.h"
#include "mega/utils.h"

#include <array>

namespace mega
{
namespace
{
enum class PrefsRecordKey : uint32_t
{
    // Avoid the low bits used by MegaClient::CACHEDNODE (1), since
    // SqliteDbTable::put asserts against that masked value for statecache rows.
    TRANSFER_PREFERENCES = 2
};

constexpr uint32_t transferPreferencesKey()
{
    return static_cast<uint32_t>(PrefsRecordKey::TRANSFER_PREFERENCES);
}

constexpr std::array<direction_t, 2> kTransferDirections = {GET, PUT};

constexpr std::size_t directionIndex(const direction_t direction)
{
    return static_cast<std::size_t>(direction);
}

constexpr uint8_t directionBit(const direction_t direction)
{
    return static_cast<uint8_t>(1u << static_cast<unsigned>(direction));
}

constexpr uint8_t kKnownTransferDirectionBits = directionBit(GET) | directionBit(PUT);
constexpr std::size_t kTransferPreferencesBlobSize = 1 + kTransferDirections.size();

bool isValidConnectionValue(const PersistedTransferPreferences::ConnectionValue value)
{
    return value >= 1 && value <= MegaClient::MAX_NUM_CONNECTIONS;
}

uint8_t presenceMask(const PersistedTransferPreferences& preferences)
{
    uint8_t mask = 0;

    for (const auto direction: kTransferDirections)
    {
        if (preferences.connection(direction))
        {
            mask = static_cast<uint8_t>(mask | directionBit(direction));
        }
    }

    return mask;
}

bool parseTransferPreferencesBlob(const string& blob, PersistedTransferPreferences& preferences)
{
    if (blob.size() != kTransferPreferencesBlobSize)
    {
        return false;
    }

    CacheableReader reader(blob);

    byte mask = 0;
    std::array<byte, kTransferDirections.size()> values{};
    if (!reader.unserializebyte(mask))
    {
        return false;
    }

    for (auto& value: values)
    {
        if (!reader.unserializebyte(value))
        {
            return false;
        }
    }

    if (!mask || (mask & ~kKnownTransferDirectionBits) != 0)
    {
        return false;
    }

    PersistedTransferPreferences parsedPreferences;
    for (const auto direction: kTransferDirections)
    {
        if ((mask & directionBit(direction)) == 0)
        {
            continue;
        }

        const auto value = static_cast<PersistedTransferPreferences::ConnectionValue>(
            values[directionIndex(direction)]);
        if (!isValidConnectionValue(value))
        {
            return false;
        }

        parsedPreferences.setConnection(direction, value);
    }

    preferences = parsedPreferences;
    return true;
}

string serializeTransferPreferencesBlob(const PersistedTransferPreferences& preferences)
{
    const auto mask = presenceMask(preferences);
    assert(mask != 0);

    string blob;
    blob.reserve(kTransferPreferencesBlobSize);

    CacheableWriter writer(blob);
    writer.serializebyte(static_cast<byte>(mask));

    for (const auto direction: kTransferDirections)
    {
        const auto& value = preferences.connection(direction);
        writer.serializebyte(static_cast<byte>(value.value_or(0)));
    }

    assert(blob.size() == kTransferPreferencesBlobSize);
    return blob;
}

void logInvalidTransferPreferencesPayload(const string& blob, const char* const action)
{
    if (blob.size() == kTransferPreferencesBlobSize)
    {
        LOG_warn << "[prefs DB] transfer preferences payload invalid; " << action << " row";
        return;
    }

    LOG_warn << "[prefs DB] transfer preferences payload has unexpected size " << blob.size()
             << "; " << action << " row";
}
} // namespace

ClientPrefsStore::ClientPrefsStore(PrnGen& rng, FileSystemAccess& fsAccess, DbAccess* dbaccess):
    mRng(rng),
    mFsAccess(fsAccess),
    mDbAccess(dbaccess)
{}

ClientPrefsStore::~ClientPrefsStore() = default;

PersistedTransferPreferences ClientPrefsStore::loadTransferPreferences()
{
    ensureTableOpen();

    PersistedTransferPreferences preferences;
    if (!mPrefsTable)
    {
        return preferences;
    }

    string blob;
    if (!mPrefsTable->get(transferPreferencesKey(), &blob))
    {
        return preferences;
    }

    if (!parseTransferPreferencesBlob(blob, preferences))
    {
        logInvalidTransferPreferencesPayload(blob, "deleting");
        DBTableTransactionCommitter committer(mPrefsTable);
        mPrefsTable->del(transferPreferencesKey());
        return {};
    }

    for (const auto direction: kTransferDirections)
    {
        if (const auto& value = preferences.connection(direction))
        {
            LOG_debug << "[prefs DB] loaded connections[" << connDirectionToStr(direction)
                      << "] = " << static_cast<unsigned>(*value);
        }
    }

    return preferences;
}

bool ClientPrefsStore::saveConnection(const direction_t direction, const uint8_t value)
{
    ensureTableOpen();

    if (!mPrefsTable)
    {
        return false;
    }

    PersistedTransferPreferences preferences;
    string blob;
    if (mPrefsTable->get(transferPreferencesKey(), &blob) &&
        !parseTransferPreferencesBlob(blob, preferences))
    {
        logInvalidTransferPreferencesPayload(blob, "overwriting");
    }

    preferences.setConnection(direction, value);
    return saveConnections(preferences);
}

bool ClientPrefsStore::saveConnections(const PersistedTransferPreferences& preferences)
{
    ensureTableOpen();

    if (!mPrefsTable)
    {
        return false;
    }

    auto serialized = serializeTransferPreferencesBlob(preferences);

    DBTableTransactionCommitter committer(mPrefsTable);
    return mPrefsTable->put(transferPreferencesKey(), &serialized);
}

void ClientPrefsStore::ensureTableOpen()
{
    if (!mDbAccess || mPrefsTable || mDbOpenFailed)
    {
        return;
    }

    // DB_OPEN_FLAG_RECYCLE preserves the prefs DB across a one-step DB version
    // bump, matching the other statecache-style tables.
    mPrefsTable.reset(mDbAccess->open(mRng,
                                      mFsAccess,
                                      "prefs",
                                      DB_OPEN_FLAG_RECYCLE,
                                      [](DBError error)
                                      {
                                          LOG_warn << "[prefs DB] error " << static_cast<int>(error)
                                                   << " - persistence will be disabled";
                                      }));

    if (!mPrefsTable)
    {
        LOG_warn << "[prefs DB] open failed; persistence disabled until process restart";
        mDbOpenFailed = true;
    }
}

} // namespace mega
