/**
 * @file sdk_test_share_nested.cpp
 * @brief This file defines tests related with nested shares
 */

#include "integration_test_utils.h"
#include "sdk_test_share.h"
#include "SdkTestNodesSetUp.h"

using namespace sdk_test;

class SdkTestShareNested:
    public virtual SdkTestShare,
    public virtual SdkTestNodesSetUp,
    virtual public ::testing::WithParamInterface<int>
{
public:
    enum class ShareOrder : int
    {
        Forward,
        Reverse
    };

    void SetUp() override
    {
        SdkTestShare::SetUp();
        ASSERT_NO_FATAL_FAILURE(getAccountsForTest(3));
        ASSERT_NO_FATAL_FAILURE(createRootTestDir());
        createNodes(getElements(), getRootTestDirectory());
    }

    const std::string& getRootTestDir() const override
    {
        return rootTestDir;
    }

    const std::vector<sdk_test::NodeInfo>& getElements() const override
    {
        return treeElements;
    }

    // Override, we don't need to have different creation time.
    bool keepDifferentCreationTimes() override
    {
        return false;
    }

    // Create a file node in the remote account.
    // Optionally, it will be validate in apiIndexB acount.
    void createRemoteFileNode(unsigned apiIndexA,
                              const sdk_test::FileNodeInfo& fileInfo,
                              MegaNode* rootnode,
                              MegaHandle& newNodeHandleResult,
                              std::optional<unsigned> apiIndexB)
    {
        bool checkA{false};
        bool checkB{false};
        mApi[apiIndexA].mOnNodesUpdateCompletion =
            createOnNodesUpdateLambda(INVALID_HANDLE, MegaNode::CHANGE_TYPE_NEW, checkA);
        if (apiIndexB)
        {
            mApi[*apiIndexB].mOnNodesUpdateCompletion =
                createOnNodesUpdateLambda(INVALID_HANDLE, MegaNode::CHANGE_TYPE_NEW, checkB);
        }
        sdk_test::LocalTempFile localFile{fileInfo.name, fileInfo.size};
        ASSERT_EQ(MegaError::API_OK,
                  doStartUpload(apiIndexA,
                                &newNodeHandleResult,
                                fileInfo.name.c_str(),
                                rootnode,
                                nullptr /*fileName*/,
                                fileInfo.mtime,
                                nullptr /*appData*/,
                                false /*isSourceTemporary*/,
                                false /*startFirst*/,
                                nullptr /*cancelToken*/))
            << "Failure uploading a file";

        ASSERT_TRUE(waitForResponse(&checkA)) << "New node not received on client " << apiIndexA
                                              << " after " << maxTimeout << " seconds";
        if (apiIndexB)
        {
            ASSERT_TRUE(waitForResponse(&checkB))
                << "New node not received on client " << *apiIndexB << " after " << maxTimeout
                << " seconds";
        }
        resetOnNodeUpdateCompletionCBs();
        std::unique_ptr<MegaNode> nodeFile{
            megaApi[apiIndexA]->getNodeByHandle(newNodeHandleResult)};
        ASSERT_NE(nodeFile, nullptr)
            << "Cannot get the node for the updated file (error: " << mApi[apiIndexA].lastError
            << ")";
    }

    void matchTree(const MegaHandle rootHandle, unsigned apiIndexA, unsigned apiIndexB) const
    {
        const std::unique_ptr<MegaNode> rootNodeA{megaApi[apiIndexA]->getNodeByHandle(rootHandle)};
        const std::unique_ptr<MegaNode> rootNodeB{megaApi[apiIndexB]->getNodeByHandle(rootHandle)};
        ASSERT_TRUE(rootNodeA) << "Node not present the accout #" << apiIndexA
                               << ". Handle: " << toNodeHandle(rootHandle);
        ASSERT_TRUE(rootNodeB) << "Node not present the second accout#" << apiIndexB
                               << ". Handle: " << toNodeHandle(rootHandle);

        ASSERT_NO_FATAL_FAILURE(
            matchTreeRecurse(rootNodeA.get(), rootNodeB.get(), apiIndexA, apiIndexB));
    }

    bool waitForNodeToBeDecrypted(unsigned apiIndex, MegaHandle nodeHandle)
    {
        return WaitFor(
            [this, apiIndex, nodeHandle]()
            {
                unique_ptr<MegaNode> node(megaApi[apiIndex]->getNodeByHandle(nodeHandle));
                return node && node->isNodeKeyDecrypted();
            },
            defaultTimeoutMs);
    }

    void reloginAccount(unsigned idx, bool forceNoCachedFetchnodes = false)
    {
        ASSERT_NO_FATAL_FAILURE(logout(idx, false, maxTimeout));
        ASSERT_NO_FATAL_FAILURE(login(idx));
        if (forceNoCachedFetchnodes)
        {
            megaApi[idx]->getClient()->fetchnodesAlreadyCompletedThisSession = true;
        }
        ASSERT_NO_FATAL_FAILURE(fetchnodes(idx));
    }

    void reloginAllAccounts(bool forceNoCachedFetchnodes = false)
    {
        for (auto idx: std::array{sharerIndex, shareeAliceIndex, shareeBobIndex})
        {
            reloginAccount(idx, forceNoCachedFetchnodes);
        }
    }

protected:
    // Name of the initial elements in the remote tree
    static constexpr auto FOLDER_A = "folderA";
    static constexpr auto FOLDER_B = "folderB";
    static constexpr auto FOLDER_C = "folderC";
    static constexpr auto FOLDER_EXP = "exportedFolder";
    static constexpr auto FILE_A = "fileA";
    static constexpr auto FILE_B = "fileB";
    static constexpr auto FILE_C = "fileC";

    static constexpr unsigned sharerIndex{0};
    static constexpr unsigned shareeAliceIndex{1};
    static constexpr unsigned shareeBobIndex{2};

private:
    // root in the cloud where the tree is created
    const std::string rootTestDir{"SdkTestShareNested"};

    // It represents the following tree:
    // RemoteRoot
    // ├── "exportedFolder"
    // └── "folderA"
    //     ├── "fileA"
    //     └── "folderB"
    //         ├── "fileB"
    //         └── "folderC"
    //             └── "fileC"
    const std::vector<NodeInfo> treeElements{
        DirNodeInfo(FOLDER_A)
            .addChild(FileNodeInfo(FILE_A).setSize(100))
            .addChild(
                DirNodeInfo(FOLDER_B)
                    .addChild(FileNodeInfo(FILE_B).setSize(100))
                    .addChild(DirNodeInfo(FOLDER_C).addChild(FileNodeInfo(FILE_C).setSize(100)))),
        DirNodeInfo(FOLDER_EXP)};

    // Check if the passed nodes have the same handle, if they are are decrypted and if they have
    // the same name. Print meaninful messages depending on the assert.
    void verifySameNodes(MegaNode* nodeA,
                         unsigned apiIndexA,
                         MegaNode* nodeB,
                         unsigned apiIndexB) const
    {
        ASSERT_TRUE(nodeA && nodeB) << "Invalid nodes in the comparision.";
        ASSERT_EQ(nodeA->getHandle(), nodeB->getHandle())
            << "Handles don't match. " << toNodeHandle(nodeA->getHandle()) << " vs "
            << toNodeHandle(nodeB->getHandle());
        ASSERT_TRUE(nodeA->isNodeKeyDecrypted() || nodeB->isNodeKeyDecrypted())
            << "Node " << toNodeHandle(nodeA->getHandle())
            << " is not decryptable in both accounts " << apiIndexA << " and " << apiIndexB;
        ASSERT_FALSE(nodeA->isNodeKeyDecrypted() && !nodeB->isNodeKeyDecrypted())
            << "Account " << apiIndexB << " can't decrypt " << nodeA->getName();
        ASSERT_FALSE(!nodeA->isNodeKeyDecrypted() && nodeB->isNodeKeyDecrypted())
            << "Account " << apiIndexA << " can't decrypt " << nodeB->getName();
        ASSERT_STREQ(nodeA->getName(), nodeB->getName())
            << "Node names don't match in in both accounts.";
    };

    // It validates the passed nodes and their descents.
    // The function is called recursively for folders.
    void matchTreeRecurse(MegaNode* rootNodeA,
                          MegaNode* rootNodeB,
                          unsigned apiIndexA,
                          unsigned apiIndexB) const
    {
        ASSERT_NO_FATAL_FAILURE(verifySameNodes(rootNodeA, apiIndexA, rootNodeB, apiIndexB));

        std::unique_ptr<MegaNodeList> childrenListA{megaApi[apiIndexA]->getChildren(rootNodeA)};
        std::unique_ptr<MegaNodeList> childrenListB{megaApi[apiIndexB]->getChildren(rootNodeB)};
        std::unordered_map<MegaHandle, MegaNode*>
            indexB; // Index for childrenListB using the handles.

        for (auto j = 0; j < childrenListB->size(); ++j)
        {
            auto childNodeB{childrenListB->get(j)};
            ASSERT_TRUE(childNodeB) << "null node in the list of childs of " << rootNodeB->getName()
                                    << "in the " << apiIndexB << " account.";
            indexB.emplace(childNodeB->getHandle(), childNodeB);
        }

        for (auto i = 0; i < childrenListA->size(); ++i)
        {
            auto childNodeA = childrenListA->get(i);
            ASSERT_TRUE(childNodeA) << "null node in the list of childs of " << rootNodeA->getName()
                                    << "in the " << apiIndexA << " account.";
            auto itChildNodeB = indexB.find(childNodeA->getHandle());
            ASSERT_NE(itChildNodeB, indexB.end())
                << "Can't find "
                << (childNodeA->isNodeKeyDecrypted() ? childNodeA->getName() :
                                                       toNodeHandle(childNodeA->getHandle()))
                << " in the " << apiIndexB << " account";
            auto childNodeB = itChildNodeB->second;
            if (childNodeA->isFolder() && childNodeB->isFolder())
            {
                ASSERT_NO_FATAL_FAILURE(
                    matchTreeRecurse(childNodeA, childNodeB, apiIndexA, apiIndexB));
            }
            else
            {
                ASSERT_NO_FATAL_FAILURE(
                    verifySameNodes(childNodeA, apiIndexA, childNodeB, apiIndexB));
            }
            indexB.erase(itChildNodeB);
        }

        std::string extraNodes;
        for (auto [_, unmatchedChildNodeB]: indexB)
        {
            extraNodes += " " + toNodeHandle(unmatchedChildNodeB->getHandle());
            if (unmatchedChildNodeB->isNodeKeyDecrypted())
                extraNodes += ":" + string(unmatchedChildNodeB->getName());
        }
        ASSERT_EQ(indexB.size(), 0) << "Unexpected " << indexB.size() << " node(s) found in the "
                                    << apiIndexB << " account: " << extraNodes;
    }
};

