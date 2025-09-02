/**
 * @file
 * @brief This file contains definition of convenient mock listeners
 */

#ifndef INCLUDE_INTEGRATION_MOCK_LISTENERS_H_
#define INCLUDE_INTEGRATION_MOCK_LISTENERS_H_

#include "integration/integration_test_utils.h"
#include "megaapi.h"

#include <gmock/gmock.h>

#include <chrono>
#include <future>

/**
 * @class SynchronizationHelper
 * @brief A helper class to implement mock classes involved in async operations.
 *
 * In summary, the class has a promise and a method (markAsFinished) that allows to set the value of
 * the promise. Then, it provides method for waiting for the promise to finish (waitForFinish and
 * waitForFinishOrTimeout).
 *
 */
class SynchronizationHelper
{
public:
    SynchronizationHelper():
        finishedFuture(finishedPromise.get_future())
    {}

    void waitForFinish()
    {
        finishedFuture.wait();
    }

    /**
     * @brief Waits for the promise to finish for the given amount of time.
     *
     * @param duration The time to wait. You can use std::chrono literals such as 3min
     * @return true if the promise finishes successfully (see `markAsFinished`) within the given
     * duration, false otherwise.
     */
    template<typename Rep, typename Period>
    bool waitForFinishOrTimeout(const std::chrono::duration<Rep, Period>& duration)
    {
        auto status = finishedFuture.wait_for(duration);
        return status == std::future_status::ready && finishedFuture.get();
    }

    void markAsFinished(const bool succeeded = true)
    {
        finishedPromise.set_value(succeeded);
    }

private:
    std::promise<bool> finishedPromise;
    std::future<bool> finishedFuture;
};

/**
 * @class MockRequestListener
 * @brief A mock class for MegaRequestListener
 *
 * By default, the underlying promise is marked as finished once the onRequestFinish method gets
 * called so remember to wait for finish before going out of scope.
 */
class MockRequestListener: public ::mega::MegaRequestListener, public SynchronizationHelper
{
public:
    MockRequestListener(::mega::MegaApi* megaApi = nullptr):
        mMegaApi{megaApi}
    {
        ON_CALL(*this, onRequestFinish)
            .WillByDefault(::testing::Invoke(this, &MockRequestListener::defaultOnRequestFinish));
    }

    ~MockRequestListener()
    {
        if (mMegaApi)
            mMegaApi->removeRequestListener(this);
    }

    MOCK_METHOD(void,
                onRequestStart,
                (::mega::MegaApi * api, ::mega::MegaRequest* request),
                (override));
    MOCK_METHOD(void,
                onRequestUpdate,
                (::mega::MegaApi * api, ::mega::MegaRequest* request),
                (override));
    MOCK_METHOD(void,
                onRequestTemporaryError,
                (::mega::MegaApi * api, ::mega::MegaRequest* request, ::mega::MegaError* error),
                (override));

    MOCK_METHOD(void,
                onRequestFinish,
                (::mega::MegaApi*, ::mega::MegaRequest*, ::mega::MegaError*),
                (override));

    /**
     * @brief Set expectations on the error codes and request type when onRequestFinish gets
     * invoked
     *
     * @param reqErrorMatcher A matcher for the value returned by MegaRequest::getErrorCode
     * @param syncErrorMatcher A matcher for the value returned by MegaRequest::getSyncError. It
     * will match any error by default.
     * @param reqTypeMatcher A matcher for the value returned by MegaRequest::getType. It
     * will match any type by default.
     */
    void setErrorExpectations(
        const testing::Matcher<int> reqErrorMatcher,
        const testing::Matcher<int> syncErrorMatcher = testing::_,
        const testing::Matcher<int> reqTypeMatcher = testing::_,
        std::function<void(const ::mega::MegaRequest&)>&& captureValues = nullptr)
    {
        // We override the default
        using namespace testing;
        EXPECT_CALL(*this, onRequestFinish)
            .Times(1)
            .WillOnce(
                [reqErrorMatcher,
                 syncErrorMatcher,
                 reqTypeMatcher,
                 capture = std::move(captureValues),
                 this](::mega::MegaApi*, ::mega::MegaRequest* req, ::mega::MegaError* err)
                {
                    EXPECT_TRUE(req) << "Null MegaRequest";
                    EXPECT_TRUE(err) << "Null MegaError";
                    if (!req || !err)
                        return markAsFinished(false);

                    bool ok = sdk_test::checkAndExpectThat(req->getType(), reqTypeMatcher);
                    ok = ok && sdk_test::checkAndExpectThat(err->getErrorCode(), reqErrorMatcher);
                    ok = ok && sdk_test::checkAndExpectThat(err->getSyncError(), syncErrorMatcher);
                    if (ok && capture)
                        capture(*req);
                    markAsFinished(ok);
                });
    }

private:
    ::mega::MegaApi* mMegaApi{nullptr};

    void defaultOnRequestFinish(::mega::MegaApi*, ::mega::MegaRequest*, ::mega::MegaError* err)
    {
        markAsFinished(err->getErrorCode() == ::mega::API_OK);
    }
};

/**
 * @class MockNodesUpdateListener
 * @brief A simple mock listener to track nodes updates
 */
class MockNodesUpdateListener: public ::mega::MegaListener
{
public:
    MockNodesUpdateListener(::mega::MegaApi* megaApi = nullptr):
        mMegaApi{megaApi}
    {}

