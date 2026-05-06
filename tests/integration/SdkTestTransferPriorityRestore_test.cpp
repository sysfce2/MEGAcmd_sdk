/**
 * @file SdkTestTransferPriorityRestore_test.cpp
 * @brief Integration tests verifying that transfer priorities survive an
 *        app-restart (session save → local logout → session resume).
 *
 * Bug description
 * ---------------
 * When transfers is saved to the transfer DB while it is still
 * queued (i.e., no transfer slot has been allocated yet), its Transfer::priority
 * is correctly persisted by Transfer::serialize.  However, when the session is
 * resumed in MegaClient::startxfer, the matching logic that looks up the cached
 * Transfer in multi_cachedtransfers requires:
 *
 *     For GET: downloadFileHandle == f->h && !downloadFileHandle.isUndef()
 *     For PUT: it->second->localfilename == f->getLocalname()
 *
 * Because 'downloadFileHandle' is only populated after the slot receives the
 * actual download URLs, queued-but-not-started downloads always have it set to undef.
 * And 'localfilename' is only populated after slot assigned.
 * The match therefore fails, startxfer creates a brand-new Transfer with priority 0,
 * and addtransfer assigns a fresh sequential priority — the original priority is lost.
 *
 */

#include "sdk_test_utils.h"
#include "SdkTest_test.h"

#include <algorithm>
#include <map>

using namespace mega;

namespace
{

/**
 * Helper: wait until the transfer-list snapshot reported by the API contains
 * exactly the expected number of transfers of the given type.
 */
bool waitForTransferCount(MegaApi* api, int type, int expectedCount, unsigned timeoutMs = 30000)
{
    return SdkTest::WaitFor(
        [api, type, expectedCount]()
        {
            auto list = std::unique_ptr<MegaTransferList>{api->getTransfers(type)};
            return list && list->size() == expectedCount;
        },
        timeoutMs);
}

/**
 * Helper for DOWNLOAD transfers: collect {nodeHandle → priority}.
 *
 * For downloads, MegaTransfer::getNodeHandle() returns the cloud node handle
 * of the source file (set in MegaApiImpl::file_added when type == GET).  This
 * handle is unique per file and is stable across session restarts, making it
 * an ideal key.
 *
 * NOTE: do NOT use this for uploads. For upload transfers the cloud node does
 * not exist yet (the file hasn't been uploaded), so getNodeHandle() returns
 * UNDEF for every transfer regardless of which local file it refers to. Use
 * collectPathToPriority() for uploads instead.
 */
std::map<MegaHandle, unsigned long long> collectHandleToPriority(MegaApi* api, int type)
{
    std::map<MegaHandle, unsigned long long> result;
    auto list = std::unique_ptr<MegaTransferList>{api->getTransfers(type)};
    if (!list)
        return result;
    for (int i = 0; i < list->size(); ++i)
    {
        auto* t = list->get(i);
        result[t->getNodeHandle()] = t->getPriority();
    }
    return result;
}

/**
 * Helper for UPLOAD transfers: collect {localPath → priority}.
 *
 * For uploads, the cloud node handle is UNDEF until the upload completes, so
 * it cannot serve as a unique key. The local file path (MegaTransfer::getPath)
 * is set from File::logicalPath() in MegaApiImpl::file_added and is restored to
 * the same value when the transfer is resumed from DB after a restart.  It is
 * therefore a stable, unique key for queued upload transfers.
 */
std::map<std::string, unsigned long long> collectPathToPriority(MegaApi* api, int type)
{
    std::map<std::string, unsigned long long> result;
    auto list = std::unique_ptr<MegaTransferList>{api->getTransfers(type)};
    if (!list)
        return result;
    for (int i = 0; i < list->size(); ++i)
    {
        auto* t = list->get(i);
        const char* path = t->getPath();
        if (path && *path != '\0')
            result[path] = t->getPriority();
    }
    return result;
}

} // anonymous namespace

class SdkTestTransferPriorityRestore: public SdkTest
{
public:
    void SetUp() override
    {
        SdkTest::SetUp();
        ASSERT_NO_FATAL_FAILURE(getAccountsForTest(1));
    }