/**
 * @brief Basic test for nested shares
 *
 * It tests the basic functionality, creating a nested share and ensuring
 * that all peers can see their respective files.
 *
 */
TEST_F(SdkTestShareNested, BasicNestedShares)
{
    const auto logPre = getLogPrefix();

    LOG_info << "Starting body of " << logPre;

    // Make sharer and sharees contacts.
    ASSERT_NO_FATAL_FAILURE(
        inviteTestAccount(sharerIndex, shareeAliceIndex, "Sharer inviting Alice"))
        << "Failure inviting Alice";
    ASSERT_NO_FATAL_FAILURE(inviteTestAccount(sharerIndex, shareeBobIndex, "Sharer inviting Bob"))
        << "Failure inviting Bob";

    if (gManualVerification)
    {
        ASSERT_NO_FATAL_FAILURE(verifyContactCredentials(sharerIndex, shareeAliceIndex));
        ASSERT_NO_FATAL_FAILURE(verifyContactCredentials(sharerIndex, shareeBobIndex));
    }

    LOG_info << logPre << "Share folder \"folderA\" to Alice and subfolder \"folderB\" to Bob";
    auto sharerFolderANode = getNodeByPath(FOLDER_A);
    auto sharerFolderBNode = getNodeByPath(string(FOLDER_A) + "/" + FOLDER_B);
    ASSERT_TRUE(sharerFolderANode) << "folder \"folderA\" not found.";
    ASSERT_TRUE(sharerFolderBNode) << "folder \"folderB\" not found.";
    ASSERT_NO_FATAL_FAILURE(createShareAtoB(sharerFolderANode.get(),
                                            {sharerIndex, true},
                                            {shareeAliceIndex, true},
                                            MegaShare::ACCESS_FULL));
    ASSERT_NO_FATAL_FAILURE(createShareAtoB(sharerFolderBNode.get(),
                                            {sharerIndex, true},
                                            {shareeBobIndex, true},
                                            MegaShare::ACCESS_FULL));

    LOG_info << logPre
             << "Ensure that the sharer, Alice and Bob can see the same nodes and that the tree is "
                "decrypted.";
    waitForNodeToBeDecrypted(shareeAliceIndex, sharerFolderANode->getHandle());
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderANode->getHandle(), sharerIndex, shareeAliceIndex));
    waitForNodeToBeDecrypted(shareeBobIndex, sharerFolderBNode->getHandle());
    ASSERT_NO_FATAL_FAILURE(matchTree(sharerFolderBNode->getHandle(), sharerIndex, shareeBobIndex));
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderBNode->getHandle(), shareeAliceIndex, shareeBobIndex));

    LOG_info << logPre << "Logout and login to ensure that all is correct after fetching nodes.";
    ASSERT_NO_FATAL_FAILURE(reloginAllAccounts());

    LOG_info << logPre
             << "Check again that the sharer, Alice and Bob can see the same nodes and that "
                "the tree is decrypted.";
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderANode->getHandle(), sharerIndex, shareeAliceIndex));
    ASSERT_NO_FATAL_FAILURE(matchTree(sharerFolderBNode->getHandle(), sharerIndex, shareeBobIndex));
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderBNode->getHandle(), shareeAliceIndex, shareeBobIndex));
}

