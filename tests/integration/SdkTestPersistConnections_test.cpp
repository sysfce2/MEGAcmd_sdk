/**
 * @file SdkTestPersistConnections_test.cpp
 * @brief Integration tests for the device-wide prefs DB that persists
 *        connections[PUT] / connections[GET] across MegaApi restarts.
 *
 * Covers:
 *   - ClientPrefsStore::loadTransferPreferences / saveConnection / saveConnections
 *   - ClientPrefsStore open retry-latch and non-fatal error policy
 *   - Folder-link sessions inherit the device-wide value
 *   - Corrupt / out-of-range DB payloads fall back to platform defaults
 *
 * SdkTestBase::SetUp removes prefs DB files before each integration test so
 * each test starts from platform defaults. The tests below still do explicit
 * per-base-path wipes where they need a local reset inside the test body
 * before recreating MegaApi on the same base path.
 */

#include "../stdfs.h"
#include "integration_test_utils.h"
#include "SdkTest_test.h"

#include <mega/db.h>

#include <array>
#include <fstream>
#include <sqlite3.h>
#include <string>

// megaApiCacheFolder is defined at file scope in SdkTest_test.cpp (external
// linkage). Declared here so we don't duplicate the per-process cache-folder
// layout logic.
extern std::string megaApiCacheFolder(int index);

namespace
{
constexpr uint32_t kPrefsCombinedConnectionsRowId = 2;
constexpr unsigned char kPrefsConnectionMaskPut = static_cast<unsigned char>(1u << PUT);

// Mirror of SqliteDbAccess::databasePath(name="prefs", version=DB_VERSION) for
// the OutOfRangePersistedValueIgnored test, which opens the DB directly via
// sqlite3_open to inject an out-of-range value the public API can never write.
fs::path prefsDbPath(const std::string& basePath)
{
    return fs::path{basePath} /
           ("megaclient_statecache" + std::to_string(::mega::DbAccess::DB_VERSION) + "_prefs.db");
}

// Local MegaApi construction helper — the anonymous-namespace newMegaApi() in
// SdkTest_test.cpp has internal linkage and cannot be reached from this TU.
// The body here is deliberately a small copy of that function so the fixture
// stays self-contained and we don't have to expose newMegaApi globally.
MegaApiTestPointer makeMegaApi(const std::string& appKey,
                               const std::string& basePath,
                               const std::string& userAgent,
                               unsigned workerThreadCount)
{
#ifdef ENABLE_ISOLATED_GFX
    static std::atomic_int endpointCounter{0};
    std::ostringstream oss;
    oss << "test_integration_persist_" << ::mega::getCurrentPid() << "_" << endpointCounter++;
    const std::string endpointName = oss.str();

    const auto gfxworkerPath = sdk_test::getTestDataDir() /
#ifdef _WIN32
                               (std::string("gfxworker") + ".exe");
#else
                               std::string("gfxworker");
#endif
    std::unique_ptr<MegaGfxProvider> provider{
        MegaGfxProvider::createIsolatedInstance(endpointName.c_str(),
                                                gfxworkerPath.string().c_str())};
    return MegaApiTestPointer{new MegaApiTest(appKey.c_str(),
                                              provider.get(),
                                              basePath.c_str(),
                                              userAgent.c_str(),
                                              workerThreadCount),
                              MegaApiTestDeleter{endpointName}};
#else
    return MegaApiTestPointer{
        new MegaApiTest(appKey.c_str(), basePath.c_str(), userAgent.c_str(), workerThreadCount),
        MegaApiTestDeleter{""}};
#endif
}

} // namespace

class SdkTestPersistConnections: public SdkTest
{
protected:
    void recreateMegaApi(const std::string& basePath)
    {
        if (megaApi.empty())
        {
            megaApi.resize(1);
            mApi.resize(1);
        }

        megaApi[0] = makeMegaApi(APP_KEY, basePath, USER_AGENT, unsigned(THREADS_PER_MEGACLIENT));
        mApi[0].megaApi = megaApi[0].get();
        megaApi[0]->addListener(this);
    }

    void expectMaxConnections(const TransferMaxConnections& expected)
    {
        TransferMaxConnections current;
        ASSERT_EQ(API_OK, doGetMaxConnections(0, current));
        EXPECT_EQ(current.upload, expected.upload);
        EXPECT_EQ(current.download, expected.download);
    }

    static TransferMaxConnections defaultMaxConnections()
    {
#if defined(__ANDROID__) || defined(USE_IOS)
        return {3, 4};
#else
        return {8, 8};
#endif
    }
};

