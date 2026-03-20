#include "sdk_test_shares.h"

#include "env_var_accounts.h"

void SdkTestShares::SetUp()
{
    SdkTest::SetUp();

    // Accounts for sharer and sharee
    ASSERT_NO_FATAL_FAILURE(getAccountsForTest(2));

    // Guest for accessing the public link, No login in SetUp
    const auto [email, pass] = getEnvVarAccounts().getVarValues(mGuestIndex);
    ASSERT_FALSE(email.empty() || pass.empty());
    mApi.resize(mGuestIndex + 1);
    megaApi.resize(mGuestIndex + 1);
    configureTestInstance(mGuestIndex, email, pass);

    // Convenience
    mSharer = &mApi[mSharerIndex];
    mSharee = &mApi[mShareeIndex];
    mGuest = &mApi[mGuestIndex];
    mSharerApi = megaApi[mSharerIndex].get();
    mShareeApi = megaApi[mShareeIndex].get();
    mGuestApi = megaApi[mGuestIndex].get();
    mGuestEmail = email;
    mGuestPass = pass;
}

void SdkTestShares::TearDown()
{
    SdkTest::TearDown();
}

MegaHandle SdkTestShares::getHandle(const std::string& path) const
{
    return mHandles.at(path);
}

void SdkTestShares::verifyCredentials(unsigned sharerIndex,
                                      const PerApi* sharer,
                                      unsigned shareeIndex,
                                      const PerApi* sharee)
{
    if (!gManualVerification)
        return;

    if (!areCredentialsVerified(sharerIndex, sharee->email))
    {
        ASSERT_NO_FATAL_FAILURE(SdkTest::verifyCredentials(sharerIndex, sharee->email));
    }

    if (!areCredentialsVerified(shareeIndex, sharer->email))
    {
        ASSERT_NO_FATAL_FAILURE(SdkTest::verifyCredentials(shareeIndex, sharer->email));
    }
}

void SdkTestShares::createNewContactAndVerify()
{
    // Invite
    const string message = "Hi contact. Let's share some stuff";
    mSharee->contactRequestUpdated = false;
    ASSERT_NO_FATAL_FAILURE(inviteContact(mSharerIndex,
                                          mSharee->email,
                                          message,
                                          MegaContactRequest::INVITE_ACTION_ADD));
    ASSERT_TRUE(waitForResponse(&mSharee->contactRequestUpdated, 10u))
        << "Contact request creation not received by the sharee after 10 seconds";

    // Get the the contact request
    ASSERT_NO_FATAL_FAILURE(getContactRequest(mShareeIndex, false));

    // Accept the request
    mSharer->contactRequestUpdated = false;
    mSharer->contactRequestUpdated = false;
    ASSERT_NO_FATAL_FAILURE(
        replyContact(mSharee->cr.get(), MegaContactRequest::REPLY_ACTION_ACCEPT));
    ASSERT_TRUE(waitForResponse(&mSharee->contactRequestUpdated, 10u))
        << "Contact request creation not received by the sharee after 10 seconds";
    ASSERT_TRUE(waitForResponse(&mSharer->contactRequestUpdated, 10u))
        << "Contact request creation not received by the sharer after 10 seconds";
    mSharer->cr.reset();

    // Verify credential
    ASSERT_NO_FATAL_FAILURE(verifyCredentials(mSharerIndex, mSharer, mShareeIndex, mSharee));
}

void SdkTestShares::createOutgoingShare(MegaHandle hfolder)
{
    std::unique_ptr<MegaNode> node{mSharerApi->getNodeByHandle(hfolder)};
    ASSERT_TRUE(node);

    // Create a new outgoing share
    bool inshareCheck = false;
    bool outshareCheck = false;
    mSharer->mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(hfolder, MegaNode::CHANGE_TYPE_OUTSHARE, outshareCheck);
    mSharee->mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(hfolder, MegaNode::CHANGE_TYPE_INSHARE, inshareCheck);
    ASSERT_NO_FATAL_FAILURE(
        shareFolder(node.get(), mSharee->email.c_str(), MegaShare::ACCESS_FULL));
    ASSERT_TRUE(waitForResponse(&outshareCheck))
        << "Node update not received by the sharer after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&inshareCheck))
        << "Node update not received by the sharee after " << maxTimeout << " seconds";
    resetOnNodeUpdateCompletionCBs(); // Important to reset
    ASSERT_TRUE(outshareCheck);
    ASSERT_TRUE(inshareCheck);

    // Check the outgoing share
    const std::unique_ptr<MegaShareList> shareList{mSharerApi->getOutShares()};
    ASSERT_EQ(1, shareList->size()) << "Outgoing share failed";
    const auto share = shareList->get(0);
    ASSERT_EQ(MegaShare::ACCESS_FULL, share->getAccess()) << "Wrong access level of outgoing share";
    ASSERT_EQ(hfolder, share->getNodeHandle()) << "Wrong node handle of outgoing share";
    ASSERT_STRCASEEQ(mSharee->email.c_str(), share->getUser())
        << "Wrong email address of outgoing share";

    // Get an updated version of the node
    node.reset(mSharerApi->getNodeByHandle(hfolder));
    ASSERT_TRUE(node->isShared()) << "Wrong sharing information at outgoing share";
    ASSERT_TRUE(node->isOutShare()) << "Wrong sharing information at outgoing share";

    int accessLevel = mSharerApi->getAccess(hfolder);
    ASSERT_EQ(accessLevel, MegaShare::ACCESS_OWNER)
        << "Wrong access level for the shared folder handle";
    accessLevel = mSharerApi->getAccess(node.get());
    ASSERT_EQ(accessLevel, MegaShare::ACCESS_OWNER)
        << "Wrong access level for the shared folder node";
}

// Get and Check only one incoming share
void SdkTestShares::getInshare(MegaHandle hfolder)
{
    const std::unique_ptr<MegaShareList> shareList{megaApi[1]->getInSharesList()};
    ASSERT_EQ(1, shareList->size()) << "Incoming share not received in auxiliar account";

    // Wait for the inshare node to be decrypted
    auto descryptedPred = [this, hfolder]()
    {
        return std::unique_ptr<MegaNode>(mShareeApi->getNodeByHandle(hfolder))
            ->isNodeKeyDecrypted();
    };
    ASSERT_TRUE(WaitFor(descryptedPred, 60 * 1000));

    const std::unique_ptr<MegaUser> contact{mShareeApi->getContact(mSharer->email.c_str())};
    const std::unique_ptr<MegaNodeList> inshareNodes{mShareeApi->getInShares(contact.get())};
    ASSERT_EQ(1, inshareNodes->size()) << "Incoming share not received in auxiliar account";
    const auto thisInshareNode = inshareNodes->get(0);
    ASSERT_EQ(hfolder, thisInshareNode->getHandle()) << "Wrong node handle of incoming share";
    ASSERT_STREQ("sharedfolder", thisInshareNode->getName())
        << "Wrong folder name of incoming share";
    ASSERT_EQ(API_OK,
              mShareeApi->checkAccessErrorExtended(thisInshareNode, MegaShare::ACCESS_FULL)
                  ->getErrorCode())
        << "Wrong access level of incoming share";
    ASSERT_TRUE(thisInshareNode->isInShare()) << "Wrong sharing information at incoming share";
    ASSERT_TRUE(thisInshareNode->isShared()) << "Wrong sharing information at incoming share";

    int accessLevel = mShareeApi->getAccess(hfolder);
    ASSERT_EQ(accessLevel, MegaShare::ACCESS_FULL)
        << "Wrong access level for the shared folder handle";
    accessLevel = mShareeApi->getAccess(thisInshareNode);
    ASSERT_EQ(accessLevel, MegaShare::ACCESS_FULL)
        << "Wrong access level for the shared folder node";
}

void SdkTestShares::createOnePublicLink(MegaHandle hfolder, std::string& nodeLink)
{
    std::unique_ptr<MegaNode> nfolder{mSharerApi->getNodeByHandle(hfolder)};
    ASSERT_TRUE(nfolder);
    const bool isFreeAccount =
        mSharer->accountDetails->getProLevel() == MegaAccountDetails::ACCOUNT_TYPE_FREE;

    // Create a public link
    nodeLink = createPublicLink(mSharerIndex, nfolder.get(), 0, maxTimeout, isFreeAccount);

    // Get a fresh snapshot of the node and check it's actually exported
    nfolder.reset(mSharerApi->getNodeByHandle(hfolder));
    ASSERT_TRUE(nfolder);
    ASSERT_TRUE(nfolder->isExported()) << "Node is not exported, must be exported";
    ASSERT_FALSE(nfolder->isTakenDown()) << "Public link is taken down, it mustn't";
    ASSERT_STREQ(nodeLink.c_str(), std::unique_ptr<char[]>(nfolder->getPublicLink()).get())
        << "Wrong public link from MegaNode";

    // Regenerate the same link should not trigger a new request
    ASSERT_EQ(nodeLink, createPublicLink(mSharerIndex, nfolder.get(), 0, maxTimeout, isFreeAccount))
        << "Wrong public link after link update";
}

void SdkTestShares::importPublicLink(const std::string& nodeLink, MegaHandle* importedNodeHandle)
{
    // Login to the folder and fetchnodes
    auto loginFolderTracker = asyncRequestLoginToFolder(mGuestIndex, nodeLink.c_str());
    ASSERT_EQ(loginFolderTracker->waitForResult(), API_OK)
        << "Failed to login to folder " << nodeLink;
    ASSERT_NO_FATAL_FAILURE(fetchnodes(mGuestIndex));

    // Authorize the node
    std::unique_ptr<MegaNode> folderNodeToImport{mGuestApi->getRootNode()};
    ASSERT_TRUE(folderNodeToImport) << "Failed to get folder node to import from link " << nodeLink;
    std::unique_ptr<MegaNode> authorizedFolderNode{
        mGuestApi->authorizeNode(folderNodeToImport.get())};
    ASSERT_TRUE(authorizedFolderNode) << "Failed to authorize folder node from link " << nodeLink;
    ASSERT_TRUE(authorizedFolderNode->getChildren())
        << "Authorized folder node children list is null but it should not";
    ASSERT_EQ(mGuestApi->getNumChildren(folderNodeToImport.get()),
              authorizedFolderNode->getChildren()->size())
        << "Different number of child nodes after authorizing the folder node";

    // Logout the folder
    ASSERT_NO_FATAL_FAILURE(logout(mGuestIndex, false, 20));

    // Login with guest and fetch nodes
    auto loginTracker = asyncRequestLogin(mGuestIndex, mGuestEmail.c_str(), mGuestPass.c_str());
    ASSERT_EQ(loginTracker->waitForResult(), API_OK) << "Failed to login with " << mGuestEmail;
    ASSERT_NO_FATAL_FAILURE(fetchnodes(mGuestIndex));

    // Copy(import) the public folder (authorized) to the root of the account
    std::unique_ptr<MegaNode> rootNode{mGuestApi->getRootNode()};
    RequestTracker nodeCopyTracker{mGuestApi};
    mGuestApi->copyNode(authorizedFolderNode.get(), rootNode.get(), nullptr, &nodeCopyTracker);
    ASSERT_EQ(nodeCopyTracker.waitForResult(), API_OK) << "Failed to copy node to import";
    std::unique_ptr<MegaNode> importedNode{
        mGuestApi->getNodeByPath(authorizedFolderNode->getName(), rootNode.get())};
    ASSERT_TRUE(importedNode) << "Imported node not found";
    if (authorizedFolderNode->getChildren()->size())
    {
        std::unique_ptr<MegaNode> authorizedImportedNode(
            mGuestApi->authorizeNode(importedNode.get()));
        EXPECT_TRUE(authorizedImportedNode) << "Failed to authorize imported node";
        EXPECT_TRUE(authorizedImportedNode->getChildren())
            << "Authorized imported node children list is null but it should not";
        ASSERT_EQ(authorizedFolderNode->getChildren()->size(),
                  authorizedImportedNode->getChildren()->size())
            << "Not all child nodes have been imported";
    }

    if (importedNodeHandle)
        *importedNodeHandle = importedNode->getHandle();
}