/**
 * @brief Test upload a file in the nested share
 *
 * It test if files uploaded by the sharees are decryptable, creating a nested share and uploading
 * files in the inshare of the nested sharee, ensuring that all peers can see their respective
 * files.
 *
 * Test is parametrizable to change the order of the outshare creation. The idea is to ensure
 * that it works if the nested share is created below an existing share or if a new share is
 * created above the existing nested share. This ensures that the sharekeys are correctly shared
 * and the "k" of the nodes are correctly encrypte and decrypted.
 */
TEST_P(SdkTestShareNested, UploadFilesInNestedShare)
{
    const auto shareOrder = static_cast<ShareOrder>(GetParam());

    const auto logPre = getLogPrefix();

    LOG_info << "Starting body of " << logPre;

    // Make sharer and sharees contacts.
    ASSERT_NO_FATAL_FAILURE(
        inviteTestAccount(sharerIndex, shareeAliceIndex, "Sharer inviting Alice"))
        << "Failure inviting Alice";
    ASSERT_NO_FATAL_FAILURE(inviteTestAccount(sharerIndex, shareeBobIndex, "Sharer inviting Bob"))
        << "Failure inviting Bob";

    if (gManualVerification)
    {
        ASSERT_NO_FATAL_FAILURE(verifyContactCredentials(sharerIndex, shareeAliceIndex));
        ASSERT_NO_FATAL_FAILURE(verifyContactCredentials(sharerIndex, shareeBobIndex));
    }

    LOG_info << logPre << "Share folder \"folderA\" to Alice and subfolder \"folderB\" to Bob.";
    auto sharerFolderANode = getNodeByPath(FOLDER_A);
    auto sharerFolderBNode = getNodeByPath(string(FOLDER_A) + "/" + FOLDER_B);
    ASSERT_TRUE(sharerFolderANode) << "folder \"folderA\" not found.";
    ASSERT_TRUE(sharerFolderBNode) << "folder \"folderB\" not found.";

    auto shareToAlice = [&]()
    {
        ASSERT_NO_FATAL_FAILURE(createShareAtoB(sharerFolderANode.get(),
                                                {sharerIndex, true},
                                                {shareeAliceIndex, true},
                                                MegaShare::ACCESS_FULL));
    };
    auto shareToBob = [&]()
    {
        ASSERT_NO_FATAL_FAILURE(createShareAtoB(sharerFolderBNode.get(),
                                                {sharerIndex, true},
                                                {shareeBobIndex, true},
                                                MegaShare::ACCESS_FULL));
    };

    // Order changed by param to exercise cases of shares avobe and below.
    if (shareOrder == ShareOrder::Forward)
    {
        shareToAlice();
        shareToBob();
    }
    else
    {
        shareToBob();
        shareToAlice();
    }

    LOG_info << logPre
             << "Ensure that the sharer, Alice and Bob can see the same nodes and that the tree is "
                "decrypted.";
    waitForNodeToBeDecrypted(shareeAliceIndex, sharerFolderANode->getHandle());
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderANode->getHandle(), sharerIndex, shareeAliceIndex));
    waitForNodeToBeDecrypted(shareeBobIndex, sharerFolderBNode->getHandle());
    ASSERT_NO_FATAL_FAILURE(matchTree(sharerFolderBNode->getHandle(), sharerIndex, shareeBobIndex));
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderBNode->getHandle(), shareeAliceIndex, shareeBobIndex));

    LOG_info
        << logPre
        << "Bob puts a file in the inshare folder. Check if Alice and the sharer can see the node.";
    auto shareeBobFolderBNode = std::unique_ptr<MegaNode>{
        megaApi[shareeBobIndex]->getNodeByHandle(sharerFolderBNode->getHandle())};
    ASSERT_TRUE(shareeBobFolderBNode);
    MegaHandle fromBobInFolderBHandle = INVALID_HANDLE;
    ASSERT_NO_FATAL_FAILURE(createRemoteFileNode(shareeBobIndex,
                                                 FileNodeInfo("fromBobInFolderB").setSize(100),
                                                 shareeBobFolderBNode.get(),
                                                 fromBobInFolderBHandle,
                                                 sharerIndex));

    waitForNodeToBeDecrypted(sharerIndex, fromBobInFolderBHandle);
    ASSERT_NO_FATAL_FAILURE(matchTree(sharerFolderBNode->getHandle(), sharerIndex, shareeBobIndex));
    waitForNodeToBeDecrypted(shareeAliceIndex, fromBobInFolderBHandle);
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderBNode->getHandle(), shareeAliceIndex, shareeBobIndex));

    LOG_info << logPre
             << "Alice puts a file in the nested inshare folder. Check if Bob and the sharer can "
                "see the node.";
    auto shareeAliceFolderBNode = std::unique_ptr<MegaNode>{
        megaApi[shareeAliceIndex]->getNodeByHandle(sharerFolderBNode->getHandle())};
    ASSERT_TRUE(shareeAliceFolderBNode);
    MegaHandle fromAliceInFolderBHandle = INVALID_HANDLE;
    ASSERT_NO_FATAL_FAILURE(createRemoteFileNode(shareeAliceIndex,
                                                 FileNodeInfo("fromAliceInFolderB").setSize(100),
                                                 shareeAliceFolderBNode.get(),
                                                 fromAliceInFolderBHandle,
                                                 sharerIndex));

    waitForNodeToBeDecrypted(sharerIndex, fromAliceInFolderBHandle);
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderBNode->getHandle(), sharerIndex, shareeAliceIndex));
    waitForNodeToBeDecrypted(shareeBobIndex, fromAliceInFolderBHandle);
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderBNode->getHandle(), shareeAliceIndex, shareeBobIndex));

    LOG_info << logPre << "Logout and login to ensure that all is correct after fetching nodes.";
    ASSERT_NO_FATAL_FAILURE(reloginAllAccounts(true));

    LOG_info << logPre
             << "Check again that the sharer, Alice and Bob can see the same nodes and that the "
                "tree is decrypted after a fresh fetchnodes.";
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderANode->getHandle(), sharerIndex, shareeAliceIndex));
    ASSERT_NO_FATAL_FAILURE(matchTree(sharerFolderBNode->getHandle(), sharerIndex, shareeBobIndex));
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderBNode->getHandle(), shareeBobIndex, shareeAliceIndex));
}