/**
 * @brief TEST_F PersistAcrossApiRestart_LoggedIn
 *
 * setMaxConnections() values set while logged in must survive a MegaApi
 * restart on the same base path.
 */
TEST_F(SdkTestPersistConnections, PersistAcrossApiRestart_LoggedIn)
{
    LOG_info << "___TEST PersistAcrossApiRestart_LoggedIn___";

    const std::string basePath = megaApiCacheFolder(0);
    wipePersistedConnectionsDbFilesUnder(basePath);

    ASSERT_NO_FATAL_FAILURE(getAccountsForTest(1));

    constexpr int desiredPut = 5;
    constexpr int desiredGet = 7;
    ASSERT_EQ(API_OK, doSetMaxConnections(0, MegaTransfer::TYPE_UPLOAD, desiredPut));
    ASSERT_EQ(API_OK, doSetMaxConnections(0, MegaTransfer::TYPE_DOWNLOAD, desiredGet));

    releaseMegaApi(0);

    recreateMegaApi(basePath);
    expectMaxConnections({desiredPut, desiredGet});
}

/**
 * @brief TEST_F PersistAcrossApiRestart_GlobalSetter
 *
 * The global MegaApi::setMaxConnections(int) overload must persist one value
 * that restores into both upload and download directions after restart.
 */
TEST_F(SdkTestPersistConnections, PersistAcrossApiRestart_GlobalSetter)
{
    LOG_info << "___TEST PersistAcrossApiRestart_GlobalSetter___";

    const std::string basePath = megaApiCacheFolder(0);
    wipePersistedConnectionsDbFilesUnder(basePath);

    constexpr int desiredConnections = 6;
    recreateMegaApi(basePath);
    ASSERT_EQ(API_OK, doSetMaxConnections(0, desiredConnections));

    releaseMegaApi(0);

    recreateMegaApi(basePath);
    expectMaxConnections({desiredConnections, desiredConnections});
}

/**
 * @brief TEST_F SetMaxConnectionsReturnsEwriteWhenPrefsDbCannotOpen
 *
 * When the prefs DB cannot be opened, the public setter must still update the
 * live value(s) for the current MegaApi instance but finish with API_EWRITE,
 * and nothing should persist across restart.
 */
TEST_F(SdkTestPersistConnections, SetMaxConnectionsReturnsEwriteWhenPrefsDbCannotOpen)
{
    LOG_info << "___TEST SetMaxConnectionsReturnsEwriteWhenPrefsDbCannotOpen___";

    const std::string basePath = megaApiCacheFolder(0);
    wipePersistedConnectionsDbFilesUnder(basePath);

    std::error_code ec;
    fs::create_directories(basePath, ec);
    ASSERT_FALSE(ec) << "could not create base path " << basePath << ": " << ec.message();

    const fs::path dbPath = prefsDbPath(basePath);
    fs::create_directory(dbPath, ec);
    ASSERT_FALSE(ec) << "could not occupy prefs DB path " << dbPath << ": " << ec.message();

    recreateMegaApi(basePath);

    constexpr int desiredConnections = 6;
    ASSERT_EQ(API_EWRITE, doSetMaxConnections(0, desiredConnections));
    expectMaxConnections({desiredConnections, desiredConnections});

    constexpr int desiredPut = 9;
    ASSERT_EQ(API_EWRITE, doSetMaxConnections(0, MegaTransfer::TYPE_UPLOAD, desiredPut));
    expectMaxConnections({desiredPut, desiredConnections});

    releaseMegaApi(0);

    fs::remove_all(dbPath, ec);
    ASSERT_FALSE(ec) << "could not remove occupied prefs DB path " << dbPath << ": "
                     << ec.message();

    recreateMegaApi(basePath);
    expectMaxConnections(defaultMaxConnections());
}

/**
 * @brief TEST_F PersistSingleDirectionDoesNotPersistOtherDirection
 *
 * The single prefsTable row stores both directions, but persisting one
 * direction must not implicitly persist the other one. Unset direction(s)
 * must still fall back to platform defaults after restart.
 */