    void TearDown() override
    {
        if (megaApi[0] && megaApi[0]->isLoggedIn())
        {
            megaApi[0]->pauseTransfers(false, MegaTransfer::TYPE_UPLOAD);
            megaApi[0]->pauseTransfers(false, MegaTransfer::TYPE_DOWNLOAD);
            synchronousCancelTransfers(0, MegaTransfer::TYPE_UPLOAD);
            synchronousCancelTransfers(0, MegaTransfer::TYPE_DOWNLOAD);

            for (const auto handle: mCloudNodesToDelete)
            {
                auto node = std::unique_ptr<MegaNode>{megaApi[0]->getNodeByHandle(handle)};
                if (node)
                    doDeleteNode(0, node.get());
            }
        }
        SdkTest::TearDown();
    }

protected:
    std::vector<MegaHandle> mCloudNodesToDelete;
};

// Test 1: Upload priorities are preserved after a session restart.
TEST_F(SdkTestTransferPriorityRestore, upload_priorities_preserved_after_restart)
{
    constexpr int NUM_FILES = 3;
    constexpr size_t FILE_SIZE = 64;

    auto rootNode = std::unique_ptr<MegaNode>{megaApi[0]->getRootNode()};
    ASSERT_TRUE(rootNode) << "Cannot get root node";

    // Create local files.
    std::vector<sdk_test::LocalTempFile> localFiles;
    localFiles.reserve(static_cast<size_t>(NUM_FILES));
    for (int i = 0; i < NUM_FILES; ++i)
    {
        localFiles.emplace_back(fs::current_path() / (getFilePrefix() + std::to_string(i) + ".bin"),
                                FILE_SIZE);
    }

    // Pause all uploads so they queue up without being scheduled.
    megaApi[0]->pauseTransfers(true, MegaTransfer::TYPE_UPLOAD);

    // Start the uploads while paused.
    std::vector<std::unique_ptr<TransferTracker>> trackers;
    trackers.reserve(static_cast<size_t>(NUM_FILES));
    MegaUploadOptions opts;
    opts.mtime = MegaApi::INVALID_CUSTOM_MOD_TIME;
    for (int i = 0; i < NUM_FILES; ++i)
    {
        trackers.push_back(std::make_unique<TransferTracker>(megaApi[0].get()));
        megaApi[0]->startUpload(localFiles[static_cast<size_t>(i)].getPath().string().c_str(),
                                rootNode.get(),
                                nullptr,
                                &opts,
                                trackers.back().get());
    }

    // Wait for all uploads to appear in the queue.
    ASSERT_TRUE(waitForTransferCount(megaApi[0].get(), MegaTransfer::TYPE_UPLOAD, NUM_FILES, 30000))
        << "Uploads did not appear in the queue within timeout";

    // Record priorities before restart, keyed by local file path
    auto prioritiesBefore = collectPathToPriority(megaApi[0].get(), MegaTransfer::TYPE_UPLOAD);
    ASSERT_EQ(static_cast<int>(prioritiesBefore.size()), NUM_FILES)
        << "Expected " << NUM_FILES
        << " distinct upload entries keyed by local path; "
           "if this fails the helper may not see a valid getPath() for each transfer";

    // Verify they have distinct, non-zero priorities.
    {
        std::vector<unsigned long long> pVals;
        pVals.reserve(prioritiesBefore.size());
        for (auto& [localPath, p]: prioritiesBefore)
            pVals.push_back(p);
        std::sort(pVals.begin(), pVals.end());
        ASSERT_EQ(std::unique(pVals.begin(), pVals.end()), pVals.end())
            << "Priorities before restart must all be distinct";
        for (auto p: pVals)
            ASSERT_GT(p, 0u) << "Priority must be non-zero";
    }

    // Save session and do a local logout (simulate app restart).
    std::unique_ptr<char[]> session(dumpSession());
    ASSERT_TRUE(session) << "Cannot dump session";
    ASSERT_NO_FATAL_FAILURE(locallogout());

    // Resume session (app restart).
    // Pause uploads before fetchnodes so that when the SDK restores transfers
    // from the transfer DB during fetchnodes, they are queued but not dispatched.
    // Calling pauseTransfers after fetchnodes would be racy: on a fast network the
    // restored transfers could complete in the window between fetchnodes returning
    // and the pause taking effect, causing waitForTransferCount to time out.
    ASSERT_NO_FATAL_FAILURE(resumeSession(session.get()));
    megaApi[0]->pauseTransfers(true, MegaTransfer::TYPE_UPLOAD);
    ASSERT_NO_FATAL_FAILURE(fetchnodes(0));

    // Wait for resumed uploads to appear.
    ASSERT_TRUE(waitForTransferCount(megaApi[0].get(), MegaTransfer::TYPE_UPLOAD, NUM_FILES, 30000))
        << "Resumed uploads did not appear within timeout";

    // Verify priorities are unchanged.
    auto prioritiesAfter = collectPathToPriority(megaApi[0].get(), MegaTransfer::TYPE_UPLOAD);
    for (auto& [localPath, priorityBefore]: prioritiesBefore)
    {
        ASSERT_TRUE(prioritiesAfter.count(localPath))
            << "Upload transfer for path '" << localPath << "' missing after restart";
        EXPECT_EQ(prioritiesAfter.at(localPath), priorityBefore)
            << "Upload priority changed after restart for path '" << localPath << "'";
    }

    // Cancel remaining uploads.
    megaApi[0]->pauseTransfers(false, MegaTransfer::TYPE_UPLOAD);
    ASSERT_EQ(API_OK, synchronousCancelTransfers(0, MegaTransfer::TYPE_UPLOAD));
}