INSTANTIATE_TEST_SUITE_P(
    UploadFilesInNestedShare,
    SdkTestShareNested,
    ::testing::Values(static_cast<int>(SdkTestShareNested::ShareOrder::Forward),
                      static_cast<int>(SdkTestShareNested::ShareOrder::Reverse)));

/**
 * @brief Verify sync state transitions for nested shares.
 *
 * Steps:
 *  1. sharer uses existing folders "folderA/folderB"
 *  2. sharer shares "folderA" with shareeAlice as FULL_ACCESS
 *  3. shareeAlice creates a sync on "folderA"
 *  4. sharer shares "folderB" with shareeAlice as FULL_ACCESS
 *  5. sharer unshares "folderB" from shareeAlice
 *  6. sharer shares "folderB" with shareeAlice as READ_ACCESS
 *
 * Result:
 *  - sync stays RUNNING after steps 4 and 5
 *  - sync becomes SUSPENDED with SHARE_NON_FULL_ACCESS after step 6
 */
TEST_F(SdkTestShareNested, SyncStateWithNestedShareFolders)
{
    const auto logPre = getLogPrefix();
    LOG_info << "Starting body of " << logPre;

    // Enable manual verification mode
    megaApi[sharerIndex]->setManualVerificationFlag(true);
    megaApi[shareeAliceIndex]->setManualVerificationFlag(true);

    ASSERT_NO_FATAL_FAILURE(
        inviteTestAccount(sharerIndex, shareeAliceIndex, "Sharer inviting Alice"))
        << "Failure inviting Alice";
    ASSERT_NO_FATAL_FAILURE(verifyContactCredentials(sharerIndex, shareeAliceIndex));

    LOG_info << logPre << "1) Use existing fixture folders folderA/folderB";
    auto parentFolder = getNodeByPath(FOLDER_A);
    ASSERT_TRUE(parentFolder) << "folder \"folderA\" not found.";
    auto childFolder = getNodeByPath(std::string(FOLDER_A) + "/" + FOLDER_B);
    ASSERT_TRUE(childFolder) << "folder \"folderB\" not found.";

    LOG_info << logPre << "2) Share folderA to shareeAlice with FULL access";
    ASSERT_NO_FATAL_FAILURE(createShareAtoB(parentFolder.get(),
                                            {sharerIndex, true},
                                            {shareeAliceIndex, true},
                                            MegaShare::ACCESS_FULL));

    LOG_info << logPre << "3) Create sync on shareeAlice for folderA";
    fs::path localBasePath = makeNewTestRoot();
    fs::path localSyncPath = localBasePath / "folderA_sync";
    fs::create_directories(localSyncPath);

    MegaHandle newSyncRootNodeHandle = UNDEF;
    int err = synchronousSyncFolder(shareeAliceIndex,
                                    &newSyncRootNodeHandle,
                                    MegaSync::TYPE_TWOWAY,
                                    path_u8string(localSyncPath).c_str(),
                                    nullptr,
                                    parentFolder->getHandle(),
                                    nullptr);
    ASSERT_EQ(err, API_OK);

    std::unique_ptr<MegaNode> shareeParentFolder{
        megaApi[shareeAliceIndex]->getNodeByHandle(parentFolder->getHandle())};
    ASSERT_TRUE(shareeParentFolder);

    std::unique_ptr<MegaSync> sync = waitForSyncState(megaApi[shareeAliceIndex].get(),
                                                      shareeParentFolder.get(),
                                                      MegaSync::RUNSTATE_RUNNING,
                                                      MegaSync::NO_SYNC_ERROR);
    ASSERT_TRUE(sync);
    ASSERT_EQ(sync->getRunState(), MegaSync::RUNSTATE_RUNNING);
    MegaHandle backupId = sync->getBackupId();

    LOG_info << logPre << "4) Share folderB to shareeAlice with FULL access";
    ASSERT_NO_FATAL_FAILURE(createShareAtoB(childFolder.get(),
                                            {sharerIndex, true},
                                            {shareeAliceIndex, true},
                                            MegaShare::ACCESS_FULL));

    // Sync should remain RUNNING.
    sync = waitForSyncState(megaApi[shareeAliceIndex].get(),
                            backupId,
                            MegaSync::RUNSTATE_RUNNING,
                            MegaSync::NO_SYNC_ERROR);
    ASSERT_TRUE(sync);
    ASSERT_EQ(sync->getRunState(), MegaSync::RUNSTATE_RUNNING);

    LOG_info << logPre << "5) Unshare folderB from shareeAlice";
    ASSERT_NO_FATAL_FAILURE(shareFolder(childFolder.get(),
                                        mApi[shareeAliceIndex].email.c_str(),
                                        MegaShare::ACCESS_UNKNOWN,
                                        sharerIndex));

    // Sync should remain RUNNING.
    sync = waitForSyncState(megaApi[shareeAliceIndex].get(),
                            backupId,
                            MegaSync::RUNSTATE_RUNNING,
                            MegaSync::NO_SYNC_ERROR);
    ASSERT_TRUE(sync);
    ASSERT_EQ(sync->getRunState(), MegaSync::RUNSTATE_RUNNING);
    ASSERT_EQ(sync->getError(), MegaSync::NO_SYNC_ERROR);

    LOG_info << logPre << "6) Share folderB to shareeAlice with READ access";
    ASSERT_NO_FATAL_FAILURE(createShareAtoB(childFolder.get(),
                                            {sharerIndex, true},
                                            {shareeAliceIndex, true},
                                            MegaShare::ACCESS_READ));

    // Sync should become SUSPENDED due to non-full access in nested share.
    sync = waitForSyncState(megaApi[shareeAliceIndex].get(),
                            backupId,
                            MegaSync::RUNSTATE_SUSPENDED,
                            MegaSync::SHARE_NON_FULL_ACCESS);
    ASSERT_TRUE(sync);
    ASSERT_EQ(sync->getRunState(), MegaSync::RUNSTATE_SUSPENDED);
    ASSERT_EQ(sync->getError(), MegaSync::SHARE_NON_FULL_ACCESS);

    LOG_info << logPre << "Cleanup: remove sync";
    ASSERT_EQ(API_OK, synchronousRemoveSync(shareeAliceIndex, backupId));
}

