#ifndef SDK_TEST_SHARES_H
#define SDK_TEST_SHARES_H

#include "SdkTest_test.h"

class SdkTestShares: public SdkTest
{
protected:
    void SetUp() override;

    void TearDown() override;

    void createNodeTrees();

    MegaHandle getHandle(const std::string& path) const;

    void verifyCredentials(unsigned sharerIndex,
                           const PerApi* sharer,
                           unsigned shareeIndex,
                           const PerApi* sharee);

    void createNewContactAndVerify();

    void createOutgoingShare(MegaHandle hfolder);

    void getInshare(MegaHandle hfolder);

    void createOnePublicLink(MegaHandle hfolder, std::string& nodeLink);

    void importPublicLink(const std::string& nodeLink, MegaHandle* importedNodeHandle = nullptr);

    void revokeOutShares(MegaHandle hfolder);

    void revokePublicLink(MegaHandle hfolder);

    /**
     * @brief Makes a copy of the node located at the sourceNodePath path in the inshare folder of
     * the mSharee account and puts it in destNodeName inside the mSharee root node
     *
     * NOTE: This method uses ASSERT_* macros
     * NOTE: This method assumes you have called the getInshare method
     */
    void copyNode(const unsigned int accountId,
                  const MegaHandle sourceNodeHandle,
                  const MegaHandle destNodeHandle,
                  const std::string& destName,
                  MegaHandle* copiedNodeHandle = nullptr);

    /**
     * @brief Same as copySharedFolderToOwnCloud but invoking move instead of copy on sourceNodePath
     */
    void moveNodeToOwnCloud(const std::string& sourceNodePath,
                            const std::string& destNodeName,
                            MegaHandle* copiedNodeHandle = nullptr);

    std::unordered_map<std::string, MegaHandle> mHandles;

    // Sharer account
    static constexpr unsigned mSharerIndex{0};

    PerApi* mSharer{nullptr};

    MegaApiTest* mSharerApi{nullptr};

    // Sharee account
    static constexpr unsigned mShareeIndex{1};

    PerApi* mSharee{nullptr};

    MegaApiTest* mShareeApi{nullptr};

    // Guest account
    static constexpr unsigned mGuestIndex{2};

    PerApi* mGuest{nullptr};

    MegaApiTest* mGuestApi{nullptr};

    std::string mGuestEmail;

    std::string mGuestPass;
};
#endif // SDK_TEST_SHARES_H