TEST_F(SdkTestPersistConnections, PersistSingleDirectionDoesNotPersistOtherDirection)
{
    LOG_info << "___TEST PersistSingleDirectionDoesNotPersistOtherDirection___";

    const std::string basePath = megaApiCacheFolder(0);
    const auto defaults = defaultMaxConnections();

    wipePersistedConnectionsDbFilesUnder(basePath);
    recreateMegaApi(basePath);

    constexpr int desiredPut = 5;
    ASSERT_EQ(API_OK, doSetMaxConnections(0, MegaTransfer::TYPE_UPLOAD, desiredPut));
    releaseMegaApi(0);

    recreateMegaApi(basePath);
    expectMaxConnections({desiredPut, defaults.download});

    releaseMegaApi(0);
    wipePersistedConnectionsDbFilesUnder(basePath);

    recreateMegaApi(basePath);

    constexpr int desiredGet = 11;
    ASSERT_EQ(API_OK, doSetMaxConnections(0, MegaTransfer::TYPE_DOWNLOAD, desiredGet));
    releaseMegaApi(0);

    recreateMegaApi(basePath);
    expectMaxConnections({defaults.upload, desiredGet});
}

/**
 * @brief TEST_F PersistAppliesToFolderLink
 *
 * Load-bearing test: values set while logged in must apply to a subsequent
 * folder-link session on the same base path (no user logged in).
 */
TEST_F(SdkTestPersistConnections, PersistAppliesToFolderLink)
{
    LOG_info << "___TEST PersistAppliesToFolderLink___";

    const std::string basePath = megaApiCacheFolder(0);
    wipePersistedConnectionsDbFilesUnder(basePath);

    ASSERT_NO_FATAL_FAILURE(getAccountsForTest(1));

    std::unique_ptr<MegaNode> rootnode{megaApi[0]->getRootNode()};
    ASSERT_TRUE(rootnode);
    const std::string folderName = "PersistFolderLink_" + std::to_string(m_time());
    const MegaHandle folderHandle = createFolder(0, folderName.c_str(), rootnode.get());
    ASSERT_NE(folderHandle, UNDEF);
    std::unique_ptr<MegaNode> folderNode{megaApi[0]->getNodeByHandle(folderHandle)};
    ASSERT_TRUE(folderNode);

    RequestTracker accountDetailsTracker{megaApi[0].get()};
    megaApi[0]->getSpecificAccountDetails(false, false, true, -1, &accountDetailsTracker);
    ASSERT_EQ(API_OK, accountDetailsTracker.waitForResult());
    const bool isFreeAccount =
        mApi[0].accountDetails->getProLevel() == MegaAccountDetails::ACCOUNT_TYPE_FREE;
    const std::string nodeLink =
        createPublicLink(0, folderNode.get(), 0, maxTimeout, isFreeAccount);
    ASSERT_FALSE(nodeLink.empty());

    constexpr int desiredPut = 9;
    constexpr int desiredGet = 11;
    ASSERT_EQ(API_OK, doSetMaxConnections(0, MegaTransfer::TYPE_UPLOAD, desiredPut));
    ASSERT_EQ(API_OK, doSetMaxConnections(0, MegaTransfer::TYPE_DOWNLOAD, desiredGet));

    releaseMegaApi(0);

    recreateMegaApi(basePath);

    RequestTracker loginToFolderTracker{megaApi[0].get()};
    megaApi[0]->loginToFolder(nodeLink.c_str(), &loginToFolderTracker);
    ASSERT_EQ(API_OK, loginToFolderTracker.waitForResult());

    expectMaxConnections({desiredPut, desiredGet});
}

/**
 * @brief TEST_F FirstRunUsesDefaults
 *
 * A pristine base path must produce the platform default values, not whatever
 * some previous test happened to persist.
 */
TEST_F(SdkTestPersistConnections, FirstRunUsesDefaults)
{
    LOG_info << "___TEST FirstRunUsesDefaults___";

    const std::string basePath = megaApiCacheFolder(0);
    wipePersistedConnectionsDbFilesUnder(basePath);

    ASSERT_NO_FATAL_FAILURE(getAccountsForTest(1));

    expectMaxConnections(defaultMaxConnections());
}

/**
 * @brief TEST_F CorruptDbFallsBackToDefaults
 *
 * A corrupt prefs DB file must not crash the client; defaults apply. Writes
 * junk bytes directly to the exact path SqliteDbAccess::databasePath produces
 * for DB_VERSION, rather than spraying files for every plausible version.
 */
TEST_F(SdkTestPersistConnections, CorruptDbFallsBackToDefaults)
{
    LOG_info << "___TEST CorruptDbFallsBackToDefaults___";

    const std::string basePath = megaApiCacheFolder(0);
    wipePersistedConnectionsDbFilesUnder(basePath);

    std::error_code ec;
    fs::create_directories(basePath, ec);

    const fs::path p = prefsDbPath(basePath);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << "NOT A VALID SQLITE DATABASE";

    ASSERT_NO_FATAL_FAILURE(getAccountsForTest(1));

    expectMaxConnections(defaultMaxConnections());
}