/**
 * @brief Test the in/outshare count when a nested share is involved.
 *
 * It test if the functions related to get the list of inshares and outshares work correctly when a
 * nested share is involved. A folder public link is also created to ensure that all kind of shares
 * are working as expected.
 */
TEST_F(SdkTestShareNested, ShareCount)
{
    const auto checkShareCounters = [this]()
    {
        unique_ptr<MegaShareList> shareList;
        unique_ptr<MegaNodeList> nodeList;
        // Check sharer int/outshares count.
        shareList.reset(megaApi[sharerIndex]->getOutShares());
        ASSERT_EQ(shareList->size(), 2);
        shareList.reset(megaApi[sharerIndex]->getInSharesList());
        ASSERT_EQ(shareList->size(), 0);
        shareList.reset(megaApi[sharerIndex]->getPendingOutShares());
        ASSERT_EQ(shareList->size(), 0);
        shareList.reset(megaApi[sharerIndex]->getUnverifiedInShares());
        ASSERT_EQ(shareList->size(), 0);
        shareList.reset(megaApi[sharerIndex]->getUnverifiedOutShares());
        ASSERT_EQ(shareList->size(), 0);
        nodeList.reset(megaApi[sharerIndex]->getPublicLinks());
        ASSERT_EQ(nodeList->size(), 1);

        // Check Alice int/outshares count.
        shareList.reset(megaApi[shareeAliceIndex]->getOutShares());
        ASSERT_EQ(shareList->size(), 0);
        shareList.reset(megaApi[shareeAliceIndex]->getInSharesList());
        ASSERT_EQ(shareList->size(), 1);
        shareList.reset(megaApi[shareeAliceIndex]->getPendingOutShares());
        ASSERT_EQ(shareList->size(), 0);
        shareList.reset(megaApi[shareeAliceIndex]->getUnverifiedInShares());
        ASSERT_EQ(shareList->size(), 0);
        shareList.reset(megaApi[shareeAliceIndex]->getUnverifiedOutShares());
        ASSERT_EQ(shareList->size(), 0);
        nodeList.reset(megaApi[shareeAliceIndex]->getPublicLinks());
        ASSERT_EQ(nodeList->size(), 0);

        // Check Bob int/outshares count.
        shareList.reset(megaApi[shareeBobIndex]->getOutShares());
        ASSERT_EQ(shareList->size(), 0);
        shareList.reset(megaApi[shareeBobIndex]->getInSharesList());
        ASSERT_EQ(shareList->size(), 1);
        shareList.reset(megaApi[shareeBobIndex]->getPendingOutShares());
        ASSERT_EQ(shareList->size(), 0);
        shareList.reset(megaApi[shareeBobIndex]->getUnverifiedInShares());
        ASSERT_EQ(shareList->size(), 0);
        shareList.reset(megaApi[shareeBobIndex]->getUnverifiedOutShares());
        ASSERT_EQ(shareList->size(), 0);
        nodeList.reset(megaApi[shareeBobIndex]->getPublicLinks());
        ASSERT_EQ(nodeList->size(), 0);
    };

    CASE_info << " start.";

    // Make sharer and sharees contacts.
    ASSERT_NO_FATAL_FAILURE(
        inviteTestAccount(sharerIndex, shareeAliceIndex, "Sharer inviting Alice"))
        << "Failure inviting Alice";
    ASSERT_NO_FATAL_FAILURE(inviteTestAccount(sharerIndex, shareeBobIndex, "Sharer inviting Bob"))
        << "Failure inviting Bob";

    if (gManualVerification)
    {
        ASSERT_NO_FATAL_FAILURE(verifyContactCredentials(sharerIndex, shareeAliceIndex));
        ASSERT_NO_FATAL_FAILURE(verifyContactCredentials(sharerIndex, shareeBobIndex));
    }

    CASE_info << "Share folder \"folderA\" to Alice and subfolder \"folderB\" to Bob.";
    auto sharerFolderANode = getNodeByPath(FOLDER_A);
    auto sharerFolderBNode = getNodeByPath(string(FOLDER_A) + "/" + FOLDER_B);
    ASSERT_TRUE(sharerFolderANode) << "folder \"folderA\" not found.";
    ASSERT_TRUE(sharerFolderBNode) << "folder \"folderB\" not found.";
    ASSERT_NO_FATAL_FAILURE(createShareAtoB(sharerFolderANode.get(),
                                            {sharerIndex, true},
                                            {shareeAliceIndex, true},
                                            MegaShare::ACCESS_FULL));
    ASSERT_NO_FATAL_FAILURE(createShareAtoB(sharerFolderBNode.get(),
                                            {sharerIndex, true},
                                            {shareeBobIndex, true},
                                            MegaShare::ACCESS_FULL));

    CASE_info << "Create a folder public link in the sharer.";
    auto sharerExportedFolderNode = getNodeByPath(FOLDER_EXP);
    ASSERT_TRUE(sharerExportedFolderNode) << "folder \"exportedFolder\" not found.";
    string returnedLink = createPublicLink(sharerIndex,
                                           sharerExportedFolderNode.get(),
                                           0,
                                           maxTimeout,
                                           mApi[sharerIndex].accountDetails->getProLevel() == 0);
    sharerExportedFolderNode = getNodeByPath(FOLDER_EXP);
    ASSERT_TRUE(sharerExportedFolderNode) << "Exported node is no longer present.";
    ASSERT_TRUE(sharerExportedFolderNode->isExported()) << "Failure exporting the folder.";
    std::unique_ptr<char[]> folderPublicLink{sharerExportedFolderNode->getPublicLink()};
    ASSERT_STREQ(returnedLink.c_str(), folderPublicLink.get());

    waitForNodeToBeDecrypted(shareeAliceIndex, sharerFolderANode->getHandle());
    waitForNodeToBeDecrypted(shareeBobIndex, sharerFolderBNode->getHandle());

    CASE_info << "Checking in/out shares count when there is a nested share.";
    ASSERT_NO_FATAL_FAILURE(checkShareCounters());

    CASE_info << "Logout and login to ensure that all counters are ok after a fresh login.";
    ASSERT_NO_FATAL_FAILURE(reloginAllAccounts());
    ASSERT_NO_FATAL_FAILURE(checkShareCounters());
}

