#include "mega/megaapp.h"
#include "mega/megaclient.h"
#include "mega/testhooks.h"
#include "sdk_test_utils.h"
#include "utils.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace mega;
using namespace sdk_test;

namespace
{
class MegaClientTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        app = std::make_shared<MegaApp>();
        client = mt::makeClient(*app);
    }

    void TearDown() override
    {
        client.reset();
        app.reset();
    }

    std::shared_ptr<MegaApp> app;
    std::shared_ptr<MegaClient> client;
    handle testHandle = 0x1234;

#ifdef MEGASDK_DEBUG_TEST_HOOKS_ENABLED
    HttpReq* setPendingScResponse(const std::string& payload)
    {
        HttpReq* req = new HttpReq;
        req->in = payload;
        req->contentlength = (m_off_t)payload.size();

        client->megaTestHooks.interceptSCRequest =
            [this, payload, req](std::unique_ptr<HttpReq>& pendingsc)
        {
            pendingsc.reset(req);

            this->client->megaTestHooks.interceptSCRequest = nullptr;
        };

        return req;
    }
#endif
};

TEST_F(MegaClientTest, isValidLocalSyncRoot_OK)
{
    const fs::path dirPath = fs::current_path() / "megaclient_test_valid_local_sync_root";
    LocalTempDir tempDir(dirPath);
    LocalPath localPath = LocalPath::fromAbsolutePath(path_u8string(dirPath));
    const auto [err, sErr, sWarn] = client->isValidLocalSyncRoot(localPath, testHandle);
    EXPECT_EQ(err, API_OK);
    EXPECT_EQ(sErr, NO_SYNC_ERROR);
    EXPECT_EQ(sWarn, NO_SYNC_WARNING);
}

TEST_F(MegaClientTest, isValidLocalSyncRoot_NotAbsolutePath)
{
    const fs::path relPath = fs::path("relative") / "path" / "to" / "dir";
    LocalPath localPath = LocalPath::fromRelativePath(path_u8string(relPath));
    const auto [err, sErr, sWarn] = client->isValidLocalSyncRoot(localPath, testHandle);
    EXPECT_EQ(err, API_EARGS);
    EXPECT_EQ(sErr, NO_SYNC_ERROR);
    EXPECT_EQ(sWarn, NO_SYNC_WARNING);
}

TEST_F(MegaClientTest, isValidLocalSyncRoot_NonExistentPath)
{
    const fs::path dirPath = fs::current_path() / "megaclient_test_non_existent_path";
    LocalPath localPath = LocalPath::fromAbsolutePath(path_u8string(dirPath));
    const auto [err, sErr, sWarn] = client->isValidLocalSyncRoot(localPath, testHandle);
    EXPECT_EQ(err, API_ENOENT);
    EXPECT_EQ(sErr, LOCAL_PATH_UNAVAILABLE);
    EXPECT_EQ(sWarn, NO_SYNC_WARNING);
}

TEST_F(MegaClientTest, isValidLocalSyncRoot_NotAFolder)
{
    const fs::path filePath = fs::current_path() / "megaclient_test_not_a_folder.txt";
    LocalTempFile tempFile(filePath, "Temporary file content");
    LocalPath localPath = LocalPath::fromAbsolutePath(path_u8string(filePath));
    const auto [err, sErr, sWarn] = client->isValidLocalSyncRoot(localPath, testHandle);
    EXPECT_EQ(err, API_EACCESS);
    EXPECT_EQ(sErr, INVALID_LOCAL_TYPE);
    EXPECT_EQ(sWarn, NO_SYNC_WARNING);
}

#ifdef MEGASDK_DEBUG_TEST_HOOKS_ENABLED
TEST_F(MegaClientTest, chooseScParsingMode_EnableAndDisableStreaming)
{
    // Default: disabled
    ASSERT_FALSE(client->isStreamingEnabled());

    // Enable streaming
    HttpReq* pendingScHolder = setPendingScResponse(R"({"apm":0,"a":[{}]})");
    client->chooseScParsingMode();

    EXPECT_TRUE(client->isStreamingEnabled());
    EXPECT_TRUE(pendingScHolder->mChunked);

    // Disable streaming
    pendingScHolder = setPendingScResponse(R"({"apm":1,"a":[{}]})");
    client->chooseScParsingMode();

    EXPECT_FALSE(client->isStreamingEnabled());
    EXPECT_FALSE(pendingScHolder->mChunked);
}

TEST_F(MegaClientTest, chooseScParsingMode_EnableStreamingWhenNoApm)
{
    // Default: disabled
    ASSERT_FALSE(client->isStreamingEnabled());

    // Enable streaming
    HttpReq* pendingScHolder = setPendingScResponse(R"({"a":[{}]})");
    client->chooseScParsingMode();

    EXPECT_TRUE(client->isStreamingEnabled());
    EXPECT_TRUE(pendingScHolder->mChunked);
}

TEST_F(MegaClientTest, chooseScParsingMode_DoesNothingForNonStartPayload)
{
    // Default: disabled
    ASSERT_FALSE(client->isStreamingEnabled());

    // Enable streaming
    HttpReq* pendingScHolder = setPendingScResponse(R"(["a":[{}]])");
    client->chooseScParsingMode();

    // No change
    EXPECT_FALSE(client->isStreamingEnabled());
    EXPECT_FALSE(pendingScHolder->mChunked);
}

TEST_F(MegaClientTest, chooseScParsingMode_DoesNothingForShortPayload)
{
    // Default: disabled
    ASSERT_FALSE(client->isStreamingEnabled());

    // Enable streaming
    HttpReq* pendingScHolder = setPendingScResponse(R"({"apm":)");
    client->chooseScParsingMode();

    // No change
    EXPECT_FALSE(client->isStreamingEnabled());
    EXPECT_FALSE(pendingScHolder->mChunked);
}
#endif

} // namespace