/**
 * @brief TEST_F OutOfRangeSetIsNotPersisted
 *
 * The public setter rejects values > MAX_NUM_CONNECTIONS (100) with
 * API_ETOOMANY; such rejected writes must never land on disk, so the next SDK
 * session must read the previously-persisted valid value (or the platform
 * default if nothing valid was ever persisted). Cover both the
 * direction-specific overload and the global overload.
 */
TEST_F(SdkTestPersistConnections, OutOfRangeSetIsNotPersisted)
{
    LOG_info << "___TEST OutOfRangeSetIsNotPersisted___";

    const std::string basePath = megaApiCacheFolder(0);
    const auto defaults = defaultMaxConnections();
    wipePersistedConnectionsDbFilesUnder(basePath);
    ASSERT_NO_FATAL_FAILURE(getAccountsForTest(1));

    constexpr int goodPut = 7;
    ASSERT_EQ(API_OK, doSetMaxConnections(0, MegaTransfer::TYPE_UPLOAD, goodPut));

    ASSERT_EQ(API_ETOOMANY, doSetMaxConnections(0, MegaTransfer::TYPE_UPLOAD, 200));
    expectMaxConnections({goodPut, defaults.download});

    ASSERT_EQ(API_ETOOMANY, doSetMaxConnections(0, 200));
    expectMaxConnections({goodPut, defaults.download});

    releaseMegaApi(0);

    recreateMegaApi(basePath);
    expectMaxConnections({goodPut, defaults.download});
}

/**
 * @brief TEST_F NonPositiveSetReturnsEargsAndDoesNotPersist
 *
 * Non-positive values are invalid public API input: the request must fail with
 * API_EARGS and leave both the live and persisted values unchanged.
 */
TEST_F(SdkTestPersistConnections, NonPositiveSetReturnsEargsAndDoesNotPersist)
{
    LOG_info << "___TEST NonPositiveSetReturnsEargsAndDoesNotPersist___";

    const std::string basePath = megaApiCacheFolder(0);
    wipePersistedConnectionsDbFilesUnder(basePath);
    ASSERT_NO_FATAL_FAILURE(getAccountsForTest(1));

    constexpr int persistedConnections = 6;
    ASSERT_EQ(API_OK, doSetMaxConnections(0, persistedConnections));
    expectMaxConnections({persistedConnections, persistedConnections});

    ASSERT_EQ(API_EARGS, doSetMaxConnections(0, MegaTransfer::TYPE_UPLOAD, 0));
    expectMaxConnections({persistedConnections, persistedConnections});

    ASSERT_EQ(API_EARGS, doSetMaxConnections(0, -5));
    expectMaxConnections({persistedConnections, persistedConnections});

    releaseMegaApi(0);

    recreateMegaApi(basePath);
    expectMaxConnections({persistedConnections, persistedConnections});
}

/**
 * @brief TEST_F OutOfRangePersistedValueIgnored
 *
 * Covers the load-side out-of-range guard in ClientPrefsStore: when a prefs
 * DB on disk holds a value outside [1, MAX_NUM_CONNECTIONS], load must reject
 * it, delete the row, and fall back to the platform default. Because the
 * public setter never emits such a value (it returns API_ETOOMANY first), we
 * tamper the sqlite file directly via sqlite3_open. The packed prefs blob now
 * lives at row id 2 and stores [presence-mask, GET, PUT], so we tamper the
 * PUT slot while marking only PUT as present.
 */