/**
 * @brief Upload nodes to inshares after a fresh login.
 *
 * It checks if after a fresh login the information from the fetchnodes
 * and if the SDK behaves correctly and send the needed information for
 * all the peers.
 *
 */
TEST_F(SdkTestShareNested, FullTreeUploads)
{
    const auto logPre = getLogPrefix();

    LOG_info << "Starting body of " << logPre;

    // Make sharer and sharees contacts.
    ASSERT_NO_FATAL_FAILURE(
        inviteTestAccount(sharerIndex, shareeAliceIndex, "Sharer inviting Alice"))
        << "Failure inviting Alice";
    ASSERT_NO_FATAL_FAILURE(inviteTestAccount(sharerIndex, shareeBobIndex, "Sharer inviting Bob"))
        << "Failure inviting Bob";

    if (gManualVerification)
    {
        ASSERT_NO_FATAL_FAILURE(verifyContactCredentials(sharerIndex, shareeAliceIndex));
        ASSERT_NO_FATAL_FAILURE(verifyContactCredentials(sharerIndex, shareeBobIndex));
    }

    LOG_info << logPre << "Share folder \"folderA\" to Alice and subfolder \"folderB\" to Bob";
    auto sharerFolderANode = getNodeByPath(FOLDER_A);
    auto sharerFolderBNode = getNodeByPath(string(FOLDER_A) + "/" + FOLDER_B);
    ASSERT_TRUE(sharerFolderANode) << "folder \"folderA\" not found.";
    ASSERT_TRUE(sharerFolderBNode) << "folder \"folderB\" not found.";
    ASSERT_NO_FATAL_FAILURE(createShareAtoB(sharerFolderANode.get(),
                                            {sharerIndex, true},
                                            {shareeAliceIndex, true},
                                            MegaShare::ACCESS_FULL));
    ASSERT_NO_FATAL_FAILURE(createShareAtoB(sharerFolderBNode.get(),
                                            {sharerIndex, true},
                                            {shareeBobIndex, true},
                                            MegaShare::ACCESS_FULL));

    LOG_info << logPre
             << "Ensure that the sharer, Alice and Bob can see the same nodes and that the tree is "
                "decrypted.";
    waitForNodeToBeDecrypted(shareeAliceIndex, sharerFolderANode->getHandle());
    waitForNodeToBeDecrypted(shareeBobIndex, sharerFolderBNode->getHandle());

    LOG_info << logPre << "Logout and login to ensure that all is correct after fetching nodes.";
    ASSERT_NO_FATAL_FAILURE(reloginAllAccounts(true));

    LOG_info << logPre << "Check that the sharer, Alice and Bob can see the same nodes";
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderANode->getHandle(), sharerIndex, shareeAliceIndex));
    ASSERT_NO_FATAL_FAILURE(matchTree(sharerFolderBNode->getHandle(), sharerIndex, shareeBobIndex));
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderBNode->getHandle(), shareeAliceIndex, shareeBobIndex));

    LOG_info << logPre << "Bob puts a file in the inshare folder.";
    auto shareeBobFolderBNode = std::unique_ptr<MegaNode>{
        megaApi[shareeBobIndex]->getNodeByHandle(sharerFolderBNode->getHandle())};
    ASSERT_TRUE(shareeBobFolderBNode);
    MegaHandle fromBobInFolderBHandle = INVALID_HANDLE;
    ASSERT_NO_FATAL_FAILURE(createRemoteFileNode(shareeBobIndex,
                                                 FileNodeInfo("fromBobInFolderB").setSize(100),
                                                 shareeBobFolderBNode.get(),
                                                 fromBobInFolderBHandle,
                                                 sharerIndex));

    LOG_info << logPre
             << "Alice puts a file in the inshare folder and in the nested inshare folder.";
    auto shareeAliceFolderBNode = std::unique_ptr<MegaNode>{
        megaApi[shareeAliceIndex]->getNodeByHandle(sharerFolderBNode->getHandle())};
    ASSERT_TRUE(shareeAliceFolderBNode);
    MegaHandle fromAliceInFolderBHandle = INVALID_HANDLE;
    ASSERT_NO_FATAL_FAILURE(createRemoteFileNode(shareeAliceIndex,
                                                 FileNodeInfo("fromAliceInFolderB").setSize(100),
                                                 shareeAliceFolderBNode.get(),
                                                 fromAliceInFolderBHandle,
                                                 sharerIndex));
    auto shareeAliceFolderANode = std::unique_ptr<MegaNode>{
        megaApi[shareeAliceIndex]->getNodeByHandle(sharerFolderANode->getHandle())};
    ASSERT_TRUE(shareeAliceFolderANode);
    MegaHandle fromAliceInFolderAHandle = INVALID_HANDLE;
    ASSERT_NO_FATAL_FAILURE(createRemoteFileNode(shareeAliceIndex,
                                                 FileNodeInfo("fromAliceInFolderA").setSize(100),
                                                 shareeAliceFolderANode.get(),
                                                 fromAliceInFolderAHandle,
                                                 sharerIndex));
    LOG_info << logPre
             << "Ensure that the sharer, Alice and Bob can see the same nodes and that the tree is "
                "decrypted.";

    // Wait for Bob file
    waitForNodeToBeDecrypted(sharerIndex, fromBobInFolderBHandle);
    waitForNodeToBeDecrypted(shareeAliceIndex, fromBobInFolderBHandle);

    // Wait for Alice files
    waitForNodeToBeDecrypted(sharerIndex, fromAliceInFolderBHandle);
    waitForNodeToBeDecrypted(shareeBobIndex, fromAliceInFolderBHandle);
    waitForNodeToBeDecrypted(sharerIndex, fromAliceInFolderAHandle);

    // Check the complete tree
    ASSERT_NO_FATAL_FAILURE(matchTree(sharerFolderBNode->getHandle(), sharerIndex, shareeBobIndex));
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderBNode->getHandle(), shareeAliceIndex, shareeBobIndex));
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderANode->getHandle(), sharerIndex, shareeAliceIndex));
}

