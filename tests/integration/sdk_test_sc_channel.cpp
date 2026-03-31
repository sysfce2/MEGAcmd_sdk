/**
 * @file sdk_test_sc_channel.cpp
 * @brief Integration tests for the SC (Server-Client) channel handling
 */

#include "mega/testhooks.h"
#include "SdkTest_test.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <mutex>
#include <vector>

using namespace mega;
using namespace testing;

namespace
{

/**
 * @brief Captured network activity event
 */
struct NetworkActivityEvent
{
    int channel;
    int activityType;
    int errorCode;
};

/**
 * @brief Listener that captures EVENT_NETWORK_ACTIVITY events
 */
class NetworkActivityListener: public MegaListener
{
public:
    void onEvent(MegaApi*, MegaEvent* event) override
    {
        if (!event || event->getType() != MegaEvent::EVENT_NETWORK_ACTIVITY)
        {
            return;
        }

        auto ch = event->getNumber("channel");
        auto at = event->getNumber("activity_type");
        auto ec = event->getNumber("error_code");
        if (!ch || !at || !ec)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(mMutex);
        mEvents.push_back({static_cast<int>(*ch), static_cast<int>(*at), static_cast<int>(*ec)});
    }

    /**
     * @brief Check if any captured event matches the given parameters
     */
    bool hasEvent(int channel, int activityType, int errorCode) const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (const auto& e: mEvents)
        {
            if (e.channel == channel && e.activityType == activityType && e.errorCode == errorCode)
            {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Clear all captured events
     */
    void clear()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mEvents.clear();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mEvents.size();
    }

private:
    mutable std::mutex mMutex;
    std::vector<NetworkActivityEvent> mEvents;
};

} // anonymous namespace

#ifdef MEGASDK_DEBUG_TEST_HOOKS_ENABLED

/**
 * @class SdkTestScChannel
 * @brief Parameterized test fixture for SC channel handling tests.
 *
 * The bool parameter controls the SC action packet parsing mode:
 *   false: non-streaming
 *   true: streaming
 *
 * Every test case runs once in each mode.
 */
class SdkTestScChannel: public SdkTest, public ::testing::WithParamInterface<bool>
{
public:
    void SetUp() override
    {
        SdkTest::SetUp();
        ASSERT_NO_FATAL_FAILURE(getAccountsForTest(1));

        if (GetParam())
        {
            megaApi[0]->getClient()->enableStreaming();
        }
        else
        {
            megaApi[0]->getClient()->disableStreaming();
        }

        megaApi[0]->addListener(&mNetworkListener);
    }

    void TearDown() override
    {
        globalMegaTestHooks.interceptSCRequest = nullptr;
        megaApi[0]->removeListener(&mNetworkListener);
        SdkTest::TearDown();
    }

protected:
    NetworkActivityListener mNetworkListener;

    /**
     * @brief Install test hook to simulate an error response
     */
    void installErrorResponseHook(int error)
    {
        globalMegaTestHooks.interceptSCRequest = [error](std::unique_ptr<HttpReq>& pendingsc)
        {
            LOG_info << "SC channel hook: injecting error response: " << error;
            pendingsc->status = REQ_SUCCESS;
            pendingsc->in = std::to_string(error);
            pendingsc->httpstatus = 200;
            globalMegaTestHooks.interceptSCRequest = nullptr;
        };
    }

    /**
     * @brief Install interceptSCRequest hook to simulate SSL check failure
     */
    void installSslFailureHook()
    {
        globalMegaTestHooks.interceptSCRequest = [](std::unique_ptr<HttpReq>& pendingsc)
        {
            LOG_info << "SC channel hook: injecting SSL check failure";
            pendingsc->status = REQ_FAILURE;
            pendingsc->httpstatus = 500;
            pendingsc->sslcheckfailed = true;
            globalMegaTestHooks.interceptSCRequest = nullptr;
        };
    }

