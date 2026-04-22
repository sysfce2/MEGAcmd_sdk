#ifndef OSXUTILS_H
#define OSXUTILS_H

#include "mega/proxy.h"

#include <cstdint>
#include <optional>

void path2localMac(const std::string* path, std::string* local);

#if defined(__APPLE__) && !(TARGET_OS_IPHONE)
void getOSXproxy(mega::Proxy* proxy);
#endif

#if TARGET_OS_IPHONE
// Queries Foundation's NSURLVolumeAvailableCapacityForImportantUsageKey for
// the volume containing `path`. Returns the number of bytes available for
// user-requested ("important") usage, accounting for purgeable storage the
// system can reclaim. Returns std::nullopt on failure so the caller can fall
// back to its default (statfs) path.
std::optional<std::int64_t> availableDiskSpaceForImportantUsage(const std::string& path);
#endif

#endif // OSXUTILS_H