// Revoke access to an outgoing shares
void SdkTestShares::revokeOutShares(MegaHandle hfolder)
{
    const std::unique_ptr<MegaNode> node{mSharerApi->getNodeByHandle(hfolder)};
    ASSERT_TRUE(node);
    bool inshareCheck = false;
    bool outshareCheck = false;
    mSharer->mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(hfolder, MegaNode::CHANGE_TYPE_OUTSHARE, outshareCheck);
    mSharee->mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(hfolder, MegaNode::CHANGE_TYPE_REMOVED, inshareCheck);
    ASSERT_NO_FATAL_FAILURE(
        shareFolder(node.get(), mSharee->email.c_str(), MegaShare::ACCESS_UNKNOWN));
    ASSERT_TRUE(waitForResponse(&outshareCheck)) // at the target side (main account)
        << "Node update not received by the sharer after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&inshareCheck)) // at the target side (auxiliar account)
        << "Node update not received by the sharee after " << maxTimeout << " seconds";

    // important to reset
    resetOnNodeUpdateCompletionCBs();
    ASSERT_TRUE(outshareCheck);
    ASSERT_TRUE(inshareCheck);

    const std::unique_ptr<MegaShareList> sl{mSharerApi->getOutShares()};
    ASSERT_EQ(0, sl->size()) << "Outgoing share revocation failed";
}

void SdkTestShares::revokePublicLink(MegaHandle hfolder)
{
    // Remove
    std::unique_ptr<MegaNode> node{mSharerApi->getNodeByHandle(hfolder)};
    ASSERT_TRUE(node);
    const MegaHandle removedLinkHandle = removePublicLink(mSharerIndex, node.get());

    // Get a fresh node and check
    node.reset(mSharerApi->getNodeByHandle(removedLinkHandle));
    ASSERT_TRUE(node);
    ASSERT_FALSE(node->isPublic()) << "Public link removal failed (still public)";
}

void SdkTestShares::copyNode(const unsigned int accountId,
                             const MegaHandle sourceNodeHandle,
                             const MegaHandle destNodeHandle,
                             const std::string& destName,
                             MegaHandle* copiedNodeHandle)
{
    auto& api = accountId == mShareeIndex ? mShareeApi : mSharerApi;
    std::unique_ptr<MegaNode> source = std::unique_ptr<MegaNode>(
        sourceNodeHandle == INVALID_HANDLE ? api->getRootNode() :
                                             api->getNodeByHandle(sourceNodeHandle));
    std::unique_ptr<MegaNode> dest = std::unique_ptr<MegaNode>(
        destNodeHandle == INVALID_HANDLE ? api->getRootNode() :
                                           api->getNodeByHandle(destNodeHandle));

    auto result =
        doCopyNode(accountId, copiedNodeHandle, source.get(), dest.get(), destName.c_str());
    ASSERT_EQ(result, API_OK) << "Error copying file";
    if (copiedNodeHandle)
    {
        ASSERT_NE(*copiedNodeHandle, INVALID_HANDLE)
            << "The copied file handle was not set properly";
    }
}

void SdkTestShares::moveNodeToOwnCloud(const std::string& sourceNodePath,
                                       const std::string& destNodeName,
                                       MegaHandle* movedNodeHandle)
{
    std::unique_ptr<MegaNode> source{mShareeApi->getNodeByHandle(getHandle(sourceNodePath))};
    std::unique_ptr<MegaNode> dest{mShareeApi->getRootNode()};
    auto result =
        doMoveNode(mShareeIndex, movedNodeHandle, source.get(), dest.get(), destNodeName.c_str());
    ASSERT_EQ(result, API_OK);
}

// Initialize a test scenario : create some folders/files to share
// Create some nodes to share
//  |--sharedfolder
//    |--subfolder
//      |--file.txt
//    |--file.txt
void SdkTestShares::createNodeTrees()
{
    const std::unique_ptr<MegaNode> rootnode{mSharerApi->getRootNode()};
    const MegaHandle hfolder = mHandles["/sharedfolder"] =
        createFolder(mSharerIndex, "sharedfolder", rootnode.get());
    ASSERT_NE(hfolder, UNDEF);

    const std::unique_ptr<MegaNode> node{mSharerApi->getNodeByHandle(hfolder)};
    ASSERT_TRUE(node);

    const MegaHandle subfolder = mHandles["/sharedfolder/subfolder"] =
        createFolder(mSharerIndex, "subfolder", node.get());
    ASSERT_NE(subfolder, UNDEF);

    // Create a local file
    ASSERT_TRUE(createFile("file.txt", false)) << "Couldn't create " << "file.txt";

    // Create a node /sharefolder/file.txt by uploading
    MegaHandle hfile = UNDEF;
    ASSERT_EQ(MegaError::API_OK,
              doStartUpload(mSharerIndex,
                            &hfile,
                            "file.txt",
                            node.get(),
                            nullptr /*fileName*/,
                            ::mega::MegaApi::INVALID_CUSTOM_MOD_TIME,
                            nullptr /*appData*/,
                            false /*isSourceTemporary*/,
                            false /*startFirst*/,
                            nullptr /*cancelToken*/))
        << "Cannot upload a test file";
    mHandles["/sharedfolder/file.txt"] = hfile;

    // Create a node /sharedfolder/subfolder/file.txt by uploading
    ASSERT_EQ(MegaError::API_OK,
              doStartUpload(mSharerIndex,
                            &hfile,
                            "file.txt",
                            node.get(),
                            nullptr /*fileName*/,
                            ::mega::MegaApi::INVALID_CUSTOM_MOD_TIME,
                            nullptr /*appData*/,
                            false /*isSourceTemporary*/,
                            false /*startFirst*/,
                            nullptr /*cancelToken*/))
        << "Cannot upload a second test file";
    mHandles["/sharedfolder/subfolder/file.txt"] = hfile;
}

/**
 * @brief TEST_F TestPublicFolderLinksWithShares
 *
 * 1 - create share
 * 2 - create folder link on same share
 * 3 - remove folder link
 * 4 - remove share
 * 5 - create folder link
 * 6 - remove folder link
 *
 */
TEST_F(SdkTestShares, TestPublicFolderLinksWithShares)
{
    LOG_info << "___TEST TestPublicFolderLinksWithShares";

    ASSERT_NO_FATAL_FAILURE(createNodeTrees());

    const MegaHandle hfolder = getHandle("/sharedfolder");

    // Create share on the folder
    ASSERT_NO_FATAL_FAILURE(createNewContactAndVerify());

    ASSERT_NO_FATAL_FAILURE(createOutgoingShare(hfolder));

    ASSERT_NO_FATAL_FAILURE(getInshare(hfolder));

    // Create a folder public link on the shared folder
    ASSERT_EQ(API_OK, synchronousGetSpecificAccountDetails(mSharerIndex, true, true, true))
        << "Cannot get account details";

    std::string nodeLink;
    ASSERT_NO_FATAL_FAILURE(createOnePublicLink(hfolder, nodeLink));

    ASSERT_NO_FATAL_FAILURE(importPublicLink(nodeLink));

    ASSERT_NO_FATAL_FAILURE(revokePublicLink(hfolder));

    // Revoke share on the folder
    ASSERT_NO_FATAL_FAILURE(revokeOutShares(hfolder));

    // Create the folder public link on the folder after revoking
    ASSERT_NO_FATAL_FAILURE(createOnePublicLink(hfolder, nodeLink));

    ASSERT_NO_FATAL_FAILURE(importPublicLink(nodeLink));

    ASSERT_NO_FATAL_FAILURE(revokePublicLink(hfolder));
}

/**
 * @brief TEST_F SdkTestShares.TestForeingNodeImportRemoveSensitiveFlag
 *
 * 1 - User 0 creates node tree and marks one file as sensitive
 * 2 - User 1 imports that folder via meeting link -> No sensitive expected
 * 3 - User 0 shares folder with User 1 -> User 1 sees sensitive node
 * 4 - User 1 copies to own cloud -> No sensitive in the copy
 * 5 - User 0 copies sensitive file with other name in the shared -> Copy keeps sensitive.
 * 6 - User 1 does the same -> Copy removes sensitive
 * 7 - User 1 moves to own cloud -> No sensitive expected
 * 8 - User 1 tags the moved node as sensitive and copies back to shared -> No sensitive expected
 *
 */