TEST_F(SdkTestPersistConnections, OutOfRangePersistedValueIgnored)
{
    LOG_info << "___TEST OutOfRangePersistedValueIgnored___";

    const std::string basePath = megaApiCacheFolder(0);
    wipePersistedConnectionsDbFilesUnder(basePath);

    // Phase 1 — persist a legitimate value so the prefs DB + schema exist.
    {
        ASSERT_NO_FATAL_FAILURE(getAccountsForTest(1));
        ASSERT_EQ(API_OK, doSetMaxConnections(0, MegaTransfer::TYPE_UPLOAD, 5));
        releaseMegaApi(0);
    }

    // Phase 2 — tamper row id 2 with a packed blob whose PUT slot is
    // 0xC8 (200, > MAX_NUM_CONNECTIONS) and only PUT is marked as present.
    const fs::path dbPath = prefsDbPath(basePath);
    ASSERT_TRUE(fs::exists(dbPath)) << "prefs DB was not created at " << dbPath;

    // RAII wrappers so any intermediate ASSERT_EQ early-return still
    // finalizes/closes the sqlite3 handles in the right order.
    using SqliteDbPtr = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
    using SqliteStmtPtr = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

    sqlite3* rawDb = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(dbPath.string().c_str(), &rawDb));
    SqliteDbPtr db{rawDb, &sqlite3_close};

    sqlite3_stmt* rawStmt = nullptr;
    ASSERT_EQ(SQLITE_OK,
              sqlite3_prepare_v2(db.get(),
                                 "INSERT OR REPLACE INTO statecache (id, content) VALUES (?1, ?2)",
                                 -1,
                                 &rawStmt,
                                 nullptr));
    SqliteStmtPtr stmt{rawStmt, &sqlite3_finalize};

    const std::array<unsigned char, 3> tamperedBlob = {kPrefsConnectionMaskPut, 0, 0xC8};
    ASSERT_EQ(SQLITE_OK, sqlite3_bind_int(stmt.get(), 1, kPrefsCombinedConnectionsRowId));
    ASSERT_EQ(SQLITE_OK,
              sqlite3_bind_blob(stmt.get(),
                                2,
                                tamperedBlob.data(),
                                static_cast<int>(tamperedBlob.size()),
                                SQLITE_TRANSIENT));
    ASSERT_EQ(SQLITE_DONE, sqlite3_step(stmt.get()));

    // Phase 3 — reconstruct MegaApi; load must reject the tampered value,
    // delete the row, and fall back to the platform default.
    {
        recreateMegaApi(basePath);
        expectMaxConnections(defaultMaxConnections());

        // Phase 4 — a subsequent valid setMaxConnections re-persists (proving
        // the load-time del() didn't leave the table in a broken state).
        ASSERT_EQ(API_OK, doSetMaxConnections(0, MegaTransfer::TYPE_UPLOAD, 6));
        releaseMegaApi(0);

        recreateMegaApi(basePath);
        expectMaxConnections({6, defaultMaxConnections().download});
    }
}

/**
 * @brief TEST_F PersistSurvivesDbVersionBump
 *
 * Simulates what happens on the disk after an SDK upgrade that bumps
 * DB_VERSION by one: the on-disk prefs file is named for the previous version,
 * and the new MegaApi looks for the new version. The RECYCLE flag passed from
 * MegaClient::openPrefsTable must drive checkDbFileAndAdjustLegacy to rename
 * the legacy file up to the current version (preserving the stored value)
 * rather than deleting it.
 *
 * DB_VERSION cannot be changed at runtime, so the version bump is simulated at
 * the filename layer: we persist a value under <DB_VERSION>, rename that file
 * (plus any -shm/-wal/-journal siblings) to <DB_VERSION - 1>, then reconstruct
 * MegaApi and assert the value comes back.
 */
TEST_F(SdkTestPersistConnections, PersistSurvivesDbVersionBump)
{
    LOG_info << "___TEST PersistSurvivesDbVersionBump___";

    const std::string basePath = megaApiCacheFolder(0);
    wipePersistedConnectionsDbFilesUnder(basePath);

    ASSERT_NO_FATAL_FAILURE(getAccountsForTest(1));

    constexpr int desiredPut = 5;
    ASSERT_EQ(API_OK, doSetMaxConnections(0, MegaTransfer::TYPE_UPLOAD, desiredPut));

    releaseMegaApi(0);

    const fs::path currentPath = prefsDbPath(basePath);
    ASSERT_TRUE(fs::exists(currentPath)) << "prefs DB was not created at " << currentPath;

    const fs::path legacyPath =
        fs::path{basePath} /
        ("megaclient_statecache" + std::to_string(::mega::DbAccess::DB_VERSION - 1) + "_prefs.db");

    constexpr std::array<const char*, 4> kSuffixes = {"", "-shm", "-wal", "-journal"};
    for (const char* suffix: kSuffixes)
    {
        const fs::path from = currentPath.string() + suffix;
        const fs::path to = legacyPath.string() + suffix;
        std::error_code ec;
        if (fs::exists(from, ec))
        {
            fs::rename(from, to, ec);
            ASSERT_FALSE(ec) << "failed to rename " << from << " -> " << to << ": " << ec.message();
        }
    }

    ASSERT_TRUE(fs::exists(legacyPath)) << "legacy prefs DB not present after rename";
    ASSERT_FALSE(fs::exists(currentPath))
        << "current-version prefs DB should have been renamed away";

    recreateMegaApi(basePath);
    expectMaxConnections({desiredPut, defaultMaxConnections().download});

    EXPECT_TRUE(fs::exists(currentPath)) << "current-version prefs DB should exist after recycle";
    EXPECT_FALSE(fs::exists(legacyPath)) << "legacy prefs DB should have been recycled away";
}
