#include <mega/common/error_or.h>
#include <mega/common/node_info.h>
#include <mega/common/utility.h>
#include <mega/file_service/file.h>
#include <mega/file_service/file_event.h>
#include <mega/file_service/file_event_vector.h>
#include <mega/file_service/file_id.h>
#include <mega/file_service/file_info.h>
#include <mega/file_service/file_location.h>
#include <mega/file_service/file_range.h>
#include <mega/file_service/file_read_result.h>
#include <mega/file_service/file_result.h>
#include <mega/file_service/file_result_or.h>
#include <mega/file_service/file_service_options.h>
#include <mega/file_service/file_service_result.h>
#include <mega/file_service/file_service_result_or.h>
#include <mega/file_service/file_write_result.h>
#include <mega/file_service/logging.h>
#include <mega/file_service/scoped_file_event_observer.h>
#include <mega/file_service/source.h>
#include <mega/fuse/common/testing/client.h>
#include <mega/fuse/common/testing/cloud_path.h>
#include <mega/fuse/common/testing/file.h>
#include <mega/fuse/common/testing/path.h>
#include <mega/fuse/common/testing/test.h>
#include <mega/fuse/common/testing/utility.h>

#include <cinttypes>

namespace mega
{
namespace file_service
{

// Teach gtest how to print FileEvent instances.
void PrintTo(const FileEvent& event, std::ostream* ostream)
{
    // Assume the event holds no range.
    std::string range = "[]";

    // Event actually holds a range.
    if (event.mRange)
        range = toString(*event.mRange);

    // Print the range in a form we can understand.
    *ostream << "{" << range << ", " << event.mModified << ", " << event.mSize << "}";
}

// Check whether lhs and rhs represent the same event.
bool operator==(const FileEvent& lhs, const FileEvent& rhs)
{
    return lhs.mRange == rhs.mRange && lhs.mModified == rhs.mModified && lhs.mSize == rhs.mSize;
}

// Check whether lhs and rhs represent different events.
bool operator!=(const FileEvent& lhs, const FileEvent& rhs)
{
    return !(lhs == rhs);
}

namespace testing
{

// Convenience.
using common::Expected;
using common::makeSharedPromise;
using common::now;
using common::unexpected;
using fuse::testing::ClientPtr;
using fuse::testing::randomBytes;
using fuse::testing::randomName;
using fuse::testing::waitFor;
using ::testing::ElementsAre;

template<typename T>
struct GenerateFailure;

template<typename E, typename T>
struct GenerateFailure<Expected<E, T>>
{
    static auto value()
    {
        return unexpected(GenerateFailure<E>::value());
    }
}; // GenerateFailure<Expected<E, T>>

template<>
struct GenerateFailure<FileResult>
{
    static auto value()
    {
        return FILE_FAILED;
    }
}; // GenerateFailure<FileResult>

template<>
struct GenerateFailure<FileServiceResult>
{
    static auto value()
    {
        return FILE_SERVICE_UNEXPECTED;
    }
}; // GenerateFailure<FileServiceResult>

// Convenience.
constexpr auto timeout = std::future_status::timeout;

struct FileServiceTests: fuse::testing::Test
{
    // Perform instance-specific setup.
    void SetUp() override;

    // Perform fixture-wide setup.
    static void SetUpTestSuite();

    // Execute an asynchronous request synchronously.
    template<typename Function, typename... Parameters>
    auto execute(Function&& function, Parameters&&... arguments)
    {
        // Execute function to kick off our request.
        auto waiter =
            std::invoke(std::forward<Function>(function), std::forward<Parameters>(arguments)...);

        // Figure out the function's result type.
        using Result = decltype(waiter.get());

        // Request timed out.
        if (waiter.wait_for(std::chrono::minutes(60)) == timeout)
            return Result(GenerateFailure<Result>::value());

        // Return result to our caller.
        return waiter.get();
    }

    // The content of the file we want to read.
    static std::string mFileContent;

    // The handle of the file we want to read.
    static NodeHandle mFileHandle;

    // The name of the file we want to read.
    static std::string mFileName;