TEST_F(SdkTestShares, TestForeingNodeImportRemoveSensitiveFlag)
{
    const auto getSensNodes = [](const auto& api, MegaHandle handle)
    {
        std::unique_ptr<MegaSearchFilter> filter(MegaSearchFilter::createInstance());
        filter->bySensitivity(MegaSearchFilter::BOOL_FILTER_ONLY_FALSE);
        filter->byLocationHandle(handle);
        std::unique_ptr<MegaNodeList> sensNodes(api->search(filter.get()));
        return sensNodes;
    };

    LOG_info << "___TEST TestForeingNodeImportRemoveSensitiveFlag";

    LOG_debug << "## Creating node tree in user 0 cloud";
    ASSERT_NO_FATAL_FAILURE(createNodeTrees());

    LOG_debug << "## Marking node as sensitive";
    // Mark one file as sensitive
    std::unique_ptr<MegaNode> sensFile{
        mSharerApi->getNodeByHandle(getHandle("/sharedfolder/file.txt"))};
    ASSERT_EQ(API_OK, synchronousSetNodeSensitive(mSharerIndex, sensFile.get(), true));

    // We test first the share via public link to ensure we go through the code path where the node
    // to import is not already in our cloud
    LOG_debug << "## User 0 creates a public link to share";
    const MegaHandle hfolder = getHandle("/sharedfolder");
    ASSERT_EQ(API_OK, synchronousGetSpecificAccountDetails(mSharerIndex, true, true, true))
        << "Cannot get account details";
    std::string nodeLink;
    ASSERT_NO_FATAL_FAILURE(createOnePublicLink(hfolder, nodeLink));

    LOG_debug << "## User 1 imports public link";
    MegaHandle importedNodeHandle = INVALID_HANDLE;
    ASSERT_NO_FATAL_FAILURE(importPublicLink(nodeLink, &importedNodeHandle));
    ASSERT_NE(importedNodeHandle, INVALID_HANDLE);

    // Check there is no sensitive nodes in the imported node
    LOG_debug << "## Checking user 1 sees no sensitive files in the imported folder";
    std::unique_ptr<MegaNodeList> sensNodes = getSensNodes(mShareeApi, importedNodeHandle);
    EXPECT_EQ(sensNodes->size(), 0)
        << "Got sensitive nodes after importing from public link while this property is expected "
           "to be cleared in the process";

    LOG_debug << "## Sharing the folder with user 1";
    ASSERT_NO_FATAL_FAILURE(createNewContactAndVerify());
    ASSERT_NO_FATAL_FAILURE(createOutgoingShare(hfolder));
    ASSERT_NO_FATAL_FAILURE(getInshare(hfolder));

    LOG_debug << "## Checking user 1 sees a sensitive file";
    sensNodes = getSensNodes(mShareeApi, hfolder);
    ASSERT_EQ(sensNodes->size(), 1);
    ASSERT_STREQ(sensNodes->get(0)->getName(), "file.txt");

    LOG_debug << "## User 1 copies folder with sensitive file into own cloud";
    MegaHandle copyHandle = INVALID_HANDLE;
    ASSERT_NO_FATAL_FAILURE(copyNode(mShareeIndex,
                                     getHandle("/sharedfolder"),
                                     INVALID_HANDLE,
                                     "copied_shared",
                                     &copyHandle));

    LOG_debug << "## Checking user 1 sees no sensitive files in the copied node";
    sensNodes = getSensNodes(mShareeApi, copyHandle);
    EXPECT_EQ(sensNodes->size(), 0)
        << "Got sensitive nodes after importing from shared folder while this property is expected "
           "to be cleared in the process";

    LOG_debug << "## User 0 copies the sensitive file into the same folder with different name";
    MegaHandle sharerCopyHandle = INVALID_HANDLE;
    ASSERT_NO_FATAL_FAILURE(copyNode(mSharerIndex,
                                     getHandle("/sharedfolder/file.txt"),
                                     getHandle("/sharedfolder"),
                                     "file_copied_by_sharer.txt",
                                     &sharerCopyHandle));

    LOG_debug << "## Checking the copy keeps the sensitive flag";
    std::unique_ptr<MegaNode> dest{mSharerApi->getNodeByHandle(sharerCopyHandle)};
    ASSERT_TRUE(dest->isMarkedSensitive())
        << "Copying a sensitive node within a shared folder by the owner resets the attribute";

    LOG_debug << "## User 1 copies the sensitive file into the same folder with different name";
    MegaHandle shareeCopyHandle = INVALID_HANDLE;
    ASSERT_NO_FATAL_FAILURE(copyNode(mShareeIndex,
                                     getHandle("/sharedfolder/file.txt"),
                                     getHandle("/sharedfolder"),
                                     "file_copied_by_sharee.txt",
                                     &shareeCopyHandle));

    LOG_debug << "## Checking the copy resets the sensitive flag";
    dest.reset(mShareeApi->getNodeByHandle(shareeCopyHandle));
    ASSERT_FALSE(dest->isMarkedSensitive())
        << "Copying a sensitive node within a shared folder by the sharee must reset sensitive";

    LOG_debug << "## User 1 copies sens to exact same place and name";
    ASSERT_NO_FATAL_FAILURE(copyNode(mShareeIndex,
                                     sharerCopyHandle,
                                     getHandle("/sharedfolder"),
                                     "file_copied_by_sharer.txt",
                                     &copyHandle));

    LOG_debug << "## Checking the copy resets the sensitive flag";
    dest.reset(mShareeApi->getNodeByHandle(shareeCopyHandle));
    EXPECT_FALSE(dest->isMarkedSensitive())
        << "Copying a sensitive node to the same place by the sharee must reset sensitive";

    LOG_debug << "## User 1 moves sensitive file from shared folder to own cloud";
    MegaHandle movedHandle = INVALID_HANDLE;
    ASSERT_NO_FATAL_FAILURE(
        moveNodeToOwnCloud("/sharedfolder/file.txt", "moved_file.txt", &movedHandle));
    ASSERT_NE(movedHandle, INVALID_HANDLE);

    LOG_debug << "## Checking the move resets the sensitive flag";
    std::unique_ptr<MegaNode> movedNode{mShareeApi->getNodeByHandle(movedHandle)};
    ASSERT_FALSE(movedNode->isMarkedSensitive())
        << "Moved node from shared folder kept the sensitive label";

    LOG_debug << "## User 1 marks it again as sensitive and copies it back to the shared folder";
    ASSERT_EQ(API_OK, synchronousSetNodeSensitive(mShareeIndex, movedNode.get(), true));
    movedNode.reset(mShareeApi->getNodeByHandle(movedHandle));
    ASSERT_TRUE(movedNode->isMarkedSensitive()) << "There was an error setting sensitive node";
    ASSERT_NO_FATAL_FAILURE(copyNode(mShareeIndex,
                                     movedHandle,
                                     getHandle("/sharedfolder"),
                                     "copied_back_sensitive_file.txt",
                                     &copyHandle));
    LOG_debug << "## Checking the copy resets the sensitive flag";
    dest.reset(mShareeApi->getNodeByHandle(copyHandle));
    ASSERT_FALSE(dest->isMarkedSensitive())
        << "The copy from sharee cloud to shared folder does nor reset the sensitive attribute";
}

/**
 * @brief TEST_F TestPublicFolderLinkLogin
 *
 * Test setup:
 *  - Create a folder (Sharee user)
 *  - Create a public link for that folder (Sharee user)
 *
 * Test steps:
 *  - Login using the folder link (Guest user)
 *  - Expect fetch to be done with MODE_API
 *  - Do local logout (Guest user)
 *  - Login again using the folder link and tryToResumeFolderLinkFromCache as true(Guest user)
 *  - Expect fetch to be done with MODE_DB
 *  - Do local logout (Guest user)
 *  - Login again using the folder link and tryToResumeFolderLinkFromCache as false (Guest user)
 *  - Expect fetch to be done with MODE_API
 *
 */
TEST_F(SdkTestShares, TestPublicFolderLinkLogin)
{
    // Test setup
    ASSERT_NO_FATAL_FAILURE(createNodeTrees());

    const MegaHandle hfolder = getHandle("/sharedfolder");
    ASSERT_EQ(API_OK, synchronousGetSpecificAccountDetails(mSharerIndex, true, true, true))
        << "Cannot get account details";
    std::string nodeLink;
    ASSERT_NO_FATAL_FAILURE(createOnePublicLink(hfolder, nodeLink));

    // Test steps
    bool tryToResumeFolderLinkFromCache = false;
    auto loginFolderTracker = asyncRequestLoginToFolder(mGuestIndex,
                                                        nodeLink.c_str(),
                                                        nullptr,
                                                        tryToResumeFolderLinkFromCache);
    ASSERT_EQ(loginFolderTracker->waitForResult(), API_OK)
        << "Failed to login to folder " << nodeLink;
    ASSERT_NO_FATAL_FAILURE(fetchnodes(mGuestIndex));
    EXPECT_EQ(megaApi[mGuestIndex]->getClient()->fnstats.mode, FetchNodesStats::MODE_API);
    ASSERT_NO_FATAL_FAILURE(locallogout(mGuestIndex));

    tryToResumeFolderLinkFromCache = true;
    loginFolderTracker = asyncRequestLoginToFolder(mGuestIndex,
                                                   nodeLink.c_str(),
                                                   nullptr,
                                                   tryToResumeFolderLinkFromCache);
    ASSERT_EQ(loginFolderTracker->waitForResult(), API_OK)
        << "Failed to login to folder " << nodeLink;
    ASSERT_NO_FATAL_FAILURE(fetchnodes(mGuestIndex));
    EXPECT_EQ(megaApi[mGuestIndex]->getClient()->fnstats.mode, FetchNodesStats::MODE_DB);
    ASSERT_NO_FATAL_FAILURE(locallogout(mGuestIndex));

    tryToResumeFolderLinkFromCache = false;
    loginFolderTracker = asyncRequestLoginToFolder(mGuestIndex,
                                                   nodeLink.c_str(),
                                                   nullptr,
                                                   tryToResumeFolderLinkFromCache);
    ASSERT_EQ(loginFolderTracker->waitForResult(), API_OK)
        << "Failed to login to folder " << nodeLink;
    ASSERT_NO_FATAL_FAILURE(fetchnodes(mGuestIndex));
    EXPECT_EQ(megaApi[mGuestIndex]->getClient()->fnstats.mode, FetchNodesStats::MODE_API);

    // Cleanup
    ASSERT_NO_FATAL_FAILURE(logout(mGuestIndex, false, 20));
}

/**
 * @brief TEST_F SdkTestShares
 *
 * Initialize a test scenario by:
 *
 * - Creating/uploading some folders/files to share
 * - Creating a new contact to share to
 *
 * Performs different operations related to sharing:
 *
 * - Share a folder with an existing contact

 * - Check the correctness of the outgoing share
 * - Check the reception and correctness of the incoming share
 * - Move a shared file (not owned) to Rubbish bin
 * - Add some subfolders
 * - Share a nested folder with same contact
 * - Check the reception and correctness of the incoming nested share
 * - Stop share main in share
 * - Check correctness of the account size
 * - Share the main in share again
 * - Check correctness of the account size
 * - Stop share nested inshare
 * - Check correctness of the account size
 * - Modify the access level
 * - Sharee leaves the inshare
 * - Share again the main folder
 * - Revoke the access to the share
 * - Share a folder with a non registered email
 * - Check the correctness of the pending outgoing share
 * - Create a file public link
 * - Import a file public link
 * - Get a node from a file public link
 * - Remove a public link
 * - Create a folder public link
 * - Import folder public link
 */