    MOCK_METHOD(void,
                onNodesUpdate,
                (::mega::MegaApi * api, ::mega::MegaNodeList* nodes),
                (override));

    ~MockNodesUpdateListener()
    {
        if (mMegaApi)
            mMegaApi->removeListener(this);
    }

private:
    ::mega::MegaApi* mMegaApi{nullptr};
};

/**
 * @class MockSyncListener
 * @brief Mock listener only implementing methods for transfers
 *
 */
class MockTransferListener: public ::mega::MegaListener
{
public:
    MockTransferListener(::mega::MegaApi* megaApi = nullptr):
        mMegaApi{megaApi}
    {}

    ~MockTransferListener()
    {
        if (mMegaApi)
            mMegaApi->removeListener(this);
    }

    MOCK_METHOD(void,
                onTransferFinish,
                (::mega::MegaApi * api, ::mega::MegaTransfer* transfer, ::mega::MegaError* error),
                (override));
    MOCK_METHOD(void,
                onTransferStart,
                (::mega::MegaApi * api, ::mega::MegaTransfer* transfer),
                (override));
    MOCK_METHOD(void,
                onTransferUpdate,
                (::mega::MegaApi * api, ::mega::MegaTransfer* transfer),
                (override));
    MOCK_METHOD(void,
                onTransferTemporaryError,
                (::mega::MegaApi * api, ::mega::MegaTransfer* transfer, ::mega::MegaError* error),
                (override));

private:
    ::mega::MegaApi* mMegaApi{nullptr};
};

class MockMegaTransferListener: public ::mega::MegaTransferListener, public SynchronizationHelper
{
public:
    MockMegaTransferListener(::mega::MegaApi* megaApi = nullptr):
        mMegaApi{megaApi}
    {
        ON_CALL(*this, onTransferFinish)
            .WillByDefault(
                ::testing::Invoke(this, &MockMegaTransferListener::defaultOnTransferFinish));
    }

    ~MockMegaTransferListener()
    {
        if (mMegaApi)
            mMegaApi->removeTransferListener(this);
    }

    MOCK_METHOD(void,
                onTransferStart,
                (::mega::MegaApi * api, ::mega::MegaTransfer* transfer),
                (override));
    MOCK_METHOD(void,
                onTransferFinish,
                (::mega::MegaApi * api, ::mega::MegaTransfer* transfer, ::mega::MegaError* error),
                (override));
    MOCK_METHOD(void,
                onTransferUpdate,
                (::mega::MegaApi * api, ::mega::MegaTransfer* transfer),
                (override));
    MOCK_METHOD(void,
                onFolderTransferUpdate,
                (::mega::MegaApi * api,
                 ::mega::MegaTransfer* transfer,
                 int stage,
                 uint32_t foldercount,
                 uint32_t createdfoldercount,
                 uint32_t filecount,
                 const char* currentFolder,
                 const char* currentFileLeafname),
                (override));
    MOCK_METHOD(void,
                onTransferTemporaryError,
                (::mega::MegaApi * api, ::mega::MegaTransfer* transfer, ::mega::MegaError* error),
                (override));
    MOCK_METHOD(bool,
                onTransferData,
                (::mega::MegaApi * api, ::mega::MegaTransfer* transfer, char* buffer, size_t size),
                (override));

private:
    ::mega::MegaApi* mMegaApi;

    void defaultOnTransferFinish(::mega::MegaApi*, ::mega::MegaTransfer*, ::mega::MegaError* err)
    {
        markAsFinished(err->getErrorCode() == ::mega::API_OK);
    }
};

#ifdef ENABLE_SYNC
/**
 * @class MockSyncListener
 * @brief Mock listener only implementing methods for syncs
 *
 */
class MockSyncListener: public ::mega::MegaListener
{
public:
    MockSyncListener(::mega::MegaApi* megaApi = nullptr):
        mMegaApi{megaApi}
    {}

    ~MockSyncListener()
    {
        if (mMegaApi)
            mMegaApi->removeListener(this);
    }

    MOCK_METHOD(
        void,
        onSyncFileStateChanged,
        (::mega::MegaApi * api, ::mega::MegaSync* sync, std::string* localPath, int newState),
        (override));
    MOCK_METHOD(void, onSyncAdded, (::mega::MegaApi * api, ::mega::MegaSync* sync), (override));
    MOCK_METHOD(void, onSyncDeleted, (::mega::MegaApi * api, ::mega::MegaSync* sync), (override));
    MOCK_METHOD(void,
                onSyncStateChanged,
                (::mega::MegaApi * api, ::mega::MegaSync* sync),
                (override));
    MOCK_METHOD(void,
                onSyncStatsUpdated,
                (::mega::MegaApi * api, ::mega::MegaSyncStats* syncStats),
                (override));
    MOCK_METHOD(void, onGlobalSyncStateChanged, (::mega::MegaApi * api), (override));
    MOCK_METHOD(void,
                onSyncRemoteRootChanged,
                (::mega::MegaApi * api, ::mega::MegaSync* sync),
                (override));
    MOCK_METHOD(void,
                onRequestFinish,
                (::mega::MegaApi*, ::mega::MegaRequest*, ::mega::MegaError*),
                (override));

private:
    ::mega::MegaApi* mMegaApi{nullptr};
};
#endif // ENABLE_SYNC

#endif // INCLUDE_INTEGRATION_MOCK_LISTENERS_H_