    // The handle of our test root directory.
    static NodeHandle mRootHandle;
}; // FUSEPartialDownloadTests

// For clarity.
constexpr std::uint64_t operator""_KiB(unsigned long long value)
{
    return value * 1024;
}

constexpr std::uint64_t operator""_MiB(unsigned long long value)
{
    return value * 1024_KiB;
}

// Append content to the end of the specified file.
static auto append(const void* buffer, File file, std::uint64_t length) -> std::future<FileResult>;

// Compare content.
static bool compare(const std::string& computed,
                    const std::string& expected,
                    std::uint64_t offset,
                    std::uint64_t length);

// Fetch all of a file's content from the cloud.
static auto fetch(File file) -> std::future<FileResult>;

// Flush a file's modified content to the cloud.
static auto flush(File file) -> std::future<FileResult>;

// Read some content from the specified file.
static auto read(File file, std::uint64_t offset, std::uint64_t length)
    -> std::future<FileResultOr<std::string>>;

// Reclaim a file's storage.
static auto reclaim(File file) -> std::future<FileResultOr<std::uint64_t>>;

// Reclaim zero or more files managed by client.
static auto reclaimAll(ClientPtr& client) -> std::future<FileServiceResultOr<std::uint64_t>>;

// Remove a file.
static auto remove(File file) -> std::future<FileResult>;

// Update the specified file's modification time.
static auto touch(File file, std::int64_t modified) -> std::future<FileResult>;

// Truncate the specified file to a particular size.
static auto truncate(File file, std::uint64_t size) -> std::future<FileResult>;

// Write some content to the specified file.
static auto write(const void* buffer, File file, std::uint64_t offset, std::uint64_t length)
    -> std::future<FileResult>;

std::string FileServiceTests::mFileContent;

NodeHandle FileServiceTests::mFileHandle;

std::string FileServiceTests::mFileName;

NodeHandle FileServiceTests::mRootHandle;

static const FileServiceOptions DefaultOptions;

static const FileServiceOptions DisableReadahead = {
    DefaultOptions.mMaximumRangeRetries,
    0u,
    0u,
    DefaultOptions.mRangeRetryBackoff,
    DefaultOptions.mReclaimAgeThreshold,
    DefaultOptions.mReclaimBatchSize,
    DefaultOptions.mReclaimDelay,
    DefaultOptions.mReclaimPeriod,
    DefaultOptions.mReclaimSizeThreshold}; // DisableReadahead

TEST_F(FileServiceTests, DISABLED_measure_average_linear_read_time)
{
    // How large should the test file be?
    constexpr auto fileSize = 16_MiB;

    // How many samples should we perform?
    constexpr auto numSamples = 10ul;

    // How large should each individual read be?
    constexpr auto readSize = 8_KiB;

    // Try and create a test file for us to read from.
    auto handle = ClientW()->upload(randomBytes(fileSize), randomName(), mRootHandle);

    // Make sure we could create our test file.
    ASSERT_EQ(handle.errorOr(API_OK), API_OK);

    // Average read time for the entire file.
    std::uint64_t averageFileReadTime = 0ul;

    // Average read time for each range.
    std::uint64_t averageRangeReadTime = 0ul;

    // Figure out how long it takes to linearly read all our file.
    for (auto i = 0ul; i < numSamples; ++i)
    {
        // Try and open our file for reading.
        auto file = ClientW()->fileOpen(*handle);

        // Make sure we could open our file.
        ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

        // Linearly read the entire file.
        for (auto offset = 0ul; offset < fileSize; offset += readSize)
        {
            // Convenience.
            using std::chrono::duration_cast;
            using std::chrono::milliseconds;
            using std::chrono::steady_clock;

            // Track when this read began.
            auto began = steady_clock::now();

            // Try and read some data.
            auto data = execute(read, *file, offset, readSize);

            // Track when the read finished.
            auto elapsed = steady_clock::now() - began;

            // Make sure the read succeeded.
            ASSERT_EQ(data.errorOr(FILE_SUCCESS), FILE_SUCCESS);

            // Convenience.
            auto elapsedMs =
                static_cast<std::uint64_t>(duration_cast<milliseconds>(elapsed).count());

            // For curiosity.
            FSDebugF("Range read time: %s: %" PRIi64 " millisecond(s).",
                     toString(FileRange(offset, offset + readSize)).c_str(),
                     elapsedMs);

            // Make sure our measurements don't overflow.
            ASSERT_GE(UINT64_MAX - elapsedMs, averageFileReadTime);
            ASSERT_GE(UINT64_MAX - elapsedMs, averageRangeReadTime);

            // Update our measurements.
            averageFileReadTime += elapsedMs;
            averageRangeReadTime += elapsedMs;
        }
    }

    // Calculate the average time it took to read the entire file.
    averageFileReadTime /= numSamples;

    // Calculate the average time it took to read each range.
    averageRangeReadTime /= (fileSize / readSize) * numSamples;

    // Log our findings.
    FSDebugF("Average linear file read time: %" PRIu64 " millisecond(s)", averageFileReadTime);
    FSDebugF("Average linear range read time: %" PRIu64 " millisecond(s)", averageRangeReadTime);
}

TEST_F(FileServiceTests, append_succeeds)
{
    // Disable readahead.
    ClientW()->fileServiceOptions(DisableReadahead);

    // Open file for writing.
    auto file = ClientW()->fileOpen(mFileHandle);

    // Make sure we could open the file.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Get our hands on the file's attributes.
    auto info = file->info();

    // Convenience.
    FileRange range(info.size() - 64_KiB, info.size() - 32_KiB);

    // Read some data just before the end of the file.
    {
        // Convenience.
        auto offset = range.mBegin;
        auto length = range.mEnd - range.mBegin;

        // Perform the read.
        auto result = execute(read, *file, offset, length);

        // Make sure the read completed successfully.
        ASSERT_EQ(result.errorOr(FILE_SUCCESS), FILE_SUCCESS);
        ASSERT_EQ(result->size(), static_cast<std::size_t>(length));

        // Reads shouldn't dirty a file.
        ASSERT_FALSE(info.dirty());
    }

    // Events that we expect to receive.
    FileEventVector expected;

    // Events that we actually received.
    FileEventVector received;

    // Store events emitted for our file.
    auto observer = observe(
        [&received](auto& event)
        {
            received.emplace_back(event);
        },
        *file);

    // Convenience.
    using fuse::testing::randomBytes;

    // Generate some data for us to append to the file.
    auto computed = randomBytes(32_KiB);

    // Latch the file's access time, modification time and size.
    auto modified = info.modified();
    auto size = info.size();

    // Try and append the data to the end of the file.
    ASSERT_EQ(execute(append, computed.data(), *file, computed.size()), FILE_SUCCESS);

    expected.emplace_back(FileEvent{FileRange(size, size + computed.size()),
                                    info.modified(),
                                    size + computed.size()});

    // The file should now have two ranges.
    ASSERT_THAT(file->ranges(), ElementsAre(range, FileRange(size, size + computed.size())));

    // Make sure the file's attributes have been updated.
    ASSERT_TRUE(info.dirty());
    ASSERT_GE(info.accessed(), modified);
    ASSERT_GE(info.modified(), modified);
    ASSERT_EQ(info.size(), size + computed.size());

    // Latch current modification time and size.
    modified = info.modified();
    size = info.size();

    // Append again to make sure contigous ranges are extended.
    ASSERT_EQ(execute(append, computed.data(), *file, computed.size()), FILE_SUCCESS);

    expected.emplace_back(FileEvent{FileRange(size, size + computed.size()),
                                    info.modified(),
                                    size + computed.size()});

    ASSERT_GE(info.modified(), modified);
    ASSERT_EQ(info.size(), size + computed.size());

    ASSERT_THAT(file->ranges(),
                ElementsAre(range, FileRange(size - computed.size(), size + computed.size())));

    // Make sure we received the events we expected.
    ASSERT_EQ(expected, received);
}

TEST_F(FileServiceTests, create_fails_when_file_already_exists)
{
    // Generate a name for a local file.
    auto name = randomName();

    // Create a local file for us to collide with.
    auto file0 = ClientW()->fileCreate(mRootHandle, name);
    ASSERT_EQ(file0.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // You can't create a file if it already exists in the cloud.
    auto file1 = ClientW()->fileCreate(mRootHandle, mFileName);
    ASSERT_EQ(file1.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_FILE_ALREADY_EXISTS);

    // You can't create a file if it already exists locally.
    file1 = ClientW()->fileCreate(mRootHandle, name);
    ASSERT_EQ(file1.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_FILE_ALREADY_EXISTS);
}

TEST_F(FileServiceTests, create_fails_when_name_is_empty)
{
    // Files must have a valid name.
    auto file = ClientW()->fileCreate(mRootHandle, std::string());
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_INVALID_NAME);
}

TEST_F(FileServiceTests, create_fails_when_parent_doesnt_exist)
{
    // Files must have a valid parent.
    auto file = ClientW()->fileCreate(NodeHandle(), randomName());
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_PARENT_DOESNT_EXIST);
}

TEST_F(FileServiceTests, create_fails_when_parent_is_a_file)
{
    // A file's parent must be a directory.
    auto file = ClientW()->fileCreate(mFileHandle, randomName());
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_PARENT_IS_A_FILE);
}

TEST_F(FileServiceTests, create_flush_succeeds)
{
    // Generate a name for our file.
    auto name = randomName();

    // Create a new file.
    auto file = ClientW()->fileCreate(mRootHandle, name);

    // Make sure the file was created.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Generate some data for us to write to the file.
    auto expected = randomBytes(128_KiB);

    // Write data to the file.
    ASSERT_EQ(execute(write, expected.data(), *file, 0, 128_KiB), FILE_SUCCESS);

    // Try and flush the file to the cloud.
    auto handle = [file = std::move(file), this]() -> FileResultOr<NodeHandle>
    {
        // Convenience.
        auto info = file->info();

        // Latch the file's access and modification time.
        auto accessed = info.accessed();
        auto modified = info.modified();

        // Try and flush the file to the cloud.
        auto result = execute(flush, *file);

        // Couldn't flush the file to the cloud.
        if (result != FILE_SUCCESS)
            return unexpected(result);

        // Make sure the file's access time has been bumped.
        EXPECT_GE(info.accessed(), accessed);

        // And that the file's modification time is unchanged.
        EXPECT_EQ(info.modified(), modified);

        // Return the file's handle.
        return file->info().handle();
    }();

    // Make sure we were able to flush the file to the cloud.
    ASSERT_EQ(handle.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    // Check that the file exists in the cloud.
    auto node = ClientW()->get(mRootHandle, name);
    ASSERT_EQ(node.errorOr(API_OK), API_OK);
    ASSERT_EQ(node->mHandle, *handle);

    // Reopen the file.
    file = ClientW()->fileOpen(*handle);

    // Make sure the file was opened successfully.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Make sure the data we uploaded was the data we wrote.
    auto computed = execute(read, *file, 0, 128_KiB);

    ASSERT_EQ(computed.errorOr(FILE_SUCCESS), FILE_SUCCESS);
    ASSERT_EQ(*computed, expected);
}

TEST_F(FileServiceTests, create_succeeds)
{
    FileID id0;

    // Create a file and latch its ID.
    {
        // Try and create a file.
        auto file = ClientW()->fileCreate(mRootHandle, randomName());

        // Make sure the file was created.
        ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

        // Make sure we can get information about the file.
        auto info0 = file->info();
        auto info1 = ClientW()->fileInfo(info0.id());

        ASSERT_EQ(info1.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);
        ASSERT_EQ(info0, *info1);

        // Make sure the file isn't associated with any node.
        EXPECT_TRUE(info0.handle().isUndef());

        // And that the file's empty.
        EXPECT_EQ(info0.size(), 0u);

        // And that its access and modification time have been set.
        EXPECT_EQ(info0.accessed(), info0.modified());

        // Latch the file's ID.
        id0 = info0.id();
    }

    // Make sure the file's been purged from storage.
    auto info = ClientW()->fileInfo(id0);
    ASSERT_EQ(info.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_UNKNOWN_FILE);

    // Try and create a new file.
    auto file1 = ClientW()->fileCreate(mRootHandle, randomName());
    ASSERT_EQ(file1.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Make sure our original file's ID was recycled.
    EXPECT_EQ(file1->info().id(), id0);

    // Create a new file.
    auto file2 = ClientW()->fileCreate(mRootHandle, randomName());
    ASSERT_EQ(file2.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Make sure it has a newly generated ID.
    EXPECT_NE(file2->info().id(), id0);
}

TEST_F(FileServiceTests, create_write_succeeds)
{
    // Disable readahead.
    ClientW()->fileServiceOptions(DisableReadahead);

    // Create a new file.
    auto file = ClientW()->fileCreate(mRootHandle, randomName());

    // Make sure the file was created.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Events we expect to receive.
    FileEventVector expected;

    // Events we actually received.
    FileEventVector received;

    // So we can track what events were emitted for our file.
    auto observer = observe(
        [&received](auto& event)
        {
            received.emplace_back(event);
        },
        *file);

    // Generate some data for us to write to the file.
    auto data = randomBytes(64_KiB);

    // Try and write data to the file.
    ASSERT_EQ(execute(write, data.data(), *file, 128_KiB, 64_KiB), FILE_SUCCESS);

    expected.emplace_back(FileEvent{FileRange(128_KiB, 192_KiB), file->info().modified(), 192_KiB});

    // Make sure the file's size is correct.
    ASSERT_EQ(file->info().size(), 192_KiB);

    // The file should have one range starting from the beginning of the file.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(0, 192_KiB)));

    // All data before what we wrote should be zeroed.
    auto computed = execute(read, *file, 0, 128_KiB);

    // Make sure the read succeeded.
    ASSERT_EQ(computed.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    // Make sure the data we read is nothing but zeroes.
    ASSERT_EQ(computed->find_first_not_of('\0'), std::string::npos);

    // We should be able to read back what we wrote.
    computed = execute(read, *file, 128_KiB, 64_KiB);

    ASSERT_EQ(computed.errorOr(FILE_SUCCESS), FILE_SUCCESS);
    ASSERT_EQ(data, *computed);

    // Write more data to the file.
    ASSERT_EQ(execute(write, data.data(), *file, 320_KiB, 64_KiB), FILE_SUCCESS);

    expected.emplace_back(FileEvent{FileRange(320_KiB, 384_KiB), file->info().modified(), 384_KiB});

    // Make sure the file's size is correct.
    ASSERT_EQ(file->info().size(), 384_KiB);

    // We should still have a single range.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(0, 384_KiB)));

    // We should be able to read back what we wrote.
    computed = execute(read, *file, 320_KiB, 64_KiB);

    ASSERT_EQ(computed.errorOr(FILE_SUCCESS), FILE_SUCCESS);
    ASSERT_EQ(data, *computed);

    // Make sure we received the events we were expecting.
    ASSERT_EQ(expected, received);
}

TEST_F(FileServiceTests, fetch_succeeds)
{
    // Disable readahead.
    ClientW()->fileServiceOptions(DisableReadahead);

    // Open a file for reading.
    auto file = ClientW()->fileOpen(mFileHandle);

    // Make sure we could open the file.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Read some ranges from the file.
    ASSERT_EQ(execute(read, *file, 256_KiB, 256_KiB).errorOr(FILE_SUCCESS), FILE_SUCCESS);
    ASSERT_EQ(execute(read, *file, 768_KiB, 128_KiB).errorOr(FILE_SUCCESS), FILE_SUCCESS);

    // Reads shouldn't dirty a file.
    ASSERT_FALSE(file->info().dirty());

    // Make sure two ranges are active.
    ASSERT_THAT(file->ranges(),
                ElementsAre(FileRange(256_KiB, 512_KiB), FileRange(768_KiB, 896_KiB)));

    // Try and fetch the rest of the file's content.
    ASSERT_EQ(execute(fetch, *file), FILE_SUCCESS);

    // Fetching shouldn't dirty a file.
    ASSERT_FALSE(file->info().dirty());

    // We should now have a single range.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(0, 1_MiB)));
}

TEST_F(FileServiceTests, flush_cancel_on_client_logout_succeeds)
{
    // Create a client that we can safely logout.
    auto client = CreateClient("file_service_" + randomName());
    ASSERT_TRUE(client);

    // Log the client in.
    ASSERT_EQ(client->login(1), API_OK);

    // Upload content so we have a file we can safely mess with.
    auto handle = client->upload(randomBytes(512_KiB), randomName(), "/z");
    ASSERT_EQ(handle.errorOr(API_OK), API_OK);

    // Open the file so we can modify it.
    auto file = client->fileOpen(*handle);
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Truncate the file so it's considered modified.
    ASSERT_EQ(execute(truncate, *file, 256_KiB), FILE_SUCCESS);

    // Retrieve the file's remaining content.
    ASSERT_EQ(execute(fetch, *file), FILE_SUCCESS);

    // Make sure the upload doesn't complete before we logout the client.
    client->setUploadSpeed(4096);

    // Try and flush our local changes.
    auto waiter = flush(std::move(*file));

    // Log out the client.
    client.reset();

    // Wait for the flush to complete.
    ASSERT_NE(waiter.wait_for(mDefaultTimeout), timeout);

    // Make sure the flush was cancelled.
    ASSERT_EQ(waiter.get(), FILE_CANCELLED);
}

TEST_F(FileServiceTests, flush_cancel_on_file_destruction_succeeds)
{
    // Upload content so we have a file we can safely mess with.
    auto handle = ClientW()->upload(randomBytes(512_KiB), randomName(), "/z");
    ASSERT_EQ(handle.errorOr(API_OK), API_OK);

    // Open the file so we can modify it.
    auto file = ClientW()->fileOpen(*handle);
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Truncate the file so it's considered modified.
    ASSERT_EQ(execute(truncate, *file, 256_KiB), FILE_SUCCESS);

    // Retrieve the file's remaining content.
    ASSERT_EQ(execute(fetch, *file), FILE_SUCCESS);

    // Make sure the upload doesn't complete before we can the file.
    ClientW()->setUploadSpeed(4096);

    // Flush the file.
    auto waiter = [](File file)
    {
        // So we can wait for the request to complete.
        auto notifier = makeSharedPromise<FileResult>();

        // Try and flush our changes to the cloud.
        file.flush(
            [notifier](FileResult result)
            {
                return notifier->set_value(result);
            });

        // Return waiter to our caller.
        return notifier->get_future();
    }(std::move(*file));

    // Wait for the flush to complete.
    ASSERT_NE(waiter.wait_for(mDefaultTimeout), timeout);

    // Make sure the flush was cancelled.
    ASSERT_EQ(waiter.get(), FILE_CANCELLED);
}

TEST_F(FileServiceTests, flush_removed_file_fails)
{
    // Create a file we can safely remove.
    auto handle = ClientW()->upload(randomBytes(512_KiB), randomName(), mRootHandle);

    // Make sure we could create our file.
    ASSERT_EQ(handle.errorOr(API_OK), API_OK);

    // Open the file.
    auto file = ClientW()->fileOpen(*handle);

    // Make sure we could open the file.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Retrieve the file's data.
    ASSERT_EQ(execute(fetch, *file), FILE_SUCCESS);

    // Remove the file from the cloud.
    ASSERT_EQ(ClientW()->remove(*handle), API_OK);

    // Touch the file so that it has been modified.
    ASSERT_EQ(execute(touch, *file, now() + 1), FILE_SUCCESS);

    // You shouldn't be able to flush a file that has been removed.
    ASSERT_EQ(execute(flush, *file), FILE_REMOVED);
}

TEST_F(FileServiceTests, flush_succeeds)
{
    // Generate content for us to mutate.
    auto initial = randomBytes(512_KiB);

    // Upload content so we have a file we can safely mess with.
    auto oldHandle = ClientW()->upload(initial, randomName(), "/z");
    ASSERT_EQ(oldHandle.errorOr(API_OK), API_OK);

    // Open the file we uploaded.
    auto oldFile = ClientW()->fileOpen(*oldHandle);
    ASSERT_EQ(oldFile.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Generate some content for us to write to the file.
    auto content = randomBytes(128_KiB);

    // Write content to our file.
    ASSERT_EQ(execute(write, content.data(), *oldFile, 128_KiB, 128_KiB), FILE_SUCCESS);
    ASSERT_EQ(execute(write, content.data(), *oldFile, 384_KiB, 128_KiB), FILE_SUCCESS);

    // Keep track of the file's expected content.
    auto expected = initial;

    expected.replace(128_KiB, 128_KiB, content);
    expected.replace(384_KiB, 128_KiB, content);

    // Flush our modifications to the cloud.
    {
        // Latch the file's ID.
        auto id = oldFile->info().id();

        // Flush our modifications to the cloud.
        ASSERT_EQ(execute(flush, *oldFile), FILE_SUCCESS);

        // Make sure the file's ID hasn't changed.
        ASSERT_EQ(oldFile->info().id(), id);
    }

    // Latch the file's new handle.
    auto newHandle = oldFile->info().handle();

    // Make sure the file's handle has changed.
    ASSERT_NE(newHandle, oldHandle);

    // Make sure we can get information about the file using its new handle.
    {
        auto info = ClientW()->fileInfo(newHandle);
        ASSERT_EQ(info.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);
        ASSERT_EQ(info->id(), oldFile->info().id());
    }

    // Release the file (this will purge it from storage.)
    [](File) {}(std::move(*oldFile));

    // newHandle and oldHandle now represent distinct files.
    auto newFile = ClientW()->fileOpen(newHandle);
    ASSERT_EQ(newFile.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    oldFile = ClientW()->fileOpen(*oldHandle);
    ASSERT_EQ(oldFile.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Make sure newFile and oldFile are distinct.
    ASSERT_NE(newFile->info().id(), oldFile->info().id());

    // Make sure oldFile's content is unchanged.
    auto computed = execute(read, *oldFile, 0, 512_KiB);
    ASSERT_EQ(computed.errorOr(FILE_SUCCESS), FILE_SUCCESS);
    ASSERT_EQ(computed, initial);

    // Make sure newFile's content includes our changes.
    computed = execute(read, *newFile, 0, 512_KiB);
    ASSERT_EQ(computed.errorOr(FILE_SUCCESS), FILE_SUCCESS);
    ASSERT_EQ(computed, expected);
}

TEST_F(FileServiceTests, info_directory_fails)
{
    // Can't get info about a directory.
    EXPECT_EQ(ClientW()->fileInfo("/z").errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_UNKNOWN_FILE);
}

TEST_F(FileServiceTests, info_unknown_fails)
{
    // Can't get info about a file the service isn't managing.
    EXPECT_EQ(ClientW()->fileInfo(mFileHandle).errorOr(FILE_SERVICE_SUCCESS),
              FILE_SERVICE_UNKNOWN_FILE);
}

TEST_F(FileServiceTests, info_succeeds)
{
    // Convenience.
    using std::chrono::seconds;

    // Open our test file.
    auto file = ClientW()->fileOpen(mFileHandle);

    // Make sure we could open the file.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Latch the file's access time.
    auto accessed = file->info().accessed();

    // Move time forward.
    std::this_thread::sleep_for(seconds(1));

    // Get our hands on the file's information.
    auto info = ClientW()->fileInfo(mFileHandle);

    // Make sure we could get our hands on the file's information.
    ASSERT_EQ(info.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Make sure the file's access time hasn't changed.
    ASSERT_EQ(info->accessed(), accessed);
}

TEST_F(FileServiceTests, location_updated_when_moved_in_cloud)
{
    // Generate a name for our file.
    auto name = randomName();

    // Upload a file that we can move.
    auto handle = ClientW()->upload(randomBytes(512), name, mRootHandle);

    // Make sure we could upload the file.
    ASSERT_EQ(handle.errorOr(API_OK), API_OK);

    // Try and open the file.
    auto file = ClientW()->fileOpen(*handle);

    // Make sure we could open the file.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Make sure the file's location is correct.
    auto location = file->info().location();

    EXPECT_EQ(location.mName, name);
    ASSERT_EQ(location.mParentHandle, mRootHandle);

    // Expected new location.
    FileLocation newLocation{randomName(), mRootHandle};

    // Sanity.
    ASSERT_NE(location, newLocation);

    // Move the file in the cloud.
    ASSERT_EQ(ClientW()->move(newLocation.mName, *handle, mRootHandle), API_OK);

    // Our file's location should change.
    EXPECT_TRUE(waitFor(
        [&]()
        {
            return file->info().location() == newLocation;
        },
        mDefaultTimeout));

    location = file->info().location();

    EXPECT_EQ(location.mName, newLocation.mName);
    EXPECT_EQ(location.mParentHandle, newLocation.mParentHandle);
}

TEST_F(FileServiceTests, open_directory_fails)
{
    // Can't open a directory.
    EXPECT_EQ(ClientW()->fileOpen("/z").errorOr(FILE_SERVICE_SUCCESS),
              FILE_SERVICE_FILE_IS_A_DIRECTORY);
}

TEST_F(FileServiceTests, open_file_succeeds)
{
    // We should be able to open a file.
    auto file = ClientW()->fileOpen(mFileHandle);
    EXPECT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // We should be able to get information about that file.
    auto fileInfo = ClientW()->fileInfo(mFileHandle);
    EXPECT_EQ(fileInfo.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Files are initially clean.
    ASSERT_FALSE(fileInfo->dirty());

    // Get our hands on the node's information.
    auto nodeInfo = ClientW()->get(mFileHandle);
    ASSERT_EQ(nodeInfo.errorOr(API_OK), API_OK);

    // Make sure the file's information matches the node's.
    EXPECT_EQ(fileInfo->id(), FileID::from(mFileHandle));
    EXPECT_EQ(fileInfo->modified(), nodeInfo->mModified);
    EXPECT_EQ(fileInfo->size(), static_cast<std::uint64_t>(nodeInfo->mSize));
}

TEST_F(FileServiceTests, open_unknown_fails)
{
    // Can't open a file that doesn't exist.
    EXPECT_EQ(ClientW()->fileOpen("/bogus").errorOr(FILE_SERVICE_SUCCESS),
              FILE_SERVICE_FILE_DOESNT_EXIST);
}

TEST_F(FileServiceTests, read_cancel_on_client_logout_succeeds)
{
    // Create a client that we can safely logout.
    auto client = CreateClient("file_service_" + randomName());
    ASSERT_TRUE(client);

    // Log the client in.
    ASSERT_EQ(client->login(1), API_OK);

    // Disable readahead.
    client->fileServiceOptions(DisableReadahead);

    // Open a file for reading.
    auto file = client->fileOpen(mFileHandle);
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Make sure the read doesn't complete before we logout the client.
    client->setDownloadSpeed(4096);

    // Kick off a read.
    auto waiter = read(std::move(*file), 512_KiB, 256_KiB);

    // Log out the client.
    client.reset();

    // Wait for the read to complete.
    ASSERT_NE(waiter.wait_for(mDefaultTimeout), timeout);

    // Make sure the read was cancelled.
    EXPECT_EQ(waiter.get().errorOr(FILE_SUCCESS), FILE_CANCELLED);
}

TEST_F(FileServiceTests, read_cancel_on_file_destruction_succeeds)
{
    // Disable readahead.
    ClientW()->fileServiceOptions(DisableReadahead);

    // Open a file for reading.
    auto file = ClientW()->fileOpen(mFileHandle);
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // So our callback can tell us when the read's been cancelled.
    auto notifier = makeSharedPromise<FileResult>();

    // So we can wait until the read's been cancelled.
    auto waiter = notifier->get_future();

    // Make sure the read doesn't complete before the file's destroyed.
    ClientW()->setDownloadSpeed(4096);

    // Called when our read's been cancelled.
    auto callback = [=](FileResultOr<FileReadResult> result)
    {
        // Let the waiter know whether the read's been cancelled.
        notifier->set_value(result.errorOr(FILE_SUCCESS));
    }; // callback

    // Begin the read, taking care to drop our file reference.
    [&callback](File file)
    {
        file.read(std::move(callback), 768_KiB, 256_KiB);
    }(std::move(*file));

    // Wait for the read to complete.
    ASSERT_NE(waiter.wait_for(mDefaultTimeout), timeout);

    // Make sure the read was cancelled.
    EXPECT_EQ(waiter.get(), FILE_CANCELLED);
}

TEST_F(FileServiceTests, read_extension_succeeds)
{
    // No minimum read size, extend if another range is <= 32K distant.
    ClientW()->fileServiceOptions(FileServiceOptions{DefaultOptions.mMaximumRangeRetries,
                                                     32_KiB,
                                                     0u,
                                                     DefaultOptions.mRangeRetryBackoff});

    // Open a file for reading.
    auto file = ClientW()->fileOpen(mFileHandle);

    // Make sure the file was opened successfully.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Read two ranges, leaving a 128KiB hole between them.
    auto data = execute(read, *file, 0, 64_KiB);
    ASSERT_EQ(data.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    data = execute(read, *file, 192_KiB, 64_KiB);
    ASSERT_EQ(data.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    // Make sure we have the ranges we expect.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(0, 64_KiB), FileRange(192_KiB, 256_KiB)));

    // Read another range, right in the hole we created before.
    data = execute(read, *file, 96_KiB, 64_KiB);
    ASSERT_EQ(data.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    // Make sure our range was expanded to fill the hole.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(0, 256_KiB)));

    // Read another range, just beyond the extension threshold.
    data = execute(read, *file, 289_KiB, 64_KiB);
    ASSERT_EQ(data.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    // Make sure the range wasn't extended.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(0, 256_KiB), FileRange(289_KiB, 353_KiB)));

    // Perform a read to make sure we extend to the left.
    data = execute(read, *file, 385_KiB, 64_KiB);
    ASSERT_EQ(data.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(0, 256_KiB), FileRange(289_KiB, 449_KiB)));

    // Perform another read to create another hole.
    data = execute(read, *file, 640_KiB, 64_KiB);
    ASSERT_EQ(data.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    // Perform a read to make sure we extend to the right.
    data = execute(read, *file, 576_KiB, 32_KiB);
    ASSERT_EQ(data.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    ASSERT_THAT(file->ranges(),
                ElementsAre(FileRange(0, 256_KiB),
                            FileRange(289_KiB, 449_KiB),
                            FileRange(576_KiB, 704_KiB)));

    // Fill remaining holes via extension.
    data = execute(read, *file, 272_KiB, 8_KiB);
    ASSERT_EQ(data.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    data = execute(read, *file, 481_KiB, 63_KiB);
    ASSERT_EQ(data.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    // We should now have a single range.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(0, 704_KiB)));
}

TEST_F(FileServiceTests, read_removed_file_succeeds)
{
    // Create a file for us to play with.
    auto handle = ClientW()->upload(randomBytes(512_KiB), randomName(), mRootHandle);

    // Make sure we could create our file.
    ASSERT_EQ(handle.errorOr(API_OK), API_OK);

    // Open the file for reading.
    auto file = ClientW()->fileOpen(*handle);

    // Make sure we could open the file.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Disable readahead.
    ClientW()->fileServiceOptions(DisableReadahead);

    // Read some data from the file.
    auto data0 = execute(read, *file, 0, 256_KiB);

    // Make sure the read succeeded.
    ASSERT_EQ(data0.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    // Remove the file from the cloud.
    ASSERT_EQ(ClientW()->remove(*handle), API_OK);

    // Make sure we can read data we've already retrieved.
    auto data1 = execute(read, *file, 0, 256_KiB);

    ASSERT_EQ(data1.errorOr(FILE_SUCCESS), FILE_SUCCESS);
    ASSERT_FALSE(data0->compare(*data1));

    // Reading new data should fail.
    data1 = execute(read, *file, 256_KiB, 256_KiB);
    ASSERT_EQ(data1.errorOr(FILE_SUCCESS), FILE_REMOVED);
}

TEST_F(FileServiceTests, read_size_extension_succeeds)
{
    // Minimum read size is 64KiB, everything else are defaults.
    ClientW()->fileServiceOptions(FileServiceOptions{DefaultOptions.mMaximumRangeRetries,
                                                     DefaultOptions.mMinimumRangeDistance,
                                                     64_KiB,
                                                     DefaultOptions.mRangeRetryBackoff});

    // Open a file for reading.
    auto file = ClientW()->fileOpen(mFileHandle);
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Read 4K from the file.
    auto data = execute(read, *file, 0, 4_KiB);
    ASSERT_EQ(data.errorOr(FILE_SUCCESS), FILE_SUCCESS);
    ASSERT_EQ(static_cast<std::uint64_t>(data->size()), 4_KiB);

    // Make sure the read's size was extended.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(0, 64_KiB)));
}

TEST_F(FileServiceTests, read_succeeds)
{
    // Disable readahead.
    ClientW()->fileServiceOptions(DisableReadahead);

    // Open a file for reading.
    auto file = ClientW()->fileOpen(mFileHandle);
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Latch the file's access time.
    auto accessed = file->info().accessed();

    // We should be able to read 64KiB from the beginning of the file.
    auto result = execute(read, *file, 0, 64_KiB);

    // Make sure the file's access time have been bumped.
    EXPECT_GE(file->info().accessed(), accessed);

    // Make sure the read completed successfully.
    ASSERT_EQ(result.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    // And that it provided the data we expected.
    EXPECT_TRUE(compare(*result, mFileContent, 0, 64_KiB));

    // Make sure the range is considered to be in storage.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(0, 64_KiB)));

    // Latch the file's access time.
    accessed = file->info().accessed();

    // Read another 64KiB.
    result = execute(read, *file, 64_KiB, 64_KiB);

    // Make sure the file's access time have been bumped.
    EXPECT_GE(file->info().accessed(), accessed);

    // Make sure the read completed successfully.
    ASSERT_EQ(result.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    // And that it provided the data we expected.
    EXPECT_TRUE(compare(*result, mFileContent, 64_KiB, 64_KiB));

    // We should have one 128KiB range in storage.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(0, 128_KiB)));

    // Latch the file's access time.
    accessed = file->info().accessed();

    // Kick off two reads in parallel.
    auto waiter0 = read(*file, 128_KiB, 64_KiB);
    auto waiter1 = read(*file, 192_KiB, 64_KiB);

    // Wait for our reads to complete.
    ASSERT_NE(waiter0.wait_for(mDefaultTimeout), timeout);
    ASSERT_NE(waiter1.wait_for(mDefaultTimeout), timeout);

    // Make sure the file's access time have been bumped.
    EXPECT_GE(file->info().accessed(), accessed);

    // Make sure both reads succeeded.
    auto result0 = waiter0.get();
    auto result1 = waiter1.get();

    EXPECT_EQ(result0.errorOr(FILE_SUCCESS), FILE_SUCCESS);
    EXPECT_EQ(result1.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    ASSERT_FALSE(HasFailure());

    // Make sure both reads gave us what we expected.
    EXPECT_TRUE(compare(*result0, mFileContent, 128_KiB, 64_KiB));
    EXPECT_TRUE(compare(*result1, mFileContent, 192_KiB, 64_KiB));

    // We should have one 256KiB range in storage.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(0, 256_KiB)));

    // Latch the file's access time.
    accessed = file->info().accessed();

    // Make sure zero length reads are handled correctly.
    result = execute(read, *file, 0, 0);
    ASSERT_EQ(result.errorOr(FILE_SUCCESS), FILE_SUCCESS);
    EXPECT_TRUE(result->empty());

    // Zero length reads shouldn't change a file's access time.
    EXPECT_EQ(file->info().accessed(), accessed);

    // Make sure reads are clamped.
    result = execute(read, *file, 768_KiB, 512_KiB);
    ASSERT_EQ(result.errorOr(FILE_SUCCESS), FILE_SUCCESS);
    EXPECT_TRUE(compare(*result, mFileContent, 768_KiB, 256_KiB));

    // Make sure the file's access time have been bumped.
    EXPECT_GE(file->info().accessed(), accessed);

    // Reads should never dirty a file.
    ASSERT_FALSE(file->info().dirty());
}

TEST_F(FileServiceTests, read_write_sequence)
{
    // Try and open our test file.
    auto file = ClientW()->fileOpen(mFileHandle);

    // Make sure the file was opened.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Generate some data for us to write to the file.
    auto data = randomBytes(512_KiB);

    // The events we expect to be emitted for our file.
    FileEventVector expected;

    // What events were emitted for our file?
    FileEventVector received;

    // So we can store the events emitted for our file.
    auto observer = observe(
        [&received](auto& event)
        {
            received.emplace_back(event);
        },
        *file);

    // Initiate a read of the file's data.
    file->read([](auto) {}, 0, file->info().size());

    // Write our data to the file.
    ASSERT_EQ(execute(write, data.data(), *file, 256_KiB, 512_KiB), FILE_SUCCESS);

    expected.emplace_back(
        FileEvent{FileRange(256_KiB, 768_KiB), file->info().modified(), file->info().size()});

    // Curiosity.
    for (const auto& range: file->ranges())
        FSDebugF("Range: %s", toString(range).c_str());

    // Make sure we received the events we expected.
    ASSERT_EQ(expected, received);
}

TEST_F(FileServiceTests, reclaim_all_succeeds)
{
    // Handles of our test files.
    std::vector<NodeHandle> handles;

    // Create some test files for us to play with.
    for (auto i = 0; i < 4; ++i)
    {
        // Generate some data for our file.
        auto data = randomBytes(1_MiB);

        // Generate a name for our file.
        auto name = randomName();

        // Try and upload our test file.
        auto handle = ClientW()->upload(data, name, mRootHandle);

        // Make sure the upload succeeded.
        ASSERT_EQ(handle.errorOr(API_OK), API_OK);

        // Remember the file's node handle.
        handles.emplace_back(*handle);
    }

    // Tracks each file that we've opened.
    std::vector<File> files;

    // We'll be modifying these options later.
    auto options = DisableReadahead;

    // Disable readahead.
    //
    // This is necessary to ensure we read only as much as specified.
    ClientW()->fileServiceOptions(options);

    // Open, read and modify each file.
    for (auto handle: handles)
    {
        // Try and open the file.
        auto file = ClientW()->fileOpen(handle);

        // Make sure we could open the file.
        ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

        // Read some data from the file.
        auto data = execute(read, *file, 0, 512_KiB);

        // Make sure the read succeeded.
        ASSERT_EQ(data.errorOr(FILE_SUCCESS), FILE_SUCCESS);

        // Modify the file.
        ASSERT_EQ(execute(write, data->data(), *file, 0, 32_KiB), FILE_SUCCESS);

        // Make sure the service doesn't purge the file.
        files.emplace_back(std::move(*file));
    }

    // Determine how much storage the service is using.
    auto usedBefore = ClientW()->fileStorageUsed();
    ASSERT_EQ(usedBefore.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Make sure we're using only as much as we read.
    ASSERT_EQ(*usedBefore, 512_KiB * files.size());

    // Try and reclaim some storage.
    //
    // This should have no effect as we've specified no quota.
    auto reclaimed = execute(reclaimAll, ClientW());
    ASSERT_EQ(reclaimed.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);
    ASSERT_EQ(reclaimed.valueOr(0ul), 0ul);

    // Make sure no storage was reclaimed.
    auto usedAfter = ClientW()->fileStorageUsed();
    ASSERT_EQ(usedAfter.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);
    ASSERT_EQ(*usedAfter, *usedBefore);

    // Let the service know it should store no more than 544K.
    options.mReclaimSizeThreshold = 544_KiB;

    // Reclaim files that haven't been accessed for three hours.
    options.mReclaimAgeThreshold = std::chrono::hours(3);

    ClientW()->fileServiceOptions(options);

    // Try and reclaim storage.
    //
    // This should also have no effect as we accessed the files recently.
    reclaimed = execute(reclaimAll, ClientW());
    ASSERT_EQ(reclaimed.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);
    ASSERT_EQ(reclaimed.valueOr(0ul), 0ul);

    // Make sure no storage was reclaimed.
    usedAfter = ClientW()->fileStorageUsed();

    ASSERT_EQ(usedAfter.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);
    ASSERT_EQ(*usedBefore, *usedAfter);

    // Let the service know it can reclaim files regardless of access time.
    options.mReclaimAgeThreshold = std::chrono::hours(0);

    // Reclaim a single file at a time.
    options.mReclaimBatchSize = 1;

    ClientW()->fileServiceOptions(options);

    // Try and reclaim storage.
    reclaimed = execute(reclaimAll, ClientW());
    ASSERT_EQ(reclaimed.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Make sure storage was reclaimed.
    usedAfter = ClientW()->fileStorageUsed();

    ASSERT_EQ(usedAfter.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);
    ASSERT_LT(*usedAfter, *usedBefore);
    ASSERT_EQ(*reclaimed, *usedBefore - *usedAfter);

    // For later comparison.
    usedBefore = usedAfter;

    // Try and reclaim storage again.
    //
    // This should have no effect as we're already below the quota.
    reclaimed = execute(reclaimAll, ClientW());
    ASSERT_EQ(reclaimed.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);
    ASSERT_EQ(reclaimed.valueOr(0ul), 0ul);

    // Make sure no more storage was reclaimed.
    usedAfter = ClientW()->fileStorageUsed();

    ASSERT_EQ(usedAfter.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);
    ASSERT_EQ(*usedAfter, *usedBefore);
}

TEST_F(FileServiceTests, reclaim_periodic_succeeds)
{
    // Convenience.
    using std::chrono::hours;
    using std::chrono::minutes;
    using std::chrono::seconds;

    // Keeps track of our test files.
    std::vector<File> files;

    // Disable readahead and reclamation.
    auto options = DisableReadahead;

    options.mReclaimSizeThreshold = 0;

    ClientW()->fileServiceOptions(options);

    // Create a few files for us to test with.
    for (auto i = 0; i < 4; ++i)
    {
        // Generate some data for our test file.
        auto data = randomBytes(1_MiB);

        // Generate a name for our test file.
        auto name = randomName();

        // Try and upload our test file to the cloud.
        auto handle = ClientW()->upload(data, name, mRootHandle);

        // Make sure the upload succeeded.
        ASSERT_EQ(handle.errorOr(API_OK), API_OK);

        // Try and open our test file.
        auto file = ClientW()->fileOpen(*handle);

        // Make sure we could open our file.
        ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

        // Read some data from the file.
        ASSERT_EQ(execute(read, *file, 0, 512_KiB).errorOr(FILE_SUCCESS), FILE_SUCCESS);

        // Make sure the service doesn't prematurely purge our file.
        files.emplace_back(std::move(*file));
    }

    // Make sure our storage footprint is as we expect.
    auto usedBefore = ClientW()->fileStorageUsed();
    ASSERT_EQ(usedBefore.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);
    ASSERT_EQ(*usedBefore, 512_KiB * files.size());

    // Enable storage reclamation.
    options.mReclaimAgeThreshold = hours(0);
    options.mReclaimPeriod = seconds(15);
    options.mReclaimSizeThreshold = 512_KiB;

    ClientW()->fileServiceOptions(options);

    // Wait for storage to be reclaimed.
    EXPECT_TRUE(waitFor(
        [&]()
        {
            return ClientW()->fileStorageUsed().valueOr(0) == 512_KiB;
        },
        minutes(5)));

    // So we get useful logs.
    auto usedAfter = ClientW()->fileStorageUsed();
    ASSERT_EQ(usedAfter.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);
    EXPECT_LT(*usedAfter, *usedBefore);
    EXPECT_EQ(*usedAfter, 512_KiB);
}

TEST_F(FileServiceTests, reclaim_single_succeeds)
{
    // Generate data for us to write to the cloud.
    auto expected = randomBytes(512_KiB);

    // Create a file that we can modify.
    auto handle = ClientW()->upload(expected, randomName(), mRootHandle);

    // Make sure we could create our test file.
    ASSERT_EQ(handle.errorOr(API_OK), API_OK);

    // Open the file for IO.
    auto file = ClientW()->fileOpen(*handle);

    // Make sure we could open our file.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Read some data from the file.
    auto computed = execute(read, *file, 0, 64_KiB);
    ASSERT_EQ(computed.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    computed = execute(read, *file, 128_KiB, 64_KiB);
    ASSERT_EQ(computed.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    // Write some data to the file.
    ASSERT_EQ(execute(write, expected.data(), *file, 256_KiB, 64_KiB), FILE_SUCCESS);

    // Update our expected buffer.
    expected.replace(256_KiB, 64_KiB, expected, 0, 64_KiB);

    // Find out how much space the service is currently using.
    auto usedBefore = ClientW()->fileStorageUsed();
    ASSERT_EQ(usedBefore.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Try and reclaim the file's storage.
    {
        // For later comparison.
        auto allocatedBefore = file->info().allocatedSize();

        // Try and reclaim storage.
        auto reclaimed = execute(reclaim, *file);

        // Make sure reclamation succeeded.
        ASSERT_EQ(reclaimed.errorOr(FILE_SUCCESS), FILE_SUCCESS);

        // Convenience.
        auto allocatedAfter = file->info().allocatedSize();

        // Make sure we actually reclaimed space.
        ASSERT_LT(allocatedAfter, allocatedBefore);
        ASSERT_EQ(*reclaimed, allocatedBefore - allocatedAfter);

        // Make sure our file's attributes were updated correctly.
        ASSERT_EQ(file->info().reportedSize(), 0u);
        ASSERT_EQ(file->info().size(), expected.size());
    }

    // Make sure a new copy of our file has been uploaded to the cloud.
    auto info = ClientW()->get(file->info().handle());
    ASSERT_EQ(info.errorOr(API_OK), API_OK);
    ASSERT_NE(*handle, info->mHandle);

    // Make sure we actually reclaimed space.
    auto usedAfter = ClientW()->fileStorageUsed();
    ASSERT_EQ(usedAfter.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);
    ASSERT_LT(*usedAfter, *usedBefore);

    // Make sure we can read all of the file's data.
    computed = execute(read, *file, 0, 512_KiB);

    // Make sure the read completed.
    ASSERT_EQ(computed.errorOr(FILE_SUCCESS), FILE_SUCCESS);

    // And that the data we read was correct.
    ASSERT_TRUE(*computed == expected);
}

TEST_F(FileServiceTests, ref_succeeds)
{
    // Convenience.
    auto SUCCESS = FILE_SERVICE_SUCCESS;
    auto UNKNOWN = FILE_SERVICE_UNKNOWN_FILE;

    // Open a file for reading without retaining a reference.
    EXPECT_EQ(ClientW()->fileOpen(mFileHandle).errorOr(SUCCESS), SUCCESS);

    // The service should've removed the file as it has no references.
    EXPECT_EQ(ClientW()->fileInfo(mFileHandle).errorOr(UNKNOWN), UNKNOWN);

    // Open a file and establish a reference.
    {
        // Open the file.
        auto file = ClientW()->fileOpen(mFileHandle);
        ASSERT_EQ(file.errorOr(SUCCESS), SUCCESS);

        // Let the service know it should keep the file.
        file->ref();
    }

    // Make sure the service hasn't removed the file.
    EXPECT_EQ(ClientW()->fileInfo(mFileHandle).errorOr(SUCCESS), SUCCESS);

    // Get our hands on the file again.
    auto file = ClientW()->fileOpen(mFileHandle);
    ASSERT_EQ(file.errorOr(SUCCESS), SUCCESS);

    // Let the service know it can remove the file.
    file->unref();
}

TEST_F(FileServiceTests, remove_local_succeeds)
{
    // Records the ID of the file created directly below.
    FileID id;

    // Records how much storage space our test file occupied.
    FileServiceResultOr<std::uint64_t> usedBefore;

    // Create and then remove a local file.
    {
        // Generate a name for our file.
        auto name = randomName();

        // Create a local file.
        auto file0 = ClientW()->fileCreate(mRootHandle, name);

        // Make sure we could create the file.
        ASSERT_EQ(file0.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

        // Latch the file's ID.
        id = file0->info().id();

        // Generate some data to write to our file.
        auto data = randomBytes(512_KiB);

        // Write some data to the file.
        ASSERT_EQ(execute(write, data.data(), *file0, 0, data.size()), FILE_SUCCESS);

        // Figure out how much space our file's using.
        usedBefore = ClientW()->fileStorageUsed();

        // Make sure we could determine how much space our file was using.
        ASSERT_EQ(usedBefore.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

        // Remove the file.
        ASSERT_EQ(execute(remove, *file0), FILE_SUCCESS);

        // Make sure the file's been marked as removed.
        ASSERT_TRUE(file0->info().removed());

        // Make sure we can create another file at the same location.
        auto file1 = ClientW()->fileCreate(mRootHandle, name);

        // Make sure we could create our file.
        ASSERT_EQ(file1.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

        // Mark that file for removal, too.
        ASSERT_EQ(execute(remove, *file1), FILE_SUCCESS);
    }

    // Make sure the file's been removed.
    auto file = ClientW()->fileOpen(id);
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_FILE_DOESNT_EXIST);

    // Make sure storage space has been recovered.
    auto usedAfter = ClientW()->fileStorageUsed();

    ASSERT_EQ(usedAfter.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);
    ASSERT_LT(*usedAfter, *usedBefore);
}

TEST_F(FileServiceTests, remove_cloud_succeeds)
{
    // Generate a name for our file.
    auto name = randomName();

    // Upload a file to the cloud.
    auto handle = ClientW()->upload(randomBytes(512_KiB), name, mRootHandle);

    // Make sure our file was uploaded.
    ASSERT_EQ(handle.errorOr(API_OK), API_OK);

    // How much storage space our file used before it was removed.
    FileServiceResultOr<std::uint64_t> usedBefore;

    // Open the file, read some data and then remove it.
    {
        // Try and open our file.
        auto file0 = ClientW()->fileOpen(*handle);

        // Make sure we could open the file.
        ASSERT_EQ(file0.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

        // Read all data from the file.
        ASSERT_EQ(execute(fetch, *file0), FILE_SUCCESS);

        // Figure out how much storage space our file's using.
        usedBefore = ClientW()->fileStorageUsed();
        ASSERT_EQ(usedBefore.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

        // Remove the file.
        ASSERT_EQ(execute(remove, *file0), FILE_SUCCESS);

        // Make sure the file's been removed.
        ASSERT_TRUE(waitFor(
            [&]()
            {
                return ClientW()->get(*handle).errorOr(API_OK) == API_ENOENT &&
                       file0->info().removed();
            },
            mDefaultTimeout));

        EXPECT_EQ(ClientW()->get(*handle).errorOr(API_OK), API_ENOENT);
        EXPECT_TRUE(file0->info().removed());

        // Make sure we can create a new file at the same location.
        auto file1 = ClientW()->fileCreate(mRootHandle, name);
        ASSERT_EQ(file1.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

        // Mark that file for removal, too.
        ASSERT_EQ(execute(remove, *file1), FILE_SUCCESS);
    }

    // Make sure the file's been removed.
    auto file = ClientW()->fileOpen(*handle);
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_FILE_DOESNT_EXIST);

    // Make sure storage space has been recovered.
    auto usedAfter = ClientW()->fileStorageUsed();

    ASSERT_EQ(usedAfter.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);
    ASSERT_LT(*usedAfter, *usedBefore);
}

TEST_F(FileServiceTests, removed_when_overwritten_in_cloud)
{
    // Generate a name for our file.
    auto name = randomName();

    // Upload a file that we can replace.
    auto handle0 = ClientW()->upload(randomBytes(512), name, mRootHandle);

    // Make sure our file was uploaded.
    ASSERT_EQ(handle0.errorOr(API_OK), API_OK);

    // Try and open our file.
    auto file = ClientW()->fileOpen(*handle0);

    // Make sure the file was opened.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // The file shouldn't be marked as removed.
    ASSERT_FALSE(file->info().removed());

    // Upload a new file that we can rename.
    auto handle1 = ClientW()->upload(randomBytes(512), randomName(), mRootHandle);

    // Make sure the file was uploaded.
    ASSERT_EQ(handle1.errorOr(API_OK), API_OK);

    // Overwrite our original file.
    ASSERT_EQ(ClientW()->move(name, *handle1, mRootHandle), API_OK);

    // Make sure our original file has been marked as removed.
    EXPECT_TRUE(waitFor(
        [&]()
        {
            return file->info().removed();
        },
        mDefaultTimeout));

    EXPECT_TRUE(file->info().removed());
}

TEST_F(FileServiceTests, removed_when_removed_in_cloud)
{
    // Upload a file that we can remove.
    auto handle = ClientW()->upload(randomBytes(512), randomName(), mRootHandle);

    // Make sure the file was uploaded okay.
    ASSERT_EQ(handle.errorOr(API_OK), API_OK);

    // Try and open the file.
    auto file = ClientW()->fileOpen(*handle);

    // Make sure we could open the file.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // The file shouldn't be marked as removed.
    ASSERT_FALSE(file->info().removed());

    // Remove the filein the cloud.
    ASSERT_EQ(ClientW()->remove(*handle), API_OK);

    // Make sure the file was marked as removed.
    EXPECT_TRUE(waitFor(
        [&]()
        {
            return file->info().removed();
        },
        mDefaultTimeout));

    ASSERT_TRUE(file->info().removed());
}

TEST_F(FileServiceTests, removed_when_replaced_in_the_cloud)
{
    // Generate a name for our file.
    auto name = randomName();

    // Try and create a local file.
    auto file = ClientW()->fileCreate(mRootHandle, name);

    // Make sure we could create the file.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Make sure the file hasn't been marked as removed.
    ASSERT_FALSE(file->info().removed());

    // Add a new file with the same name in the cloud.
    auto handle = ClientW()->upload(randomBytes(512), name, mRootHandle);

    // Make sure the file was added.
    ASSERT_EQ(handle.errorOr(API_OK), API_OK);

    // Our local file should be marked as removed.
    EXPECT_TRUE(waitFor(
        [&]()
        {
            return file->info().removed();
        },
        mDefaultTimeout));

    ASSERT_TRUE(file->info().removed());
}

TEST_F(FileServiceTests, touch_succeeds)
{
    // Open a file for modification.
    auto file = ClientW()->fileOpen(mFileHandle);

    // Make sure the file was opened okay.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Events we expect to receive.
    FileEventVector expected;

    // Events we've received.
    FileEventVector received;

    // So we can keep track of our file's events.
    auto observer = observe(
        [&received](auto& event)
        {
            received.emplace_back(event);
        },
        *file);

    // Get our hands on the file's attributes.
    auto info = file->info();

    // Files should be clean initially.
    ASSERT_FALSE(info.dirty());

    // Latch the file's current access and modification time.
    auto accessed = info.accessed();
    auto modified = info.modified();

    // Try and update the file's modification time.
    ASSERT_EQ(execute(touch, *file, modified - 1), FILE_SUCCESS);

    expected.emplace_back(FileEvent{std::nullopt, modified - 1, file->info().size()});

    // Make sure the file's now considered dirty.
    EXPECT_TRUE(info.dirty());

    // Make sure the file's access time has been updated.
    EXPECT_GE(info.accessed(), accessed);
    EXPECT_GE(info.accessed(), info.modified());

    // Make sure the file's modification time was updated.
    EXPECT_EQ(info.modified(), modified - 1);

    // Make sure we received an event.
    ASSERT_EQ(expected, received);
}

TEST_F(FileServiceTests, truncate_with_ranges_succeeds)
{
    // Disable readahead.
    ClientW()->fileServiceOptions(DisableReadahead);

    // Open the file for truncation.
    auto file = ClientW()->fileOpen(mFileHandle);

    // Make sure the file was opened.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Reads a range from the file.
    auto read = [&](std::uint64_t offset, std::uint64_t length)
    {
        return execute(testing::read, *file, offset, length);
    }; // read

    // Download a range from the file.
    auto fetch = [&](std::uint64_t offset, std::uint64_t length)
    {
        return read(offset, length).errorOr(FILE_SUCCESS);
    }; // fetch

    // Truncate the file to a particular size.
    auto truncate = [&](std::uint64_t newSize)
    {
        // The events we expect to receive.
        FileEventVector expected;

        // The events we've received.
        FileEventVector received;

        // So we can receive events.
        auto observer = observe(
            [&received](auto& event)
            {
                received.emplace_back(event);
            },
            *file);

        // Get our hands on the file's attributes.
        auto info = file->info();

        // Latch the file's current size.
        auto size = info.size();

        // Determine whether the file should become dirty.
        auto dirty = newSize != size;

        // Latch the file's current access time.
        auto accessed = info.accessed();

        // Latch the file's current modification time.
        auto modified = info.modified();

        // Initiate the truncate request.
        auto result = execute(testing::truncate, *file, newSize);

        // Truncate failed.
        if (result != FILE_SUCCESS)
            return result;

        // We should only receive events if the file's size changed.
        if (dirty)
        {
            // Assume the file's size is increasing.
            std::optional<FileRange> range;

            // File's size is actually decreasing.
            if (newSize < size)
                range.emplace(newSize, size);

            expected.emplace_back(FileEvent{range, info.modified(), newSize});
        }

        // Make sure the file's attributes have been updated.
        EXPECT_EQ(info.dirty(), dirty);
        EXPECT_GE(info.accessed(), accessed);
        EXPECT_GE(info.modified(), modified);
        EXPECT_EQ(info.size(), newSize);

        // Make sure we received our expected events.
        EXPECT_EQ(expected, received);

        // One of the above expectations wasn't met.
        if (HasFailure())
            return FILE_FAILED;

        // Let the caller know the truncation was successful.
        return result;
    }; // truncate

    // Read a few ranges from the file.
    EXPECT_EQ(fetch(32_KiB, 32_KiB), FILE_SUCCESS);
    EXPECT_EQ(fetch(96_KiB, 32_KiB), FILE_SUCCESS);
    EXPECT_EQ(fetch(160_KiB, 32_KiB), FILE_SUCCESS);

    // Make sure we have the ranges we requested.
    ASSERT_THAT(file->ranges(),
                ElementsAre(FileRange(32_KiB, 64_KiB),
                            FileRange(96_KiB, 128_KiB),
                            FileRange(160_KiB, 192_KiB)));

    // Truncate the file to 256KiB.
    ASSERT_EQ(truncate(256_KiB), FILE_SUCCESS);

    // Existing ranges should be unchanged.
    ASSERT_THAT(file->ranges(),
                ElementsAre(FileRange(32_KiB, 64_KiB),
                            FileRange(96_KiB, 128_KiB),
                            FileRange(160_KiB, 192_KiB)));

    // Truncate the file to 160KiB.
    ASSERT_EQ(truncate(160_KiB), FILE_SUCCESS);

    // The range [160, 192] should have been removed.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(32_KiB, 64_KiB), FileRange(96_KiB, 128_KiB)));

    // Truncate the file to 112KiB.
    ASSERT_EQ(truncate(112_KiB), FILE_SUCCESS);

    // The range [96, 128] should become [96, 112].
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(32_KiB, 64_KiB), FileRange(96_KiB, 112_KiB)));

    // Extend the file to 256KiB.
    ASSERT_EQ(truncate(256_KiB), FILE_SUCCESS);

    // The range [96, 112] should become [96, 256].
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(32_KiB, 64_KiB), FileRange(96_KiB, 256_KiB)));
}

TEST_F(FileServiceTests, truncate_without_ranges_succeeds)
{
    // Open the file for truncation.
    auto file = ClientW()->fileOpen(mFileHandle);

    // Make sure the file was opened.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // The events we expect to receive.
    FileEventVector expected;

    // The events we've actually received.
    FileEventVector received;

    // So we can receive file events.
    auto observer = observe(
        [&received](auto& event)
        {
            received.emplace_back(event);
        },
        *file);

    // Get our hands on the file's attributes.
    auto info = file->info();

    // Files should be clean initially.
    ASSERT_FALSE(info.dirty());

    // Make sure the file has no active ranges.
    ASSERT_EQ(file->ranges().size(), 0u);

    // Latch the file's current modification time and size.
    auto modified = info.modified();
    auto size = info.size();

    // We should be able to reduce the file's size.
    ASSERT_EQ(execute(truncate, *file, size / 2), FILE_SUCCESS);

    expected.emplace_back(FileEvent{FileRange(size / 2, size), info.modified(), size / 2});

    // Mak sure the file's become dirty.
    EXPECT_TRUE(info.dirty());

    // Make sure the file's modification time and size were updated.
    EXPECT_GE(info.modified(), modified);
    EXPECT_EQ(info.size(), size / 2);

    // The file should still have no active ranges.
    EXPECT_EQ(file->ranges().size(), 0u);

    // Latch the file's current modification time.
    modified = info.modified();

    // We should be able to grow the file's size.
    ASSERT_EQ(execute(truncate, *file, size), FILE_SUCCESS);

    expected.emplace_back(FileEvent{std::nullopt, info.modified(), size});

    // Make sure the file's attributes were updated.
    EXPECT_GE(info.modified(), modified);
    EXPECT_EQ(info.size(), size);

    // There should be a single active range.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(size / 2, size)));

    // Make sure we can still read the file's content.
    auto result = execute(read, *file, 0, size);

    // Make sure the read succeeded.
    ASSERT_EQ(result.errorOr(FILE_SUCCESS), FILE_SUCCESS);
    ASSERT_EQ(result->size(), static_cast<std::size_t>(size));

    // Convenience.
    auto length = size / 2;
    auto npos = std::string::npos;

    // Make sure we read what we expected.
    EXPECT_EQ(mFileContent.compare(0, length, *result, 0, length), 0);
    EXPECT_EQ(result->find_first_not_of('\0', length), npos);

    // Make sure we received the events we expected.
    ASSERT_EQ(expected, received);
}

TEST_F(FileServiceTests, write_succeeds)
{
    // Disable readahead.
    ClientW()->fileServiceOptions(DisableReadahead);

    // File content that's updated as we write.
    auto expected = mFileContent;

    // Open a file for writing.
    auto file = ClientW()->fileOpen(mFileHandle);

    // Make sure we could actually open the file.
    ASSERT_EQ(file.errorOr(FILE_SERVICE_SUCCESS), FILE_SERVICE_SUCCESS);

    // Read content from the file and make sure it matches our expectations.
    auto read = [&](std::uint64_t offset, std::uint64_t length)
    {
        // Try and read content from our file.
        auto result = execute(testing::read, *file, offset, length);

        // Read failed.
        if (!result)
            return result.error();

        // Content isn't what we expected.
        if (!compare(*result, expected, offset, length))
            return FILE_FAILED;

        // Content satisfies our expectations.
        return FILE_SUCCESS;
    }; // read

    // Write content to our file.
    auto write = [&](const void* content, std::uint64_t offset, std::uint64_t length)
    {
        // Events we actually received.
        FileEventVector received;

        // Events we want to receive.
        FileEventVector wanted;

        // So we can receive events.
        auto observer = observe(
            [&received](auto& event)
            {
                received.emplace_back(event);
            },
            *file);

        // Get our hands on the file's information.
        auto info = file->info();

        // Latch the file's current access time.
        auto accessed = info.accessed();

        // Latch the file's current modification time and size.
        auto modified = info.modified();

        // Try and write content to our file.
        auto result = execute(testing::write, content, *file, offset, length);

        // Write failed.
        if (result != FILE_SUCCESS)
            return result;

        wanted.emplace_back(
            FileEvent{FileRange(offset, offset + length), info.modified(), info.size()});

        // Compute size of local file content.
        auto size = std::max<std::uint64_t>(expected.size(), offset + length);

        // Extend local file content as necessary.
        expected.resize(static_cast<std::size_t>(size));

        // Copy written content into our local file content buffer.
        std::memcpy(&expected[offset], content, length);

        // Make sure the file's become dirty.
        EXPECT_TRUE(info.dirty());

        // Make sure the file's access time has been updated.
        EXPECT_GE(info.accessed(), accessed);

        // Make sure the file's modification time hasn't gone backwards.
        EXPECT_GE(info.modified(), modified);

        // Make sure the file's size has been updated correctly.
        EXPECT_EQ(info.size(), size);

        // Make sure we received the events we wanted.
        EXPECT_EQ(received, wanted);

        // One or more of our expectations weren't satisfied.
        if (HasFailure())
            return FILE_FAILED;

        // Let our caller know the write was successful.
        return FILE_SUCCESS;
    }; // write

    // Convenience.
    using fuse::testing::randomBytes;

    // Generate some content for us to write to the file.
    auto computed = randomBytes(256u * 1024);

    // Write 64KiB to the file.
    ASSERT_EQ(write(computed.data(), 64_KiB, 64_KiB), FILE_SUCCESS);

    // Make sure we can read back what we wrote.
    ASSERT_EQ(read(64_KiB, 64_KiB), FILE_SUCCESS);

    // And that the file has a single range.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(64_KiB, 128_KiB)));

    // Read 128KiB from the file.
    //
    // This should cause us to download the first 64KiB of the file.
    ASSERT_EQ(read(0, 128_KiB), FILE_SUCCESS);

    // We should have a single 128KiB range.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(0, 128_KiB)));

    // Read 128KiB from the file.
    //
    // This should cause the download of two new ranges.
    ASSERT_EQ(read(192_KiB, 64_KiB), FILE_SUCCESS);
    ASSERT_EQ(read(320_KiB, 64_KiB), FILE_SUCCESS);

    // We should now have three ranges.
    ASSERT_THAT(file->ranges(),
                ElementsAre(FileRange(0, 128_KiB),
                            FileRange(192_KiB, 256_KiB),
                            FileRange(320_KiB, 384_KiB)));

    // Write 192KiB to the file.
    ASSERT_EQ(write(computed.data(), 160_KiB, 192_KiB), FILE_SUCCESS);

    // We should now have two ranges.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(0, 128_KiB), FileRange(160_KiB, 384_KiB)));

    // Read 384KiB from the file.
    //
    // This should cause us to download another range.
    ASSERT_EQ(read(0, 384_KiB), FILE_SUCCESS);

    // Which should coalesce with all the other ranges.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(0, 384_KiB)));

    // Make sure writes can extend the file's size.
    ASSERT_EQ(write(computed.data(), 2_MiB, 64_KiB), FILE_SUCCESS);

    // We should now have two ranges.
    ASSERT_THAT(file->ranges(), ElementsAre(FileRange(0, 384_KiB), FileRange(1024_KiB, 2112_KiB)));

    // Make sure we can read back what we wrote.
    ASSERT_EQ(read(2_MiB, 64_KiB), FILE_SUCCESS);
}

void FileServiceTests::SetUp()
{
    // Make sure our clients are still sane.
    Test::SetUp();

    // Make sure the service's options are in a known state.
    ClientW()->fileServiceOptions(DefaultOptions);

    // Make sure transfers proceed at full speed.
    ClientW()->setDownloadSpeed(0);
    ClientW()->setUploadSpeed(0);
}

void FileServiceTests::SetUpTestSuite()
{
    // Convenience.
    using ::testing::AnyOf;

    // Make sure our clients are set up.
    Test::SetUpTestSuite();

    // Make sure the test root is clean.
    ASSERT_THAT(ClientW()->remove("/z"), AnyOf(API_ENOENT, API_OK));

    // Recreate the test root.
    auto rootHandle = ClientW()->makeDirectory("z", "/");
    ASSERT_EQ(rootHandle.errorOr(API_OK), API_OK);

    // Generate content for our test file.
    mFileContent = randomBytes(1_MiB);

    // Generate a name for our test file.
    mFileName = randomName();

    // Upload our content to the cloud.
    auto fileHandle = ClientW()->upload(mFileContent, mFileName, *rootHandle);
    ASSERT_EQ(fileHandle.errorOr(API_OK), API_OK);

    // Latch the file's handle for later use.
    mFileHandle = *fileHandle;

    // Latch the root handle for later use.
    mRootHandle = *rootHandle;
}

auto append(const void* buffer, File file, std::uint64_t length) -> std::future<FileResult>
{
    // So we can signal when the request has completed.
    auto notifier = makeSharedPromise<FileResult>();

    // So our caller can wait until the request is completed.
    auto waiter = notifier->get_future();

    // Execute an append request.
    file.append(
        buffer,
        [=](FileResult result)
        {
            notifier->set_value(result);
        },
        length);

    // Return waiter to our caller.
    return waiter;
}

bool compare(const std::string& computed,
             const std::string& expected,
             std::uint64_t offset,
             std::uint64_t length)
{
    // Convenience.
    std::uint64_t size = expected.size();

    // Offset and/or length is out of bounds.
    if (offset + length > size)
        return false;

    // Content is smaller than expected.
    if (computed.size() != length)
        return false;

    // Make sure the content matches our file.
    return !expected.compare(offset, length, computed);
}

auto fetch(File file) -> std::future<FileResult>
{
    // So we can signal when the request has completed.
    auto notifier = makeSharedPromise<FileResult>();

    // So our caller can wait until the request has completed.
    auto waiter = notifier->get_future();

    // Execute the fetch request.
    file.fetch(
        [=](auto result)
        {
            notifier->set_value(result);
        });

    // Return waiter to our caller.
    return waiter;
}

auto flush(File file) -> std::future<FileResult>
{
    // So we can signal when the flush has completed.
    auto notifier = makeSharedPromise<FileResult>();

    // So our caller can wait until the request has completed.
    auto waiter = notifier->get_future();

    // Execute the flush request.
    file.flush(
        [file, notifier](auto result)
        {
            notifier->set_value(result);
        });

    // Return waiter to our caller.
    return waiter;
}

auto read(File file, std::uint64_t offset, std::uint64_t length)
    -> std::future<FileResultOr<std::string>>
{
    struct ReadContext;

    // Convenience.
    using ReadContextPtr = std::shared_ptr<ReadContext>;

    // Tracks state necessary for our read.
    struct ReadContext
    {
        ReadContext(File file, std::uint64_t length):
            mBuffer(),
            mFile(std::move(file)),
            mLength(length),
            mNotifier(),
            mOffset(0u)
        {}

        // Called when we've received content.
        void onRead(ReadContextPtr& context, FileResultOr<FileReadResult> result)
        {
            // Couldn't read content.
            if (!result)
                return mNotifier.set_value(unexpected(result.error()));

            // Convenience.
            auto& source = result->mSource;

            // No more content to read.
            if (!result->mLength)
                return mNotifier.set_value(std::move(mBuffer));

            // Extend buffer as needed.
            mBuffer.resize(mOffset + result->mLength);

            // Try and copy the content to our buffer.
            auto [count, _] = source.read(&mBuffer[mOffset], 0, result->mLength);

            // Couldn't copy the content to our buffer.
            if (count != result->mLength)
                return mNotifier.set_value(unexpected(FILE_FAILED));

            // Bump our offset and length.
            mOffset += count;
            mLength -= count;

            // Read remaining content, if any.
            mFile.read(
                std::bind(&ReadContext::onRead, this, std::move(context), std::placeholders::_1),
                result->mOffset + count,
                mLength);
        }

        // Where we'll store content.
        std::string mBuffer;

        // What file are we reading from?
        File mFile;

        // How much content are we reading?
        std::uint64_t mLength;

        // Who should we notify when the read is complete?
        std::promise<FileResultOr<std::string>> mNotifier;

        // Where in the file are we reading from?
        std::uint64_t mOffset;
    }; // ReadContext

    // Create a context to track our read state.
    auto context = std::make_shared<ReadContext>(file, length);

    // Get our hands on the context's future.
    auto waiter = context->mNotifier.get_future();

    // Kick off the read.
    file.read(
        std::bind(&ReadContext::onRead, context.get(), std::move(context), std::placeholders::_1),
        offset,
        length);

    // Return waiter to our caller.
    return waiter;
}

auto reclaim(File file) -> std::future<FileResultOr<std::uint64_t>>
{
    // So we can notify our waiter when the request completes.
    auto notifier = makeSharedPromise<FileResultOr<std::uint64_t>>();

    // So our caller can wait until the request completes.
    auto waiter = notifier->get_future();

    // Try and reclaim this file's storage.
    file.reclaim(
        [notifier](FileResultOr<std::uint64_t> result)
        {
            notifier->set_value(result);
        });

    // Return the waiter to our caller.
    return waiter;
}

auto reclaimAll(ClientPtr& client) -> std::future<FileServiceResultOr<std::uint64_t>>
{
    // Sanity.
    assert(client);

    // So we can notify our client when files have been reclaimed.
    auto notifier = makeSharedPromise<FileServiceResultOr<std::uint64_t>>();

    // So our caller can wait until files have been reclaimed.
    auto waiter = notifier->get_future();

    // Try and reclaim zero or more files.
    client->fileStorageReclaim(
        [notifier](auto result)
        {
            notifier->set_value(result);
        });

    // Return the waiter to our caller.
    return waiter;
}

auto remove(File file) -> std::future<FileResult>
{
    // So we can notify our caller when the file has been removed.
    auto notifier = makeSharedPromise<FileResult>();

    // So our caller can wait for the file to be removed.
    auto waiter = notifier->get_future();

    // Try and remove the file.
    file.remove(
        [notifier](auto result)
        {
            notifier->set_value(result);
        });

    // Return the waiter to our caller.
    return waiter;
}

auto touch(File file, std::int64_t modified) -> std::future<FileResult>
{
    // So we can notify our waiter when the request completes.
    auto notifier = makeSharedPromise<FileResult>();

    // So our caller can wait until the request completes.
    auto waiter = notifier->get_future();

    // Try and touch the file.
    file.touch(
        [notifier](FileResult result)
        {
            notifier->set_value(result);
        },
        modified);

    // Return the waiter to our caller.
    return waiter;
}

auto truncate(File file, std::uint64_t size) -> std::future<FileResult>
{
    // So we can notify our waiter when the request completes.
    auto notifier = makeSharedPromise<FileResult>();

    // So our caller can wait until the request completes.
    auto waiter = notifier->get_future();

    // Try and truncate the file.
    file.truncate(
        [notifier](FileResult result)
        {
            notifier->set_value(result);
        },
        size);

    // Return the waiter to our caller.
    return waiter;
}

auto write(const void* buffer, File file, std::uint64_t offset, std::uint64_t length)
    -> std::future<FileResult>
{
    struct WriteContext;

    // Convenience.
    using WriteContextPtr = std::shared_ptr<WriteContext>;

    // Tracks state necessary for our write.
    struct WriteContext
    {
        WriteContext(const void* buffer, File file, std::uint64_t length):
            mBuffer(static_cast<const std::uint8_t*>(buffer)),
            mFile(std::move(file)),
            mLength(length)
        {}

        // Called when we've written file content.
        void onWrite(WriteContextPtr& context, FileResultOr<FileWriteResult> result)
        {
            // Couldn't write content.
            if (!result)
                return mNotifier.set_value(result.error());

            // No more content to write.
            if (!result->mLength)
                return mNotifier.set_value(FILE_SUCCESS);

            // Bump buffer and length.
            mBuffer += result->mLength;
            mLength -= result->mLength;

            // Write remaning content, if any.
            mFile.write(
                mBuffer,
                std::bind(&WriteContext::onWrite, this, std::move(context), std::placeholders::_1),
                result->mLength + result->mOffset,
                mLength);
        }

        // The content we want to write.
        const std::uint8_t* mBuffer;

        // What file we should write content to.
        File mFile;

        // How much content we should write to our file.
        std::uint64_t mLength;

        // Who should we notify when the write is complete?
        std::promise<FileResult> mNotifier;
    }; // WriteContext

    // Create a context to track our write state.
    auto context = std::make_shared<WriteContext>(buffer, file, length);

    // Get our hands on the context's future.
    auto waiter = context->mNotifier.get_future();

    // Kick off the write.
    file.write(
        buffer,
        std::bind(&WriteContext::onWrite, context.get(), std::move(context), std::placeholders::_1),
        offset,
        length);

    // Return waiter to our caller.
    return waiter;
}

} // testing
} // file_service
} // mega