    /**
     * @brief Install interceptSCRequest hook to simulate DNS failure
     */
    void installDnsFailureHook()
    {
        globalMegaTestHooks.interceptSCRequest = [](std::unique_ptr<HttpReq>& pendingsc)
        {
            LOG_info << "SC channel hook: injecting DNS failure";
            pendingsc->status = REQ_FAILURE;
            pendingsc->httpstatus = 0;
            pendingsc->mDnsFailure = true;
            globalMegaTestHooks.interceptSCRequest = nullptr;
        };
    }

    /**
     * @brief Wait until the listener captures all expected events and no others.
     */
    bool waitForNetworkEvents(std::vector<NetworkActivityEvent> expectedEvents,
                              unsigned int timeoutMs,
                              bool requireSizeMatch = true)
    {
        return WaitFor(
            [&, this]()
            {
                if (mNetworkListener.size() < expectedEvents.size())
                {
                    return false; // still waiting for events
                }

                // All expected events must be present
                for (const auto& exp: expectedEvents)
                {
                    if (!mNetworkListener.hasEvent(exp.channel, exp.activityType, exp.errorCode))
                    {
                        return false;
                    }
                }

                // No extra events beyond the expected ones
                return requireSizeMatch ? mNetworkListener.size() == expectedEvents.size() : true;
            },
            timeoutMs);
    }
};

/**
 * @brief Test: Process API_ESID
 */
TEST_P(SdkTestScChannel, ProcessSidError)
{
    CASE_info << "started";

    mNetworkListener.clear();

    installErrorResponseHook(API_ESID);

    // Prepare to detect the logout triggered by request_error(API_ESID)
    mApi[0].requestFlags[MegaRequest::TYPE_LOGOUT] = false;

    // Force SC channel catchup to trigger the hook
    megaApi[0]->catchup();

    // Verify events
    EXPECT_TRUE(waitForNetworkEvents({{MegaEvent::SC, MegaEvent::REQUEST_SENT, API_OK},
                                      {MegaEvent::SC, MegaEvent::REQUEST_RECEIVED, API_ESID}},
                                     defaultTimeoutMs))
        << "Not expected events received";

    // Verify logout should be triggered
    EXPECT_TRUE(waitForResponse(&mApi[0].requestFlags[MegaRequest::TYPE_LOGOUT], defaultTimeoutMs))
        << "Expected logout";

    CASE_info << "finished";
}

/**
 * @brief Test: Process API_ENOENT when logged into a folder link
 */
TEST_P(SdkTestScChannel, ProcessNoEntryErrorInFolderLink)
{
    CASE_info << "started";

    // Create a folder and export it as a public link
    std::unique_ptr<MegaNode> rootNode{megaApi[0]->getRootNode()};
    ASSERT_THAT(rootNode, ::testing::NotNull());

    auto folderHandle = createFolder(0, "TestFolder", rootNode.get());
    ASSERT_NE(folderHandle, UNDEF);

    std::unique_ptr<MegaNode> folderNode{megaApi[0]->getNodeByHandle(folderHandle)};
    ASSERT_THAT(folderNode, ::testing::NotNull());

    const auto folderLink = createPublicLink(0, folderNode.get(), 0, maxTimeout, false);

    // Logout, then re-login via the folder link
    logout(0, false, maxTimeout);

    auto loginTracker = asyncRequestLoginToFolder(0u, folderLink.c_str());
    ASSERT_EQ(loginTracker->waitForResult(), API_OK) << "Failed to login to folder";
    ASSERT_NO_FATAL_FAILURE(fetchnodes(0));

    LOG_info << "Clear network events";
    mNetworkListener.clear();

    installErrorResponseHook(API_ENOENT);

    mApi[0].requestFlags[MegaRequest::TYPE_LOGOUT] = false;

    // Force SC channel catchup to trigger the hook
    megaApi[0]->catchup();

    // Verify events
    EXPECT_TRUE(waitForNetworkEvents({{MegaEvent::SC, MegaEvent::REQUEST_SENT, API_OK},
                                      {MegaEvent::SC, MegaEvent::REQUEST_RECEIVED, API_ENOENT}},
                                     defaultTimeoutMs))
        << "Not expected events received";

    EXPECT_TRUE(waitForResponse(&mApi[0].requestFlags[MegaRequest::TYPE_LOGOUT], defaultTimeoutMs))
        << "Expected logout";

    CASE_info << "finished";
}

/**
 * @brief Test: Process API_ETOOMANY
 */
TEST_P(SdkTestScChannel, ProcessTooManyError)
{
    CASE_info << "started";

    mNetworkListener.clear();
    mApi[0].resetlastEvent();

    installErrorResponseHook(API_ETOOMANY);

    // Force SC channel catchup to trigger the hook
    megaApi[0]->catchup();

    // Verify events
    EXPECT_TRUE(waitForNetworkEvents({{MegaEvent::SC, MegaEvent::REQUEST_SENT, API_OK},
                                      {MegaEvent::SC, MegaEvent::REQUEST_RECEIVED, API_ETOOMANY}},
                                     defaultTimeoutMs))
        << "Not expected events received";

    ASSERT_TRUE(WaitFor(
        [this]()
        {
            return mApi[0].lastEventsContain(MegaEvent::EVENT_RELOADING);
        },
        defaultTimeoutMs))
        << "Expected EVENT_RELOADING";

    CASE_info << "finished";
}

/**
 * @brief Test: Process API_EAGAIN
 */
TEST_P(SdkTestScChannel, ProcessAgainError)
{
    CASE_info << "started";

    mNetworkListener.clear();

    installErrorResponseHook(API_EAGAIN);

    // Force SC channel catchup to trigger the hook
    megaApi[0]->catchup();

    // Verify events
    // The subsequent SC request could trigger another network event, so can't expect the network
    // events number here.
    EXPECT_TRUE(waitForNetworkEvents({{MegaEvent::SC, MegaEvent::REQUEST_SENT, API_OK},
                                      {MegaEvent::SC, MegaEvent::REQUEST_RECEIVED, API_EAGAIN}},
                                     defaultTimeoutMs,
                                     false))
        << "Not expected events received";

    CASE_info << "finished";
}

/**
 * @brief Test: Process API_ERATELIMIT
 */
TEST_P(SdkTestScChannel, ProcessRateLimitError)
{
    CASE_info << "started";

    mNetworkListener.clear();

    installErrorResponseHook(API_ERATELIMIT);

    // Force SC channel catchup to trigger the hook
    megaApi[0]->catchup();

    // Verify events
    // The subsequent SC request could trigger another network event, so can't expect the network
    // events number here.
    EXPECT_TRUE(waitForNetworkEvents({{MegaEvent::SC, MegaEvent::REQUEST_SENT, API_OK},
                                      {MegaEvent::SC, MegaEvent::REQUEST_RECEIVED, API_ERATELIMIT}},
                                     defaultTimeoutMs,
                                     false))
        << "Not expected events received";

    CASE_info << "finished";
}

/**
 * @brief Test: Process API_EBLOCKED
 */
TEST_P(SdkTestScChannel, ProcessBlockedError)
{
    CASE_info << "started";

    mNetworkListener.clear();

    installErrorResponseHook(API_EBLOCKED);

    // Force SC channel catchup to trigger the hook
    megaApi[0]->catchup();

    // Verify events
    // The subsequent SC request could trigger another network event, so can't expect the network
    // events number here.
    EXPECT_TRUE(waitForNetworkEvents({{MegaEvent::SC, MegaEvent::REQUEST_SENT, API_OK},
                                      {MegaEvent::SC, MegaEvent::REQUEST_RECEIVED, API_EBLOCKED}},
                                     defaultTimeoutMs,
                                     false))
        << "Not expected events received";

    EXPECT_TRUE(waitForResponse(&mApi[0].requestFlags[MegaRequest::TYPE_WHY_AM_I_BLOCKED],
                                defaultTimeoutMs))
        << "Not expected events received";

    CASE_info << "finished";
}

/**
 * @brief Test: Process unexpected error code
 */
TEST_P(SdkTestScChannel, ProcessUnexpectedError)
{
    CASE_info << "started";

    mNetworkListener.clear();

    installErrorResponseHook(1);

    // Force SC channel catchup to trigger the hook
    megaApi[0]->catchup();

    // Verify events
    EXPECT_TRUE(waitForNetworkEvents({{MegaEvent::SC, MegaEvent::REQUEST_SENT, API_OK},
                                      {MegaEvent::SC, MegaEvent::REQUEST_ERROR, 1}},
                                     defaultTimeoutMs))
        << "Not expected events received";

    EXPECT_TRUE(megaApi[0]->getClient()->scsn.stopped()) << "Expected SCSN to be stopped";

    CASE_info << "finished";
}

/**
 * @brief Test: Process SSL failure without retry
 */
TEST_P(SdkTestScChannel, ProcessSslFailureWithoutRetry)
{
    CASE_info << "started";

    // Ensure retryessl is disabled (default)
    megaApi[0]->retrySSLerrors(false);

    mNetworkListener.clear();

    installSslFailureHook();

    // Force SC channel catchup to trigger the hook
    megaApi[0]->catchup();

    // Verify events
    EXPECT_TRUE(waitForNetworkEvents({{MegaEvent::SC, MegaEvent::REQUEST_SENT, API_OK},
                                      {MegaEvent::SC, MegaEvent::REQUEST_ERROR, API_ESSL}},
                                     defaultTimeoutMs))
        << "Not expected events received";

    // Verify logout should be triggered
    EXPECT_TRUE(waitForResponse(&mApi[0].requestFlags[MegaRequest::TYPE_LOGOUT], defaultTimeoutMs))
        << "Expected logout";

    EXPECT_TRUE(megaApi[0]->getClient()->sslfakeissuer.empty())
        << "Expected sslfakeissuer to be cleared";

    CASE_info << "finished";
}

/**
 * @brief Test: Process SSL failure with retry
 */
TEST_P(SdkTestScChannel, ProcessSslFailureWithRetry)
{
    CASE_info << "started";

    // Enable retryessl
    megaApi[0]->retrySSLerrors(true);

    mNetworkListener.clear();

    installSslFailureHook();

    // Force SC channel catchup to trigger the hook
    megaApi[0]->catchup();

    // Verify events
    // The subsequent SC request could trigger another network event, so can't expect the network
    // events number here.
    EXPECT_TRUE(waitForNetworkEvents({{MegaEvent::SC, MegaEvent::REQUEST_SENT, API_OK},
                                      {MegaEvent::SC, MegaEvent::REQUEST_RECEIVED, 500}},
                                     defaultTimeoutMs,
                                     false))
        << "Not expected events received";

    EXPECT_TRUE(megaApi[0]->getClient()->sslfakeissuer.empty())
        << "Expected sslfakeissuer to be cleared";

    CASE_info << "finished";
}

/**
 * @brief Test: Process DNS failure
 */
TEST_P(SdkTestScChannel, ProcessDnsFailure)
{
    CASE_info << "started";

    // Enable retryessl
    megaApi[0]->retrySSLerrors(true);

    mNetworkListener.clear();

    installDnsFailureHook();

    // Force SC channel catchup to trigger the hook
    megaApi[0]->catchup();

    // Verify events
    // The subsequent SC request could trigger another network event, so can't expect the network
    // events number here.
    EXPECT_TRUE(waitForNetworkEvents({{MegaEvent::SC, MegaEvent::REQUEST_SENT, API_OK},
                                      {MegaEvent::SC, MegaEvent::REQUEST_SENT, LOCAL_ENETWORK}},
                                     defaultTimeoutMs,
                                     false))
        << "Not expected events received";

    CASE_info << "finished";
}

INSTANTIATE_TEST_SUITE_P(ScChannel,
                         SdkTestScChannel,
                         ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info)
                         {
                             return info.param ? "Streaming" : "NonStreaming";
                         });

#endif // MEGASDK_DEBUG_TEST_HOOKS_ENABLED