/**
 * @brief Check for correctness if the higher level sharee is unverified.
 *
 * It tests whether files uploaded by a new sharee can be decrypted by another sharee whose
 * higher-level share was created earlier but is still unverified.
 */
TEST_F(SdkTestShareNested, UnverifiedHigherLevelSharee)
{
    const auto logPre = getLogPrefix();

    LOG_info << "Starting body of " << logPre;

    megaApi[sharerIndex]->setManualVerificationFlag(true);
    megaApi[shareeAliceIndex]->setManualVerificationFlag(true);
    megaApi[shareeBobIndex]->setManualVerificationFlag(true);

    ASSERT_NO_FATAL_FAILURE(resetCredentialsIfContactFound(sharerIndex, shareeAliceIndex));
    ASSERT_NO_FATAL_FAILURE(resetCredentialsIfContactFound(sharerIndex, shareeBobIndex));

    // Make sharer and sharees contacts.
    ASSERT_NO_FATAL_FAILURE(
        inviteTestAccount(sharerIndex, shareeAliceIndex, "Sharer inviting Alice"))
        << "Failure inviting Alice";
    ASSERT_NO_FATAL_FAILURE(inviteTestAccount(sharerIndex, shareeBobIndex, "Sharer inviting Bob"))
        << "Failure inviting Bob";

    ASSERT_NO_FATAL_FAILURE(verifyContactCredentials(sharerIndex, shareeBobIndex));

    LOG_info << logPre
             << "Share folder \"folderA\" to Alice while still unverified and subfolder "
                "\"folderB\" to Bob.";
    auto sharerFolderANode = getNodeByPath(FOLDER_A);
    auto sharerFolderBNode = getNodeByPath(string(FOLDER_A) + "/" + FOLDER_B);
    ASSERT_TRUE(sharerFolderANode) << "folder \"folderA\" not found.";
    ASSERT_TRUE(sharerFolderBNode) << "folder \"folderB\" not found.";

    ASSERT_NO_FATAL_FAILURE(createShareAtoB(sharerFolderANode.get(),
                                            {sharerIndex, true},
                                            {shareeAliceIndex, true},
                                            MegaShare::ACCESS_FULL));

    ASSERT_NO_FATAL_FAILURE(createShareAtoB(sharerFolderBNode.get(),
                                            {sharerIndex, true},
                                            {shareeBobIndex, true},
                                            MegaShare::ACCESS_FULL));

    LOG_info << logPre
             << "Ensure that Alice still has the share as unverified and that the sharer and Bob "
                "can see the same nodes and that the tree is decrypted.";

    waitForNodeToBeDecrypted(shareeBobIndex, sharerFolderBNode->getHandle());
    ASSERT_NO_FATAL_FAILURE(matchTree(sharerFolderBNode->getHandle(), sharerIndex, shareeBobIndex));
    ASSERT_TRUE(WaitFor(
        [this]()
        {
            return unique_ptr<MegaShareList>(megaApi[sharerIndex]->getUnverifiedOutShares())
                       ->size() == 1;
        },
        60 * 1000));

    {
        auto shareeAliceFolderANode = std::unique_ptr<MegaNode>{
            megaApi[shareeAliceIndex]->getNodeByHandle(sharerFolderANode->getHandle())};
        ASSERT_TRUE(shareeAliceFolderANode);
        ASSERT_FALSE(shareeAliceFolderANode->isNodeKeyDecrypted())
            << "Alice should not decrypt nested file before verification.";
    }

    LOG_info << logPre << "Bob puts a file in the inshare folder.";
    auto shareeBobFolderBNode = std::unique_ptr<MegaNode>{
        megaApi[shareeBobIndex]->getNodeByHandle(sharerFolderBNode->getHandle())};
    ASSERT_TRUE(shareeBobFolderBNode);
    MegaHandle fromBobInFolderBHandle = INVALID_HANDLE;
    ASSERT_NO_FATAL_FAILURE(
        createRemoteFileNode(shareeBobIndex,
                             FileNodeInfo("fromBobInFolderBWhileAliceUnverified").setSize(100),
                             shareeBobFolderBNode.get(),
                             fromBobInFolderBHandle,
                             sharerIndex));

    waitForNodeToBeDecrypted(sharerIndex, fromBobInFolderBHandle);
    ASSERT_NO_FATAL_FAILURE(matchTree(sharerFolderBNode->getHandle(), sharerIndex, shareeBobIndex));
    {
        std::unique_ptr<MegaNode> aliceNodefromBobInFolderB{
            megaApi[shareeAliceIndex]->getNodeByHandle(fromBobInFolderBHandle)};
        ASSERT_TRUE(aliceNodefromBobInFolderB);
        ASSERT_FALSE(aliceNodefromBobInFolderB->isNodeKeyDecrypted())
            << "Alice should not decrypt nested file before verification.";
    }

    LOG_info << logPre << "Verify Alice and ensure the missing nested key is repaired.";
    ASSERT_NO_FATAL_FAILURE(verifyContactCredentials(sharerIndex, shareeAliceIndex));
    waitForNodeToBeDecrypted(shareeAliceIndex, sharerFolderANode->getHandle());
    waitForNodeToBeDecrypted(shareeAliceIndex, fromBobInFolderBHandle);
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderANode->getHandle(), sharerIndex, shareeAliceIndex));
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderBNode->getHandle(), shareeAliceIndex, shareeBobIndex));
}