TEST_F(SdkTestShares, SdkTestShares)
{
    // Run in action packet streaming parsing mode
    sdk_test::setScParserMode(true);

    LOG_info << "___TEST Shares___";

    // Initialize a test scenario : create some folders/files to share

    // Create some nodes to share
    //  |--Shared-folder
    //    |--subfolder
    //      |--file.txt
    //    |--file.txt

    std::unique_ptr<MegaNode> rootnode{megaApi[0]->getRootNode()};
    char foldername1[64] = "Shared-folder";
    MegaHandle hfolder1 = createFolder(0, foldername1, rootnode.get());
    ASSERT_NE(hfolder1, UNDEF);

    std::unique_ptr<MegaNode> n1(megaApi[0]->getNodeByHandle(hfolder1));
    ASSERT_NE(n1.get(), nullptr);
    unsigned long long inSharedNodeCount = 1;

    char foldername2[64] = "subfolder";
    MegaHandle hfolder2 =
        createFolder(0,
                     foldername2,
                     std::unique_ptr<MegaNode>{megaApi[0]->getNodeByHandle(hfolder1)}.get());
    ASSERT_NE(hfolder2, UNDEF);
    ++inSharedNodeCount;

    // not a large file since don't need to test transfers here
    ASSERT_TRUE(createFile(PUBLICFILE.c_str(), false)) << "Couldn't create " << PUBLICFILE.c_str();

    MegaHandle hfile1 = UNDEF;
    ASSERT_EQ(MegaError::API_OK,
              doStartUpload(0,
                            &hfile1,
                            PUBLICFILE.c_str(),
                            std::unique_ptr<MegaNode>{megaApi[0]->getNodeByHandle(hfolder1)}.get(),
                            nullptr /*fileName*/,
                            ::mega::MegaApi::INVALID_CUSTOM_MOD_TIME,
                            nullptr /*appData*/,
                            false /*isSourceTemporary*/,
                            false /*startFirst*/,
                            nullptr /*cancelToken*/))
        << "Cannot upload a test file";

    ++inSharedNodeCount;

    MegaHandle hfile2 = UNDEF;
    ASSERT_EQ(MegaError::API_OK,
              doStartUpload(0,
                            &hfile2,
                            PUBLICFILE.c_str(),
                            std::unique_ptr<MegaNode>{megaApi[0]->getNodeByHandle(hfolder2)}.get(),
                            nullptr /*fileName*/,
                            ::mega::MegaApi::INVALID_CUSTOM_MOD_TIME,
                            nullptr /*appData*/,
                            false /*isSourceTemporary*/,
                            false /*startFirst*/,
                            nullptr /*cancelToken*/))
        << "Cannot upload a second test file";

    ++inSharedNodeCount;

    // --- Download authorized node from another account ---

    MegaNode* nNoAuth = megaApi[0]->getNodeByHandle(hfile1);

    int transferError =
        doStartDownload(1,
                        nNoAuth,
                        "unauthorized_node",
                        nullptr /*customName*/,
                        nullptr /*appData*/,
                        false /*startFirst*/,
                        nullptr /*cancelToken*/,
                        MegaTransfer::COLLISION_CHECK_FINGERPRINT /*collisionCheck*/,
                        MegaTransfer::COLLISION_RESOLUTION_NEW_WITH_N /* collisionResolution */,
                        false /*undelete*/);

    bool hasFailed = (transferError != API_OK);
    ASSERT_TRUE(hasFailed)
        << "Download of node without authorization successful! (it should fail): " << transferError;

    MegaNode* nAuth = megaApi[0]->authorizeNode(nNoAuth);

    // make sure target download file doesn't already exist:
    deleteFile("authorized_node");

    transferError =
        doStartDownload(1,
                        nAuth,
                        "authorized_node",
                        nullptr /*customName*/,
                        nullptr /*appData*/,
                        false /*startFirst*/,
                        nullptr /*cancelToken*/,
                        MegaTransfer::COLLISION_CHECK_FINGERPRINT /*collisionCheck*/,
                        MegaTransfer::COLLISION_RESOLUTION_NEW_WITH_N /* collisionResolution */,
                        false /*undelete*/);

    ASSERT_EQ(API_OK, transferError)
        << "Cannot download authorized node (error: " << mApi[1].lastError << ")";
    delete nNoAuth;
    delete nAuth;

    // Initialize a test scenario: create a new contact to share to and verify credentials

    string message = "Hi contact. Let's share some stuff";

    mApi[1].contactRequestUpdated = false;
    ASSERT_NO_FATAL_FAILURE(
        inviteContact(0, mApi[1].email, message, MegaContactRequest::INVITE_ACTION_ADD));
    EXPECT_TRUE(waitForResponse(&mApi[1].contactRequestUpdated,
                                10u)) // at the target side (auxiliar account)
        << "Contact request creation not received after 10 seconds";

    EXPECT_NO_FATAL_FAILURE(getContactRequest(1, false));

    mApi[0].contactRequestUpdated = mApi[1].contactRequestUpdated = false;
    EXPECT_NO_FATAL_FAILURE(
        replyContact(mApi[1].cr.get(), MegaContactRequest::REPLY_ACTION_ACCEPT));
    EXPECT_TRUE(waitForResponse(&mApi[1].contactRequestUpdated,
                                10u)) // at the target side (auxiliar account)
        << "Contact request creation not received after 10 seconds";
    EXPECT_TRUE(
        waitForResponse(&mApi[0].contactRequestUpdated, 10u)) // at the source side (main account)
        << "Contact request creation not received after 10 seconds";

    mApi[1].cr.reset();

    if (gManualVerification)
    {
        if (!areCredentialsVerified(0, mApi[1].email))
        {
            ASSERT_NO_FATAL_FAILURE(SdkTest::verifyCredentials(0, mApi[1].email));
        }
        if (!areCredentialsVerified(1, mApi[0].email))
        {
            ASSERT_NO_FATAL_FAILURE(SdkTest::verifyCredentials(1, mApi[0].email));
        }
    }

    auto ownedNodeCount = megaApi[1]->getAccurateNumNodes();

    // upload a file, just to test node counters
    bool check1;
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(INVALID_HANDLE, MegaNode::CHANGE_TYPE_NEW, check1);
    ASSERT_EQ(MegaError::API_OK,
              doStartUpload(1,
                            nullptr,
                            PUBLICFILE.data(),
                            std::unique_ptr<MegaNode>{megaApi[1]->getRootNode()}.get(),
                            nullptr /*fileName*/,
                            ::mega::MegaApi::INVALID_CUSTOM_MOD_TIME,
                            nullptr /*appData*/,
                            false /*isSourceTemporary*/,
                            false /*startFirst*/,
                            nullptr /*cancelToken*/))
        << "Cannot upload a second test file";

    ASSERT_TRUE(waitForResponse(&check1))
        << "Node update not received after " << maxTimeout << " seconds";
    // important to reset
    resetOnNodeUpdateCompletionCBs();
    auto nodeCountAfterNewOwnedFile = megaApi[1]->getAccurateNumNodes();
    ASSERT_EQ(ownedNodeCount + 1, nodeCountAfterNewOwnedFile);
    ownedNodeCount = nodeCountAfterNewOwnedFile;
    ASSERT_EQ(check1, true);

    // --- Create a new outgoing share ---
    bool check2;
    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(hfolder1, MegaNode::CHANGE_TYPE_OUTSHARE, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(hfolder1, MegaNode::CHANGE_TYPE_INSHARE, check2);

    ASSERT_NO_FATAL_FAILURE(shareFolder(n1.get(), mApi[1].email.c_str(), MegaShare::ACCESS_FULL));
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";

    // important to reset
    resetOnNodeUpdateCompletionCBs();
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    // --- Check the outgoing share ---

    auto sl = std::unique_ptr<MegaShareList>{megaApi[0]->getOutShares()};
    ASSERT_EQ(1, sl->size()) << "Outgoing share failed";
    // Test another interface
    sl.reset(megaApi[0]->getOutShares(n1.get()));
    ASSERT_EQ(1, sl->size()) << "Outgoing share failed";

    MegaShare* s = sl->get(0);

    n1.reset(megaApi[0]->getNodeByHandle(hfolder1)); // get an updated version of the node

    ASSERT_EQ(MegaShare::ACCESS_FULL, s->getAccess()) << "Wrong access level of outgoing share";
    ASSERT_EQ(hfolder1, s->getNodeHandle()) << "Wrong node handle of outgoing share";
    ASSERT_STRCASEEQ(mApi[1].email.c_str(), s->getUser())
        << "Wrong email address of outgoing share";
    ASSERT_TRUE(n1->isShared()) << "Wrong sharing information at outgoing share";
    ASSERT_TRUE(n1->isOutShare()) << "Wrong sharing information at outgoing share";

    // --- Check the incoming share ---

    sl.reset(megaApi[1]->getInSharesList());
    ASSERT_EQ(1, sl->size()) << "Incoming share not received in auxiliar account";

    // Wait for the inshare node to be decrypted
    ASSERT_TRUE(WaitFor(
        [this, &n1]()
        {
            return unique_ptr<MegaNode>(megaApi[1]->getNodeByHandle(n1->getHandle()))
                ->isNodeKeyDecrypted();
        },
        60 * 1000));

    std::unique_ptr<MegaUser> contact(megaApi[1]->getContact(mApi[0].email.c_str()));
    auto nl = std::unique_ptr<MegaNodeList>{megaApi[1]->getInShares(contact.get())};
    ASSERT_EQ(1, nl->size()) << "Incoming share not received in auxiliar account";
    MegaNode* n = nl->get(0);

    ASSERT_EQ(hfolder1, n->getHandle()) << "Wrong node handle of incoming share";
    ASSERT_STREQ(foldername1, n->getName()) << "Wrong folder name of incoming share";
    ASSERT_EQ(API_OK,
              megaApi[1]->checkAccessErrorExtended(n, MegaShare::ACCESS_FULL)->getErrorCode())
        << "Wrong access level of incoming share";
    ASSERT_TRUE(n->isInShare()) << "Wrong sharing information at incoming share";
    ASSERT_TRUE(n->isShared()) << "Wrong sharing information at incoming share";

    auto nodeCountAfterInShares = megaApi[1]->getAccurateNumNodes();
    ASSERT_EQ(ownedNodeCount + inSharedNodeCount, nodeCountAfterInShares);

    // --- Move share file from different subtree, same file and fingerprint ---
    // Pre-requisite, the movement finds a file with same name and fp at target folder
    // Since the source and target folders belong to different trees, it will attempt to copy+delete
    // (hfile1 copied to rubbish, renamed to "copy", copied back to hfolder2, move
    // Since there is a file with same name and fingerprint, it will skip the copy and will do
    // delete
    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(INVALID_HANDLE, MegaNode::CHANGE_TYPE_NEW, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(INVALID_HANDLE, MegaNode::CHANGE_TYPE_NEW, check2);
    MegaHandle copiedNodeHandle = INVALID_HANDLE;
    ASSERT_EQ(API_OK,
              doCopyNode(1,
                         &copiedNodeHandle,
                         std::unique_ptr<MegaNode>(megaApi[1]->getNodeByHandle(hfile2)).get(),
                         std::unique_ptr<MegaNode>(megaApi[1]->getNodeByHandle(hfolder1)).get(),
                         "copy"))
        << "Copying shared file (not owned) to same place failed";
    EXPECT_TRUE(waitForResponse(&check1, 10u)) // at the target side (main account)
        << "Node update not received after 10 seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";

    resetOnNodeUpdateCompletionCBs();
    ++inSharedNodeCount;
    EXPECT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(INVALID_HANDLE, MegaNode::CHANGE_TYPE_NEW, check1);
    MegaHandle copiedNodeHandleInRubbish = INVALID_HANDLE;
    std::unique_ptr<MegaNode> rubbishNode(megaApi[1]->getRubbishNode());
    std::unique_ptr<MegaNode> copiedNode(megaApi[1]->getNodeByHandle(copiedNodeHandle));
    ASSERT_EQ(API_OK,
              doCopyNode(1, &copiedNodeHandleInRubbish, copiedNode.get(), rubbishNode.get()))
        << "Copying shared file (not owned) to Rubbish bin failed";
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";

    resetOnNodeUpdateCompletionCBs();
    ++ownedNodeCount;
    ASSERT_EQ(check1, true);

    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(copiedNodeHandle, MegaNode::CHANGE_TYPE_REMOVED, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(copiedNodeHandle, MegaNode::CHANGE_TYPE_REMOVED, check2);
    MegaHandle copyAndDeleteNodeHandle = INVALID_HANDLE;

    copiedNode.reset(megaApi[0]->getNodeByHandle(copiedNodeHandle));
    EXPECT_EQ(API_OK, doMoveNode(1, &copyAndDeleteNodeHandle, copiedNode.get(), rubbishNode.get()))
        << "Moving shared file, same name and fingerprint";

    ASSERT_EQ(megaApi[1]->getNodeByHandle(copiedNodeHandle), nullptr)
        << "Move didn't delete source file";
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";

    resetOnNodeUpdateCompletionCBs();
    --inSharedNodeCount;
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    // --- Move shared file (not owned) to Rubbish bin ---
    MegaHandle movedNodeHandle = UNDEF;
    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(hfile2, MegaNode::CHANGE_TYPE_REMOVED, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(hfile2, MegaNode::CHANGE_TYPE_REMOVED, check2);
    ASSERT_EQ(API_OK,
              doMoveNode(1,
                         &movedNodeHandle,
                         std::unique_ptr<MegaNode>(megaApi[0]->getNodeByHandle(hfile2)).get(),
                         rubbishNode.get()))
        << "Moving shared file (not owned) to Rubbish bin failed";
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";
    // important to reset
    resetOnNodeUpdateCompletionCBs();
    --inSharedNodeCount;
    ++ownedNodeCount;
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    // --- Test that file in Rubbish bin can be restored ---

    // Different handle! the node must have been copied due to differing accounts
    std::unique_ptr<MegaNode> nodeMovedFile(megaApi[1]->getNodeByHandle(movedNodeHandle));
    ASSERT_EQ(nodeMovedFile->getRestoreHandle(), hfolder2)
        << "Incorrect restore handle for file in Rubbish Bin";

    // check the corresponding user alert
    ASSERT_TRUE(
        checkAlert(1, "New shared folder from " + mApi[0].email, mApi[0].email + ":Shared-folder"));

    // add folders under the share
    char foldernameA[64] = "dummyname1";
    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(INVALID_HANDLE, MegaNode::CHANGE_TYPE_NEW, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(INVALID_HANDLE, MegaNode::CHANGE_TYPE_NEW, check2);
    MegaHandle dummyhandle1 =
        createFolder(0,
                     foldernameA,
                     std::unique_ptr<MegaNode>{megaApi[0]->getNodeByHandle(hfolder2)}.get());
    ASSERT_NE(dummyhandle1, UNDEF);
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";
    // important to reset
    resetOnNodeUpdateCompletionCBs();
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    char foldernameB[64] = "dummyname2";
    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(INVALID_HANDLE, MegaNode::CHANGE_TYPE_NEW, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(INVALID_HANDLE, MegaNode::CHANGE_TYPE_NEW, check2);
    MegaHandle dummyhandle2 =
        createFolder(0,
                     foldernameB,
                     std::unique_ptr<MegaNode>{megaApi[0]->getNodeByHandle(hfolder2)}.get());
    ASSERT_NE(dummyhandle2, UNDEF);
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";
    // important to reset
    resetOnNodeUpdateCompletionCBs();
    inSharedNodeCount += 2;
    unsigned long long nodesAtFolderDummyname2 = 1; // Take account own node
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    auto nodeCountAfterInSharesAddedDummyFolders = megaApi[1]->getAccurateNumNodes();
    ASSERT_EQ(ownedNodeCount + inSharedNodeCount, nodeCountAfterInSharesAddedDummyFolders);

    // check the corresponding user alert
    EXPECT_TRUE(checkAlert(
        1,
        mApi[0].email + " added 2 folders",
        std::unique_ptr<MegaNode> { megaApi[0]->getNodeByHandle(hfolder2) } -> getHandle(),
        2,
        dummyhandle1));

    // add 2 more files to the share
    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(INVALID_HANDLE, MegaNode::CHANGE_TYPE_NEW, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(INVALID_HANDLE, MegaNode::CHANGE_TYPE_NEW, check2);
    ASSERT_EQ(
        MegaError::API_OK,
        doStartUpload(0,
                      nullptr,
                      PUBLICFILE.data(),
                      std::unique_ptr<MegaNode>{megaApi[0]->getNodeByHandle(dummyhandle1)}.get(),
                      nullptr /*fileName*/,
                      ::mega::MegaApi::INVALID_CUSTOM_MOD_TIME,
                      nullptr /*appData*/,
                      false /*isSourceTemporary*/,
                      false /*startFirst*/,
                      nullptr /*cancelToken*/))
        << "Cannot upload a test file";

    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";
    // important to reset
    resetOnNodeUpdateCompletionCBs();
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    ++inSharedNodeCount;
    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(INVALID_HANDLE, MegaNode::CHANGE_TYPE_NEW, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(INVALID_HANDLE, MegaNode::CHANGE_TYPE_NEW, check2);
    ASSERT_EQ(
        MegaError::API_OK,
        doStartUpload(0,
                      nullptr,
                      PUBLICFILE.data(),
                      std::unique_ptr<MegaNode>{megaApi[0]->getNodeByHandle(dummyhandle2)}.get(),
                      nullptr /*fileName*/,
                      ::mega::MegaApi::INVALID_CUSTOM_MOD_TIME,
                      nullptr /*appData*/,
                      false /*isSourceTemporary*/,
                      false /*startFirst*/,
                      nullptr /*cancelToken*/))
        << "Cannot upload a test file";

    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";
    // important to reset
    resetOnNodeUpdateCompletionCBs();
    ++inSharedNodeCount;
    ++nodesAtFolderDummyname2;
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    auto nodeCountAfterInSharesAddedDummyFile = megaApi[1]->getAccurateNumNodes();
    ASSERT_EQ(ownedNodeCount + inSharedNodeCount, nodeCountAfterInSharesAddedDummyFile);

    // move a folder outside share
    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(dummyhandle1, MegaNode::CHANGE_TYPE_PARENT, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(dummyhandle1, MegaNode::CHANGE_TYPE_REMOVED, check2);
    std::unique_ptr<MegaNode> dummyNode1(megaApi[0]->getNodeByHandle(dummyhandle1));
    megaApi[0]->moveNode(dummyNode1.get(), rootnode.get());
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";
    // important to reset
    resetOnNodeUpdateCompletionCBs();
    inSharedNodeCount -= 2;
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    auto nodeCountAfterInSharesRemovedDummyFolder1 = megaApi[1]->getAccurateNumNodes();
    ASSERT_EQ(ownedNodeCount + inSharedNodeCount, nodeCountAfterInSharesRemovedDummyFolder1);

    // add a nested share
    std::unique_ptr<MegaNode> dummyNode2(megaApi[0]->getNodeByHandle(dummyhandle2));
    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(dummyhandle2, MegaNode::CHANGE_TYPE_OUTSHARE, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(dummyhandle2, MegaNode::CHANGE_TYPE_INSHARE, check2);
    ASSERT_NO_FATAL_FAILURE(
        shareFolder(dummyNode2.get(), mApi[1].email.data(), MegaShare::ACCESS_FULL));
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";
    // important to reset
    resetOnNodeUpdateCompletionCBs();
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    // number of nodes should not change, because this node is a nested share
    auto nodeCountAfterInSharesAddedNestedSubfolder = megaApi[1]->getAccurateNumNodes();
    ASSERT_EQ(ownedNodeCount + inSharedNodeCount, nodeCountAfterInSharesAddedNestedSubfolder);

    // Stop share main folder (Shared-folder)
    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(n1->getHandle(), MegaNode::CHANGE_TYPE_OUTSHARE, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(n1->getHandle(), MegaNode::CHANGE_TYPE_REMOVED, check2);
    ASSERT_NO_FATAL_FAILURE(shareFolder(n1.get(), mApi[1].email.data(), MegaShare::ACCESS_UNKNOWN));
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";
    // important to reset
    resetOnNodeUpdateCompletionCBs();
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    // number of nodes own cloud + nodes at nested in-share
    auto nodeCountAfterRemoveMainInshare = megaApi[1]->getAccurateNumNodes();
    ASSERT_EQ(ownedNodeCount + nodesAtFolderDummyname2, nodeCountAfterRemoveMainInshare);

    // Share again main folder (Shared-folder)
    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(n1->getHandle(), MegaNode::CHANGE_TYPE_OUTSHARE, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(n1->getHandle(), MegaNode::CHANGE_TYPE_INSHARE, check2);
    ASSERT_NO_FATAL_FAILURE(shareFolder(n1.get(), mApi[1].email.data(), MegaShare::ACCESS_FULL));
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";
    // important to reset
    resetOnNodeUpdateCompletionCBs();
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    // number of nodes own cloud + nodes at nested in-share
    auto nodeCountAfterShareN1 = megaApi[1]->getAccurateNumNodes();
    ASSERT_EQ(ownedNodeCount + inSharedNodeCount, nodeCountAfterShareN1);

    // remove nested share
    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(dummyNode2->getHandle(), MegaNode::CHANGE_TYPE_OUTSHARE, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(dummyNode2->getHandle(), MegaNode::CHANGE_TYPE_INSHARE, check2);
    ASSERT_NO_FATAL_FAILURE(
        shareFolder(dummyNode2.get(), mApi[1].email.data(), MegaShare::ACCESS_UNKNOWN));
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";

    // important to reset
    resetOnNodeUpdateCompletionCBs();
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    // number of nodes should not change, because this node was a nested share
    auto nodeCountAfterInSharesRemovedNestedSubfolder = megaApi[1]->getAccurateNumNodes();
    ASSERT_EQ(ownedNodeCount + inSharedNodeCount, nodeCountAfterInSharesRemovedNestedSubfolder);

    // --- Modify the access level of an outgoing share ---
    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(hfolder1, MegaNode::CHANGE_TYPE_OUTSHARE, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(hfolder1, MegaNode::CHANGE_TYPE_INSHARE, check2);

    ASSERT_NO_FATAL_FAILURE(
        shareFolder(std::unique_ptr<MegaNode>(megaApi[0]->getNodeByHandle(hfolder1)).get(),
                    mApi[1].email.c_str(),
                    MegaShare::ACCESS_READWRITE));
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";

    // important to reset
    resetOnNodeUpdateCompletionCBs();
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    contact.reset(megaApi[1]->getContact(mApi[0].email.c_str()));
    nl.reset(megaApi[1]->getInShares(contact.get()));
    ASSERT_EQ(1, nl->size()) << "Incoming share not received in auxiliar account";
    n = nl->get(0);

    ASSERT_EQ(API_OK,
              megaApi[1]->checkAccessErrorExtended(n, MegaShare::ACCESS_READWRITE)->getErrorCode())
        << "Wrong access level of incoming share";

    // --- Sharee leaves the inshare ---
    // Testing APs caused by actions done in the sharee account.
    unique_ptr<MegaNode> inshareRootNode(megaApi[1]->getNodeByHandle(hfolder1));

    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(hfolder1, MegaNode::CHANGE_TYPE_OUTSHARE, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(hfolder1, MegaNode::CHANGE_TYPE_REMOVED, check2);
    ASSERT_NO_FATAL_FAILURE(doDeleteNode(
        1,
        inshareRootNode.get())); // Delete an inshare root node to leave the inconming share
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";
    // important to reset
    resetOnNodeUpdateCompletionCBs();
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    sl.reset(megaApi[0]->getOutShares());
    ASSERT_EQ(0, sl->size())
        << "Leaving the inshare failed. Outshare is still active in the first account.";

    contact.reset(megaApi[1]->getContact(mApi[0].email.c_str()));
    nl.reset(megaApi[1]->getInShares(contact.get()));
    ASSERT_EQ(0, nl->size())
        << "Leaving the inshare failed. Inshare is still active in the second account.";

    // Number of nodes should be the ones in the account only.
    auto nodeCountAfterShareeLeavesShare = megaApi[1]->getNumNodes();
    ASSERT_EQ(ownedNodeCount, nodeCountAfterShareeLeavesShare);

    // --- Share again the main folder ---
    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(hfolder1, MegaNode::CHANGE_TYPE_OUTSHARE, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(hfolder1, MegaNode::CHANGE_TYPE_INSHARE, check2);
    ASSERT_NO_FATAL_FAILURE(shareFolder(n1.get(), mApi[1].email.data(), MegaShare::ACCESS_FULL));
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";
    // important to reset
    resetOnNodeUpdateCompletionCBs();
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    sl.reset(megaApi[0]->getOutShares());
    ASSERT_EQ(1, sl->size()) << "Outgoing share failed. Sharing again after sharee left the share.";

    // Wait for the inshare node to be decrypted
    ASSERT_TRUE(WaitFor(
        [this, &n1]()
        {
            return unique_ptr<MegaNode>(megaApi[1]->getNodeByHandle(n1->getHandle()))
                ->isNodeKeyDecrypted();
        },
        60 * 1000));

    contact.reset(megaApi[1]->getContact(mApi[0].email.c_str()));
    nl.reset(megaApi[1]->getInShares(contact.get()));
    ASSERT_EQ(1, nl->size()) << "Incoming share failed. Sharing again after sharee left the share.";

    // Number of nodes restored after sharing again.
    auto nodeCountAfterShareAgainIfShareeLeaves = megaApi[1]->getNumNodes();
    ASSERT_EQ(ownedNodeCount + inSharedNodeCount, nodeCountAfterShareAgainIfShareeLeaves);

    // --- Revoke access to an outgoing share ---

    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(hfolder1, MegaNode::CHANGE_TYPE_OUTSHARE, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(hfolder1, MegaNode::CHANGE_TYPE_REMOVED, check2);
    ASSERT_NO_FATAL_FAILURE(
        shareFolder(n1.get(), mApi[1].email.c_str(), MegaShare::ACCESS_UNKNOWN));
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";

    // important to reset
    resetOnNodeUpdateCompletionCBs();
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    sl.reset(megaApi[0]->getOutShares());
    ASSERT_EQ(0, sl->size()) << "Outgoing share revocation failed";
    // Test another interface
    sl.reset(megaApi[0]->getOutShares(n1.get()));
    ASSERT_EQ(0, sl->size()) << "Outgoing share revocation failed";

    contact.reset(megaApi[1]->getContact(mApi[0].email.c_str()));
    nl.reset(megaApi[1]->getInShares(contact.get()));
    ASSERT_EQ(0, nl->size()) << "Incoming share revocation failed";

    // check the corresponding user alert
    {
        MegaUserAlertList* list = megaApi[1]->getUserAlerts();
        ASSERT_TRUE(list->size() > 0);
        MegaUserAlert* a = list->get(list->size() - 1);
        ASSERT_STRCASEEQ(a->getTitle(),
                         ("Access to folders shared by " + mApi[0].email + " was removed").c_str());
        ASSERT_STRCASEEQ(a->getPath(), (mApi[0].email + ":Shared-folder").c_str());
        ASSERT_NE(a->getNodeHandle(), UNDEF);
        delete list;
    }

    auto nodeCountAfterRevokedSharesAccess = megaApi[1]->getAccurateNumNodes();
    ASSERT_EQ(ownedNodeCount, nodeCountAfterRevokedSharesAccess);

    // --- Get pending outgoing shares ---

    char emailfake[64];
    srand(unsigned(time(NULL)));
    snprintf(emailfake, sizeof(emailfake), "%d@nonexistingdomain.com", rand() % 1000000);
    // carefull, antispam rejects too many tries without response for the same address

    auto node = std::unique_ptr<MegaNode>{megaApi[0]->getNodeByHandle(hfolder2)};

    mApi[0].contactRequestUpdated = false;
    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(hfolder2, MegaNode::CHANGE_TYPE_PENDINGSHARE, check1);

    ASSERT_NO_FATAL_FAILURE(shareFolder(node.get(), emailfake, MegaShare::ACCESS_FULL));
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(
        waitForResponse(&mApi[0].contactRequestUpdated)) // at the target side (main account)
        << "Contact request update not received after " << maxTimeout << " seconds";

    // important to reset
    resetOnNodeUpdateCompletionCBs();
    ASSERT_EQ(check1, true);

    sl.reset(megaApi[0]->getPendingOutShares(node.get()));
    ASSERT_EQ(1, sl->size()) << "Pending outgoing share failed";
    // Test another interface
    sl.reset(megaApi[0]->getOutShares(node.get()));
    ASSERT_EQ(1, sl->size()) << "Pending outgoing share failed";
    s = sl->get(0);
    node.reset(megaApi[0]->getNodeByHandle(s->getNodeHandle()));

    ASSERT_FALSE(node->isShared()) << "Node is already shared, must be pending";
    ASSERT_FALSE(node->isOutShare()) << "Node is already shared, must be pending";
    ASSERT_FALSE(node->isInShare()) << "Node is already shared, must be pending";

    mApi[0].mOnNodesUpdateCompletion = createOnNodesUpdateLambda(dummyNode1->getHandle(),
                                                                 MegaNode::CHANGE_TYPE_PENDINGSHARE,
                                                                 check1);
    ASSERT_NO_FATAL_FAILURE(shareFolder(dummyNode1.get(), emailfake, MegaShare::ACCESS_FULL));
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";

    // important to reset
    resetOnNodeUpdateCompletionCBs();
    ASSERT_EQ(check1, true);

    sl.reset(megaApi[0]->getPendingOutShares());
    ASSERT_EQ(2, sl->size()) << "Pending outgoing share failed";
    // Test another interface
    sl.reset(megaApi[0]->getOutShares());
    ASSERT_EQ(2, sl->size()) << "Pending outgoing share failed";

    // --- Create a file public link ---

    ASSERT_EQ(API_OK, synchronousGetSpecificAccountDetails(0, true, true, true))
        << "Cannot get account details";

    std::unique_ptr<MegaNode> nfile1{megaApi[0]->getNodeByHandle(hfile1)};

    string nodelink3 = createPublicLink(0,
                                        nfile1.get(),
                                        0,
                                        maxTimeout,
                                        mApi[0].accountDetails->getProLevel() == 0);
    // The created link is stored in this->link at onRequestFinish()

    // Get a fresh snapshot of the node and check it's actually exported
    nfile1 = std::unique_ptr<MegaNode>{megaApi[0]->getNodeByHandle(hfile1)};
    ASSERT_TRUE(nfile1->isExported()) << "Node is not exported, must be exported";
    ASSERT_FALSE(nfile1->isTakenDown()) << "Public link is taken down, it mustn't";

    // Make sure that search functionality finds it
    std::unique_ptr<MegaSearchFilter> filterResults(MegaSearchFilter::createInstance());
    filterResults->byName(nfile1->getName());
    filterResults->byLocation(MegaApi::SEARCH_TARGET_PUBLICLINK);
    std::unique_ptr<MegaNodeList> foundByLink(megaApi[0]->search(filterResults.get()));
    ASSERT_TRUE(foundByLink);
    ASSERT_EQ(foundByLink->size(), 1);
    ASSERT_EQ(foundByLink->get(0)->getHandle(), nfile1->getHandle());

    // Regenerate the same link should not trigger a new request
    nfile1 = std::unique_ptr<MegaNode>{megaApi[0]->getNodeByHandle(hfile1)};
    string nodelink4 = createPublicLink(0,
                                        nfile1.get(),
                                        0,
                                        maxTimeout,
                                        mApi[0].accountDetails->getProLevel() == 0);
    ASSERT_STREQ(nodelink3.c_str(), nodelink4.c_str()) << "Wrong public link after link update";

    // Try to update the expiration time of an existing link (only for PRO accounts are allowed,
    // otherwise -11
    string nodelinkN = createPublicLink(0,
                                        nfile1.get(),
                                        m_time() + 30 * 86400,
                                        maxTimeout,
                                        mApi[0].accountDetails->getProLevel() == 0);
    nfile1 = std::unique_ptr<MegaNode>{megaApi[0]->getNodeByHandle(hfile1)};
    if (mApi[0].accountDetails->getProLevel() == 0)
    {
        ASSERT_EQ(0, nfile1->getExpirationTime())
            << "Expiration time successfully set, when it shouldn't";
    }
    ASSERT_FALSE(nfile1->isExpired()) << "Public link is expired, it mustn't";

    // --- Import a file public link ---

    auto importHandle = SdkTest::importPublicLink(0, nodelink4, rootnode.get());

    MegaNode* nimported = megaApi[0]->getNodeByHandle(importHandle);

    ASSERT_STREQ(nfile1->getName(), nimported->getName()) << "Imported file with wrong name";
    ASSERT_EQ(rootnode->getHandle(), nimported->getParentHandle()) << "Imported file in wrong path";

    // --- Get node from file public link ---

    auto nodeUP = getPublicNode(1, nodelink4);

    ASSERT_TRUE(nodeUP && nodeUP->isPublic()) << "Cannot get a node from public link";

    // --- Remove a public link ---

    MegaHandle removedLinkHandle = removePublicLink(0, nfile1.get());

    nfile1 = std::unique_ptr<MegaNode>{megaApi[0]->getNodeByHandle(removedLinkHandle)};
    ASSERT_FALSE(nfile1->isPublic()) << "Public link removal failed (still public)";

    delete nimported;

    // --- Create a folder public link ---

    std::unique_ptr<MegaNode> nfolder1(megaApi[0]->getNodeByHandle(hfolder1));

    string nodelink5 = createPublicLink(0,
                                        nfolder1.get(),
                                        0,
                                        maxTimeout,
                                        mApi[0].accountDetails->getProLevel() == 0);
    // The created link is stored in this->link at onRequestFinish()

    // Get a fresh snapshot of the node and check it's actually exported
    nfolder1.reset(megaApi[0]->getNodeByHandle(hfolder1));
    ASSERT_TRUE(nfolder1->isExported()) << "Node is not exported, must be exported";
    ASSERT_FALSE(nfolder1->isTakenDown()) << "Public link is taken down, it mustn't";

    nfolder1.reset(megaApi[0]->getNodeByHandle(hfolder1));
    ASSERT_STREQ(nodelink5.c_str(), std::unique_ptr<char[]>(nfolder1->getPublicLink()).get())
        << "Wrong public link from MegaNode";

    // Make sure that search functionality finds it
    filterResults.reset(MegaSearchFilter::createInstance());
    filterResults->byName(nfolder1->getName());
    filterResults->byLocation(MegaApi::SEARCH_TARGET_PUBLICLINK);
    foundByLink.reset(megaApi[0]->search(filterResults.get()));
    ASSERT_TRUE(foundByLink);
    ASSERT_EQ(foundByLink->size(), 1);
    ASSERT_EQ(foundByLink->get(0)->getHandle(), nfolder1->getHandle());

    // Regenerate the same link should not trigger a new request
    string nodelink6 = createPublicLink(0,
                                        nfolder1.get(),
                                        0,
                                        maxTimeout,
                                        mApi[0].accountDetails->getProLevel() == 0);
    ASSERT_STREQ(nodelink5.c_str(), nodelink6.c_str()) << "Wrong public link after link update";

    // --- Import folder public link ---
    const auto [email, pass] = getEnvVarAccounts().getVarValues(2);
    ASSERT_FALSE(email.empty() || pass.empty());
    mApi.resize(3);
    megaApi.resize(3);
    configureTestInstance(2, email, pass);
    auto loginFolderTracker = asyncRequestLoginToFolder(2, nodelink6.c_str());
    ASSERT_EQ(loginFolderTracker->waitForResult(), API_OK)
        << "Failed to login to folder " << nodelink6;
    ASSERT_NO_FATAL_FAILURE(fetchnodes(2));
    std::unique_ptr<MegaNode> folderNodeToImport(megaApi[2]->getRootNode());
    ASSERT_TRUE(folderNodeToImport)
        << "Failed to get folder node to import from link " << nodelink6;
    std::unique_ptr<MegaNode> authorizedFolderNode(
        megaApi[2]->authorizeNode(folderNodeToImport.get()));
    ASSERT_TRUE(authorizedFolderNode) << "Failed to authorize folder node from link " << nodelink6;
    ASSERT_TRUE(authorizedFolderNode->getChildren())
        << "Authorized folder node children list is null but it should not";
    ASSERT_EQ(megaApi[2]->getNumChildren(folderNodeToImport.get()),
              authorizedFolderNode->getChildren()->size())
        << "Different number of child nodes after authorizing the folder node";
    logout(2, false, 20);

    auto loginTracker = asyncRequestLogin(2, email.c_str(), pass.c_str());
    ASSERT_EQ(loginTracker->waitForResult(), API_OK) << "Failed to login with " << email;
    ASSERT_NO_FATAL_FAILURE(fetchnodes(2));
    std::unique_ptr<MegaNode> rootNode2(megaApi[2]->getRootNode());
    RequestTracker nodeCopyTracker(megaApi[2].get());
    megaApi[2]->copyNode(authorizedFolderNode.get(), rootNode2.get(), nullptr, &nodeCopyTracker);
    EXPECT_EQ(nodeCopyTracker.waitForResult(), API_OK) << "Failed to copy node to import";
    std::unique_ptr<MegaNode> importedNode(
        megaApi[2]->getNodeByPath(authorizedFolderNode->getName(), rootNode2.get()));
    EXPECT_TRUE(importedNode) << "Imported node not found";
    std::unique_ptr<MegaNode> authorizedImportedNode(megaApi[2]->authorizeNode(importedNode.get()));
    EXPECT_TRUE(authorizedImportedNode) << "Failed to authorize imported node";
    EXPECT_TRUE(authorizedImportedNode->getChildren())
        << "Authorized imported node children list is null but it should not";
    ASSERT_EQ(authorizedFolderNode->getChildren()->size(),
              authorizedImportedNode->getChildren()->size())
        << "Not all child nodes have been imported";
}

/**
 * @brief TEST_F SdkTestShares2
 *
 * - Create and upload some folders and files to User1 account
 * - Create a new contact to share to
 * - Share a folder with User2
 * - Check the outgoing share from User1
 * - Check the incoming share to User2
 * - Check that User2 (sharee) cannot tag the incoming share as favourite
 * - Check that User1 (sharer) can tag the outgoing share as favourite
 * - Get file name and fingerprint from User1
 * - Search by file name for User2
 * - Search by fingerprint for User2
 * - User2 add file
 * - Check that User1 has received the change
 * - User1 remove file
 * - Locallogout from User2 and login with session
 * - Check that User2 no longer sees the removed file
 */
TEST_F(SdkTestShares, SdkTestShares2)
{
    // Run in action packet non-streaming parsing mode
    sdk_test::setScParserMode(false);

    // --- Create some nodes to share ---
    //  |--Shared-folder
    //    |--subfolder
    //      |--file.txt
    //    |--file.txt

    std::unique_ptr<MegaNode> rootnode{megaApi[0]->getRootNode()};
    static constexpr char foldername1[] = "Shared-folder";
    MegaHandle hfolder1 = createFolder(0, foldername1, rootnode.get());
    ASSERT_NE(hfolder1, UNDEF) << "Cannot create " << foldername1;

    std::unique_ptr<MegaNode> n1{megaApi[0]->getNodeByHandle(hfolder1)};
    ASSERT_NE(n1, nullptr);

    static constexpr char foldername2[] = "subfolder";
    MegaHandle hfolder2 = createFolder(0, foldername2, n1.get());
    ASSERT_NE(hfolder2, UNDEF) << "Cannot create " << foldername2;

    createFile(PUBLICFILE.c_str(),
               false); // not a large file since don't need to test transfers here

    MegaHandle hfile1 = 0;
    ASSERT_EQ(MegaError::API_OK,
              doStartUpload(0,
                            &hfile1,
                            PUBLICFILE.c_str(),
                            n1.get(),
                            nullptr /*fileName*/,
                            ::mega::MegaApi::INVALID_CUSTOM_MOD_TIME,
                            nullptr /*appData*/,
                            false /*isSourceTemporary*/,
                            false /*startFirst*/,
                            nullptr /*cancelToken*/))
        << "Cannot upload a test file";

    MegaHandle hfile2 = 0;
    ASSERT_EQ(MegaError::API_OK,
              doStartUpload(0,
                            &hfile2,
                            PUBLICFILE.c_str(),
                            std::unique_ptr<MegaNode>{megaApi[0]->getNodeByHandle(hfolder2)}.get(),
                            nullptr /*fileName*/,
                            ::mega::MegaApi::INVALID_CUSTOM_MOD_TIME,
                            nullptr /*appData*/,
                            false /*isSourceTemporary*/,
                            false /*startFirst*/,
                            nullptr /*cancelToken*/))
        << "Cannot upload a second test file";

    // --- Create a new contact to share to ---

    string message = "Hi contact. Let's share some stuff";

    mApi[1].contactRequestUpdated = false;
    ASSERT_NO_FATAL_FAILURE(
        inviteContact(0, mApi[1].email, message, MegaContactRequest::INVITE_ACTION_ADD));
    ASSERT_TRUE(
        waitForResponse(&mApi[1].contactRequestUpdated)) // at the target side (auxiliar account)
        << "Contact request creation not received after " << maxTimeout << " seconds";

    ASSERT_NO_FATAL_FAILURE(getContactRequest(1, false));

    mApi[0].contactRequestUpdated = mApi[1].contactRequestUpdated = false;
    ASSERT_NO_FATAL_FAILURE(
        replyContact(mApi[1].cr.get(), MegaContactRequest::REPLY_ACTION_ACCEPT));
    ASSERT_TRUE(
        waitForResponse(&mApi[1].contactRequestUpdated)) // at the target side (auxiliar account)
        << "Contact request creation not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(
        waitForResponse(&mApi[0].contactRequestUpdated)) // at the source side (main account)
        << "Contact request creation not received after " << maxTimeout << " seconds";

    mApi[1].cr.reset();

    // --- Verify credentials in both accounts ---

    if (gManualVerification)
    {
        if (!areCredentialsVerified(0, mApi[1].email))
        {
            ASSERT_NO_FATAL_FAILURE(SdkTest::verifyCredentials(0, mApi[1].email));
        }
        if (!areCredentialsVerified(1, mApi[0].email))
        {
            ASSERT_NO_FATAL_FAILURE(SdkTest::verifyCredentials(1, mApi[0].email));
        }
    }

    // --- Share a folder with User2 ---
    MegaHandle nodeHandle = n1->getHandle();
    bool check1, check2;
    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(nodeHandle, MegaNode::CHANGE_TYPE_OUTSHARE, check1);
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(nodeHandle, MegaNode::CHANGE_TYPE_INSHARE, check2);

    ASSERT_NO_FATAL_FAILURE(shareFolder(n1.get(), mApi[1].email.c_str(), MegaShare::ACCESS_FULL));
    ASSERT_TRUE(waitForResponse(&check1)) // at the target side (main account)
        << "Node update not received after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2)) // at the target side (auxiliar account)
        << "Node update not received after " << maxTimeout << " seconds";
    // important to reset
    resetOnNodeUpdateCompletionCBs();
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    // --- Check the outgoing share from User1 ---

    std::unique_ptr<MegaShareList> sl{megaApi[0]->getOutShares()};
    ASSERT_EQ(1, sl->size()) << "Outgoing share failed";
    MegaShare* s = sl->get(0);

    n1.reset(megaApi[0]->getNodeByHandle(hfolder1)); // get an updated version of the node

    ASSERT_EQ(MegaShare::ACCESS_FULL, s->getAccess()) << "Wrong access level of outgoing share";
    ASSERT_EQ(hfolder1, s->getNodeHandle()) << "Wrong node handle of outgoing share";
    ASSERT_STRCASEEQ(mApi[1].email.c_str(), s->getUser())
        << "Wrong email address of outgoing share";
    ASSERT_TRUE(n1->isShared()) << "Wrong sharing information at outgoing share";
    ASSERT_TRUE(n1->isOutShare()) << "Wrong sharing information at outgoing share";

    // --- Check the incoming share to User2 ---

    sl.reset(megaApi[1]->getInSharesList());
    ASSERT_EQ(1, sl->size()) << "Incoming share not received in auxiliar account";

    // Wait for the inshare node to be decrypted
    ASSERT_TRUE(WaitFor(
        [this, &n1]()
        {
            return unique_ptr<MegaNode>(megaApi[1]->getNodeByHandle(n1->getHandle()))
                ->isNodeKeyDecrypted();
        },
        60 * 1000));

    std::unique_ptr<MegaUser> contact(megaApi[1]->getContact(mApi[0].email.c_str()));
    std::unique_ptr<MegaNodeList> nl(megaApi[1]->getInShares(contact.get()));
    ASSERT_EQ(1, nl->size()) << "Incoming share not received in auxiliar account";
    MegaNode* n = nl->get(0);

    ASSERT_EQ(hfolder1, n->getHandle()) << "Wrong node handle of incoming share";
    ASSERT_STREQ(foldername1, n->getName()) << "Wrong folder name of incoming share";
    ASSERT_EQ(MegaError::API_OK,
              megaApi[1]->checkAccessErrorExtended(n, MegaShare::ACCESS_FULL)->getErrorCode())
        << "Wrong access level of incoming share";
    ASSERT_TRUE(n->isInShare()) << "Wrong sharing information at incoming share";
    ASSERT_TRUE(n->isShared()) << "Wrong sharing information at incoming share";

    // --- Check that User2 (sharee) cannot tag the incoming share as favourite ---

    auto errU2SetFavourite = synchronousSetNodeFavourite(1, n, true);
    ASSERT_EQ(API_EACCESS, errU2SetFavourite)
        << " synchronousSetNodeFavourite by the sharee should return API_EACCESS (returned error: "
        << errU2SetFavourite << ")";

    // --- Check that User2 (sharee) cannot tag an inner inshare folder as favourite ---

    std::unique_ptr<MegaNode> subfolderNode{megaApi[1]->getNodeByHandle(hfolder2)};
    auto errU2SetFavourite2 = synchronousSetNodeFavourite(1, subfolderNode.get(), true);
    ASSERT_EQ(API_EACCESS, errU2SetFavourite2)
        << " synchronousSetNodeFavourite by the sharee should return API_EACCESS (returned error: "
        << errU2SetFavourite << ")";

    // --- Check that User1 (sharer) can tag the outgoing share as favourite ---

    auto errU1SetFavourite = synchronousSetNodeFavourite(0, n, true);
    ASSERT_EQ(API_OK, errU1SetFavourite)
        << " synchronousSetNodeFavourite by the sharer failed (error: " << errU1SetFavourite << ")";

    // --- Check that User1 (sharer) can tag an inner outshare folder as favourite ---

    auto errU1SetFavourite2 = synchronousSetNodeFavourite(0, subfolderNode.get(), true);
    ASSERT_EQ(API_OK, errU1SetFavourite2)
        << " synchronousSetNodeFavourite by the sharer failed (error: " << errU1SetFavourite << ")";

    // --- Get file name and fingerprint from User1 account ---

    unique_ptr<MegaNode> nfile2(megaApi[0]->getNodeByHandle(hfile2));
    ASSERT_NE(nfile2, nullptr) << "Cannot initialize second node for scenario (error: "
                               << mApi[0].lastError << ")";
    const char* fileNameToSearch = nfile2->getName();
    const char* fingerPrintToSearch = nfile2->getFingerprint();

    // --- Search by fingerprint for User2 ---

    unique_ptr<MegaNodeList> fingerPrintList(
        megaApi[1]->getNodesByFingerprint(fingerPrintToSearch));
    ASSERT_EQ(fingerPrintList->size(), 2)
        << "Node count by fingerprint is wrong"; // the same file was uploaded twice, with
                                                 // differernt paths
    bool found = false;
    for (int i = 0; i < fingerPrintList->size(); i++)
    {
        if (fingerPrintList->get(i)->getHandle() == hfile2)
        {
            found = true;
            break;
        }
    }

    ASSERT_TRUE(found);

    // --- Search by file name for User2 ---

    std::unique_ptr<MegaSearchFilter> filterResults(MegaSearchFilter::createInstance());
    filterResults->byName(fileNameToSearch);
    std::unique_ptr<MegaNodeList> searchList(megaApi[1]->search(filterResults.get()));
    ASSERT_EQ(searchList->size(), 2)
        << "Node count by file name is wrong"; // the same file was uploaded twice, to differernt
                                               // paths
    ASSERT_TRUE(
        (searchList->get(0)->getHandle() == hfile1 && searchList->get(1)->getHandle() == hfile2) ||
        (searchList->get(0)->getHandle() == hfile2 && searchList->get(1)->getHandle() == hfile1))
        << "Node handles are not the expected ones";

    // --- User2 add file ---
    //  |--Shared-folder
    //    |--subfolder
    //      |--by_user_2.txt

    static constexpr char fileByUser2[] = "by_user_2.txt";
    createFile(fileByUser2, false); // not a large file since don't need to test transfers here
    MegaHandle hfile2U2 = 0;
    mApi[1].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(INVALID_HANDLE, MegaNode::CHANGE_TYPE_NEW, check1);
    mApi[0].mOnNodesUpdateCompletion =
        createOnNodesUpdateLambda(INVALID_HANDLE, MegaNode::CHANGE_TYPE_NEW, check2);
    ASSERT_EQ(MegaError::API_OK,
              doStartUpload(1,
                            &hfile2U2,
                            fileByUser2,
                            std::unique_ptr<MegaNode>{megaApi[1]->getNodeByHandle(hfolder2)}.get(),
                            nullptr /*fileName*/,
                            ::mega::MegaApi::INVALID_CUSTOM_MOD_TIME,
                            nullptr /*appData*/,
                            false /*isSourceTemporary*/,
                            false /*startFirst*/,
                            nullptr /*cancelToken*/))
        << "Cannot upload a second test file";

    ASSERT_TRUE(waitForResponse(&check1))
        << "Node update not received on client 1 after " << maxTimeout << " seconds";
    ASSERT_TRUE(waitForResponse(&check2))
        << "Node update not received on client 0 after " << maxTimeout << " seconds";
    // important to reset
    resetOnNodeUpdateCompletionCBs();
    ASSERT_EQ(check1, true);
    ASSERT_EQ(check2, true);

    // --- Check that User1 has received the change ---

    std::unique_ptr<MegaNode> nU2{
        megaApi[0]->getNodeByHandle(hfile2U2)}; // get an updated version of the node
    ASSERT_TRUE(nU2 && string(fileByUser2) == nU2->getName()) << "Finding node by handle failed";

    // --- Locallogout from User1 and login with session ---

    string session = unique_ptr<char[]>(dumpSession()).get();
    locallogout();
    auto tracker = asyncRequestFastLogin(0, session.c_str());
    PerApi& target0 = mApi[0];
    target0.resetlastEvent();
    ASSERT_EQ(API_OK, tracker->waitForResult())
        << " Failed to establish a login/session for account " << 0;
    fetchnodes(0, maxTimeout);
    ASSERT_TRUE(WaitFor(
        [&target0]()
        {
            return target0.lastEventsContain(MegaEvent::EVENT_NODES_CURRENT);
        },
        10000))
        << "Timeout expired to receive actionpackets";

    // --- User1 remove file ---

    ASSERT_EQ(MegaError::API_OK, synchronousRemove(0, nfile2.get()))
        << "Error while removing file " << nfile2->getName();

    // --- Locallogout from User2 and login with session ---

    session = unique_ptr<char[]>(megaApi[1]->dumpSession()).get();

    auto logoutErr = doRequestLocalLogout(1);
    ASSERT_EQ(MegaError::API_OK, logoutErr) << "Local logout failed (error: " << logoutErr << ")";
    PerApi& target1 = mApi[1];
    target1.resetlastEvent(); // clear any previous EVENT_NODES_CURRENT
    auto trackerU2 = asyncRequestFastLogin(1, session.c_str());
    ASSERT_EQ(API_OK, trackerU2->waitForResult())
        << " Failed to establish a login/session for account " << 1;
    fetchnodes(1, maxTimeout);

    // make sure that client is up to date (upon logout, recent changes might not be committed to
    // DB)
    ASSERT_TRUE(WaitFor(
        [&target1]()
        {
            return target1.lastEventsContain(MegaEvent::EVENT_NODES_CURRENT);
        },
        10000))
        << "Timeout expired to receive actionpackets";

    // --- Check that User2 no longer sees the removed file ---

    std::unique_ptr<MegaNode> nremoved{
        megaApi[1]->getNodeByHandle(hfile2)}; // get an updated version of the node
    ASSERT_EQ(nremoved, nullptr) << " Failed to see the file was removed";
}