// Test 2: Download priorities are preserved after restart when the downloads
//         were queued but NEVER started (downloadFileHandle stays undef).
TEST_F(SdkTestTransferPriorityRestore, queued_download_priorities_preserved_after_restart)
{
    constexpr int NUM_FILES = 3;
    constexpr size_t FILE_SIZE = 64;

    auto rootNode = std::unique_ptr<MegaNode>{megaApi[0]->getRootNode()};
    ASSERT_TRUE(rootNode) << "Cannot get root node";

    // Upload source files to have nodes to download.
    std::vector<sdk_test::LocalTempFile> srcFiles;
    srcFiles.reserve(static_cast<size_t>(NUM_FILES));
    std::vector<MegaHandle> nodeHandles(static_cast<size_t>(NUM_FILES), UNDEF);
    for (int i = 0; i < NUM_FILES; ++i)
    {
        srcFiles.emplace_back(fs::current_path() /
                                  (getFilePrefix() + "src_" + std::to_string(i) + ".bin"),
                              FILE_SIZE);
        ASSERT_EQ(API_OK,
                  doStartUpload(0,
                                &nodeHandles[static_cast<size_t>(i)],
                                srcFiles[static_cast<size_t>(i)].getPath().string().c_str(),
                                rootNode.get(),
                                nullptr,
                                MegaApi::INVALID_CUSTOM_MOD_TIME,
                                nullptr,
                                false,
                                false,
                                nullptr))
            << "Failed to upload source file " << i;
        ASSERT_NE(UNDEF, nodeHandles[static_cast<size_t>(i)]);
        mCloudNodesToDelete.push_back(nodeHandles[static_cast<size_t>(i)]);
    }

    // Pause all downloads before starting them.
    // This ensures no slot is ever allocated, so downloadFileHandle stays undef.
    megaApi[0]->pauseTransfers(true, MegaTransfer::TYPE_DOWNLOAD);

    sdk_test::LocalTempDir downloadDir(fs::current_path() / (getFilePrefix() + "dl"));

    std::vector<std::unique_ptr<TransferTracker>> dlTrackers;
    dlTrackers.reserve(static_cast<size_t>(NUM_FILES));
    for (int i = 0; i < NUM_FILES; ++i)
    {
        auto node = std::unique_ptr<MegaNode>{
            megaApi[0]->getNodeByHandle(nodeHandles[static_cast<size_t>(i)])};
        ASSERT_TRUE(node) << "Cannot get node " << i;

        dlTrackers.push_back(std::make_unique<TransferTracker>(megaApi[0].get()));
        megaApi[0]->startDownload(
            node.get(),
            (downloadDir.getPath().string() + LocalPath::localPathSeparator_utf8).c_str(),
            nullptr,
            nullptr,
            false,
            nullptr,
            MegaTransfer::COLLISION_CHECK_FINGERPRINT,
            MegaTransfer::COLLISION_RESOLUTION_NEW_WITH_N,
            false,
            dlTrackers.back().get());
    }

    // Wait until all downloads are visible in the queue.
    ASSERT_TRUE(
        waitForTransferCount(megaApi[0].get(), MegaTransfer::TYPE_DOWNLOAD, NUM_FILES, 30000))
        << "Downloads did not appear in the queue within timeout";

    // Record priorities before restart, keyed by node handle (stable across restart).
    auto prioritiesBefore = collectHandleToPriority(megaApi[0].get(), MegaTransfer::TYPE_DOWNLOAD);
    ASSERT_EQ(static_cast<int>(prioritiesBefore.size()), NUM_FILES);

    // All priorities must be distinct and non-zero.
    {
        std::vector<unsigned long long> pVals;
        pVals.reserve(prioritiesBefore.size());
        for (auto& [h, p]: prioritiesBefore)
            pVals.push_back(p);
        std::sort(pVals.begin(), pVals.end());
        ASSERT_EQ(std::unique(pVals.begin(), pVals.end()), pVals.end())
            << "Priorities before restart must all be distinct";
        for (auto p: pVals)
            ASSERT_GT(p, 0u) << "Priority must be non-zero";
    }

    // Simulate app restart: save session, local logout, resume session.
    std::unique_ptr<char[]> session(dumpSession());
    ASSERT_TRUE(session) << "Cannot dump session";
    ASSERT_NO_FATAL_FAILURE(locallogout());

    // Pause downloads before fetchnodes so that restored transfers from the
    // transfer DB are queued but not dispatched. See upload_priorities test
    // for a detailed explanation of the race this avoids.
    ASSERT_NO_FATAL_FAILURE(resumeSession(session.get()));
    megaApi[0]->pauseTransfers(true, MegaTransfer::TYPE_DOWNLOAD);
    ASSERT_NO_FATAL_FAILURE(fetchnodes(0));

    // Wait for the resumed downloads to appear in the queue.
    ASSERT_TRUE(
        waitForTransferCount(megaApi[0].get(), MegaTransfer::TYPE_DOWNLOAD, NUM_FILES, 30000))
        << "Resumed downloads did not appear within timeout. "
           "The transfers may not have been persisted, or they were discarded on resume.";

    // Collect priorities after restart.
    auto prioritiesAfter = collectHandleToPriority(megaApi[0].get(), MegaTransfer::TYPE_DOWNLOAD);

    // Each download has the same priority as before the restart.
    for (auto& [handle, priorityBefore]: prioritiesBefore)
    {
        ASSERT_TRUE(prioritiesAfter.count(handle))
            << "Download transfer for node " << handle << " is missing after restart";

        EXPECT_EQ(prioritiesAfter.at(handle), priorityBefore)
            << "BUG: Download priority changed after restart for node " << handle
            << ". Before=" << priorityBefore << " After=" << prioritiesAfter.at(handle)
            << ". Queued downloads without a transfer slot lose their priority because "
               "startxfer cannot match them in multi_cachedtransfers when "
               "downloadFileHandle is undef.";
    }

    // Cancel pending downloads.
    megaApi[0]->pauseTransfers(false, MegaTransfer::TYPE_DOWNLOAD);
    ASSERT_EQ(API_OK, synchronousCancelTransfers(0, MegaTransfer::TYPE_DOWNLOAD));
}

