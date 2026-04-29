/**
 * @file mega/megaclientprefs.h
 * @brief Private helpers for device-wide MegaClient preferences persistence
 */

#ifndef MEGA_MEGACLIENTPREFS_H
#define MEGA_MEGACLIENTPREFS_H 1

#include "mega/types.h"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>

namespace mega
{

class DbTable;
struct DbAccess;
struct FileSystemAccess;
class PrnGen;

struct PersistedTransferPreferences
{
    using ConnectionValue = uint8_t;

    void setConnection(const direction_t direction, const ConnectionValue value) noexcept
    {
        maxConnections[index(direction)] = value;
    }

    const std::optional<ConnectionValue>& connection(const direction_t direction) const noexcept
    {
        return maxConnections[index(direction)];
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return !connection(GET).has_value() && !connection(PUT).has_value();
    }

    std::array<std::optional<ConnectionValue>, 2> maxConnections{};

private:
    static constexpr std::size_t index(const direction_t direction) noexcept
    {
        return static_cast<std::size_t>(direction);
    }
};

class ClientPrefsStore final
{
public:
    ClientPrefsStore(PrnGen& rng, FileSystemAccess& fsAccess, DbAccess* dbaccess);
    ~ClientPrefsStore();

    [[nodiscard]] PersistedTransferPreferences loadTransferPreferences();
    [[nodiscard]] bool saveConnection(const direction_t direction, const uint8_t value);
    [[nodiscard]] bool saveConnections(const PersistedTransferPreferences& preferences);

private:
    void ensureTableOpen();

    PrnGen& mRng;
    FileSystemAccess& mFsAccess;
    DbAccess* mDbAccess{nullptr};
    std::unique_ptr<DbTable> mPrefsTable;
    bool mDbOpenFailed{false};
};

} // namespace mega

#endif // MEGA_MEGACLIENTPREFS_H