/**
 * @brief Check for correctness if the lower level sharee is unverified.
 *
 * It tests whether files uploaded by a new sharee can be decrypted by another sharee whose
 * lower-level share was created earlier but is still unverified.
 */
TEST_F(SdkTestShareNested, UnverifiedLowerLevelSharee)
{
    const auto logPre = getLogPrefix();

    LOG_info << "Starting body of " << logPre;

    megaApi[sharerIndex]->setManualVerificationFlag(true);
    megaApi[shareeAliceIndex]->setManualVerificationFlag(true);
    megaApi[shareeBobIndex]->setManualVerificationFlag(true);

    ASSERT_NO_FATAL_FAILURE(resetCredentialsIfContactFound(sharerIndex, shareeAliceIndex));
    ASSERT_NO_FATAL_FAILURE(resetCredentialsIfContactFound(sharerIndex, shareeBobIndex));

    // Make sharer and sharees contacts.
    ASSERT_NO_FATAL_FAILURE(
        inviteTestAccount(sharerIndex, shareeAliceIndex, "Sharer inviting Alice"))
        << "Failure inviting Alice";
    ASSERT_NO_FATAL_FAILURE(inviteTestAccount(sharerIndex, shareeBobIndex, "Sharer inviting Bob"))
        << "Failure inviting Bob";

    ASSERT_NO_FATAL_FAILURE(verifyContactCredentials(sharerIndex, shareeAliceIndex));

    LOG_info << logPre
             << "Share folder \"folderB\" to Bob while still unverified and folder \"folderA\" to "
                "Alice.";
    auto sharerFolderANode = getNodeByPath(FOLDER_A);
    auto sharerFolderBNode = getNodeByPath(string(FOLDER_A) + "/" + FOLDER_B);
    ASSERT_TRUE(sharerFolderANode) << "folder \"folderA\" not found.";
    ASSERT_TRUE(sharerFolderBNode) << "folder \"folderB\" not found.";
    ASSERT_NO_FATAL_FAILURE(createShareAtoB(sharerFolderBNode.get(),
                                            {sharerIndex, true},
                                            {shareeBobIndex, true},
                                            MegaShare::ACCESS_FULL));
    ASSERT_NO_FATAL_FAILURE(createShareAtoB(sharerFolderANode.get(),
                                            {sharerIndex, true},
                                            {shareeAliceIndex, true},
                                            MegaShare::ACCESS_FULL));

    LOG_info << logPre
             << "Ensure that Bob still has the share as unverified and that the sharer and Alice "
                "can see the same nodes and that the tree is decrypted.";

    waitForNodeToBeDecrypted(shareeAliceIndex, sharerFolderANode->getHandle());
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderANode->getHandle(), sharerIndex, shareeAliceIndex));
    ASSERT_TRUE(WaitFor(
        [this]()
        {
            return unique_ptr<MegaShareList>(megaApi[sharerIndex]->getUnverifiedOutShares())
                       ->size() == 1;
        },
        60 * 1000));

    {
        auto shareeBobFolderBNode = std::unique_ptr<MegaNode>{
            megaApi[shareeBobIndex]->getNodeByHandle(sharerFolderBNode->getHandle())};
        ASSERT_TRUE(shareeBobFolderBNode);
        ASSERT_FALSE(shareeBobFolderBNode->isNodeKeyDecrypted())
            << "Bob should not decrypt nested file before verification.";
    }

    auto shareeAliceFolderBNode = std::unique_ptr<MegaNode>{
        megaApi[shareeAliceIndex]->getNodeByHandle(sharerFolderBNode->getHandle())};
    ASSERT_TRUE(shareeAliceFolderBNode);

    LOG_info << logPre << "Alice uploads a file to nested folderB.";
    MegaHandle fromAliceInFolderBHandle = INVALID_HANDLE;
    ASSERT_NO_FATAL_FAILURE(
        createRemoteFileNode(shareeAliceIndex,
                             FileNodeInfo("fromAliceWhileBobUnverified").setSize(100),
                             shareeAliceFolderBNode.get(),
                             fromAliceInFolderBHandle,
                             sharerIndex));

    waitForNodeToBeDecrypted(sharerIndex, fromAliceInFolderBHandle);
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderBNode->getHandle(), sharerIndex, shareeAliceIndex));
    {
        std::unique_ptr<MegaNode> bobNode{
            megaApi[shareeBobIndex]->getNodeByHandle(fromAliceInFolderBHandle)};
        ASSERT_TRUE(bobNode);
        ASSERT_FALSE(bobNode->isNodeKeyDecrypted())
            << "Bob should not decrypt nested file before verification.";
    }

    LOG_info << logPre << "Verify Bob and ensure the missing nested key is repaired.";
    ASSERT_NO_FATAL_FAILURE(verifyContactCredentials(sharerIndex, shareeBobIndex));
    waitForNodeToBeDecrypted(shareeBobIndex, sharerFolderBNode->getHandle());
    waitForNodeToBeDecrypted(shareeBobIndex, fromAliceInFolderBHandle);
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderANode->getHandle(), sharerIndex, shareeAliceIndex));
    ASSERT_NO_FATAL_FAILURE(
        matchTree(sharerFolderBNode->getHandle(), shareeAliceIndex, shareeBobIndex));
}