// Test 3: Relative priority order of queued downloads is preserved after restart.
TEST_F(SdkTestTransferPriorityRestore, queued_download_priority_order_preserved_after_restart)
{
    constexpr int NUM_FILES = 4;
    constexpr size_t FILE_SIZE = 64;

    auto rootNode = std::unique_ptr<MegaNode>{megaApi[0]->getRootNode()};
    ASSERT_TRUE(rootNode) << "Cannot get root node";

    // Upload source files.
    std::vector<sdk_test::LocalTempFile> srcFiles;
    srcFiles.reserve(static_cast<size_t>(NUM_FILES));
    std::vector<MegaHandle> nodeHandles(static_cast<size_t>(NUM_FILES), UNDEF);
    for (int i = 0; i < NUM_FILES; ++i)
    {
        srcFiles.emplace_back(fs::current_path() /
                                  (getFilePrefix() + "src_" + std::to_string(i) + ".bin"),
                              FILE_SIZE);
        ASSERT_EQ(API_OK,
                  doStartUpload(0,
                                &nodeHandles[static_cast<size_t>(i)],
                                srcFiles[static_cast<size_t>(i)].getPath().string().c_str(),
                                rootNode.get(),
                                nullptr,
                                MegaApi::INVALID_CUSTOM_MOD_TIME,
                                nullptr,
                                false,
                                false,
                                nullptr))
            << "Failed to upload source file " << i;
        ASSERT_NE(UNDEF, nodeHandles[static_cast<size_t>(i)]);
        mCloudNodesToDelete.push_back(nodeHandles[static_cast<size_t>(i)]);
    }

    // Pause downloads before starting them.
    megaApi[0]->pauseTransfers(true, MegaTransfer::TYPE_DOWNLOAD);

    sdk_test::LocalTempDir downloadDir(fs::current_path() / (getFilePrefix() + "dl"));

    std::vector<std::unique_ptr<TransferTracker>> dlTrackers;
    dlTrackers.reserve(static_cast<size_t>(NUM_FILES));
    for (int i = 0; i < NUM_FILES; ++i)
    {
        auto node = std::unique_ptr<MegaNode>{
            megaApi[0]->getNodeByHandle(nodeHandles[static_cast<size_t>(i)])};
        ASSERT_TRUE(node);

        dlTrackers.push_back(std::make_unique<TransferTracker>(megaApi[0].get()));
        megaApi[0]->startDownload(
            node.get(),
            (downloadDir.getPath().string() + LocalPath::localPathSeparator_utf8).c_str(),
            nullptr,
            nullptr,
            false,
            nullptr,
            MegaTransfer::COLLISION_CHECK_FINGERPRINT,
            MegaTransfer::COLLISION_RESOLUTION_NEW_WITH_N,
            false,
            dlTrackers.back().get());
    }

    ASSERT_TRUE(
        waitForTransferCount(megaApi[0].get(), MegaTransfer::TYPE_DOWNLOAD, NUM_FILES, 30000))
        << "Downloads did not appear in the queue";

    // Build ordered list [handle, priority] sorted ascending by priority.
    struct HndPri
    {
        MegaHandle handle;
        unsigned long long priority;
    };

    auto buildOrderedList = [](MegaApi* api, int type) -> std::vector<HndPri>
    {
        std::vector<HndPri> vec;
        auto list = std::unique_ptr<MegaTransferList>{api->getTransfers(type)};
        if (!list)
            return vec;
        for (int i = 0; i < list->size(); ++i)
        {
            auto* t = list->get(i);
            vec.push_back({t->getNodeHandle(), t->getPriority()});
        }
        std::sort(vec.begin(),
                  vec.end(),
                  [](const HndPri& a, const HndPri& b)
                  {
                      return a.priority < b.priority;
                  });
        return vec;
    };

    auto orderBefore = buildOrderedList(megaApi[0].get(), MegaTransfer::TYPE_DOWNLOAD);
    ASSERT_EQ(static_cast<int>(orderBefore.size()), NUM_FILES);

    // Simulate app restart.
    // Pause downloads before fetchnodes to avoid the race described in the
    // upload_priorities test: restored DB transfers must not be dispatched
    // in the window between fetchnodes returning and the pause taking effect.
    std::unique_ptr<char[]> session(dumpSession());
    ASSERT_TRUE(session);
    ASSERT_NO_FATAL_FAILURE(locallogout());
    ASSERT_NO_FATAL_FAILURE(resumeSession(session.get()));
    megaApi[0]->pauseTransfers(true, MegaTransfer::TYPE_DOWNLOAD);
    ASSERT_NO_FATAL_FAILURE(fetchnodes(0));

    ASSERT_TRUE(
        waitForTransferCount(megaApi[0].get(), MegaTransfer::TYPE_DOWNLOAD, NUM_FILES, 30000))
        << "Resumed downloads did not appear after restart";

    auto orderAfter = buildOrderedList(megaApi[0].get(), MegaTransfer::TYPE_DOWNLOAD);
    ASSERT_EQ(orderBefore.size(), orderAfter.size());

    // Sorted handle order is identical (relative priority preserved).
    for (size_t i = 0; i < orderBefore.size(); ++i)
    {
        EXPECT_EQ(orderBefore[i].handle, orderAfter[i].handle)
            << "BUG: Relative download priority order changed after restart at position " << i
            << ". Before: handle=" << orderBefore[i].handle
            << " priority=" << orderBefore[i].priority << "; After: handle=" << orderAfter[i].handle
            << " priority=" << orderAfter[i].priority;
    }

    megaApi[0]->pauseTransfers(false, MegaTransfer::TYPE_DOWNLOAD);
    ASSERT_EQ(API_OK, synchronousCancelTransfers(0, MegaTransfer::TYPE_DOWNLOAD));
}
