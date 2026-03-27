/**
 * @file SqliteNodesPerf_test.cpp
 * @brief Performance tests for the SqliteAccountState `nodes` table query methods.
 *
 * Each TEST_F runs a query repeatedly and reports per-iteration latency via
 * GTEST_LOG_(INFO).  No wall-clock assertions are made so the suite never
 * fails on slow machines, but developers can spot regressions by comparing
 * reported numbers.
 *
 * Dataset (created in SetUp):
 *   - 3 root nodes  (ROOT / VAULT / RUBBISH)
 *   - NUM_TOP_FOLDERS folder nodes  (children of ROOT)
 *   - NUM_TOP_FOLDERS * NUM_SUB_PER_TOP sub-folders
 *   - NUM_FILES_PER_SUB file nodes per sub-folder
 *   Total ≈ 3 + 10 + 100 + 9 800 = 9 913 nodes
 */

#include <gtest/gtest.h>
#include <mega/db/sqlite.h>
#include <mega/megaapp.h>
#include <mega/megaclient.h>
#include <mega/nodemanager.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mega.h>
#include <optional>
#include <sstream>
#include <string>

#if defined(__linux__)
#include <malloc.h>
#endif

#include "utils.h"

#include <stdfs.h>

#ifdef USE_SQLITE

namespace fs = std::filesystem;

using namespace mega;

namespace
{

// ─── Dataset dimensions ─────────────────────────────────────────────────────
constexpr int NUM_TOP_FOLDERS = 10;
constexpr int NUM_SUB_PER_TOP = 10; // → 100 sub-folders total
constexpr int NUM_FILES_PER_SUB = 980; // → 98,0000 file nodes total

// ─── Iteration counts ───────────────────────────────────────────────────────
// Simple point-queries (index seek, returns ≤ 1 row)
constexpr int SIMPLE_ITERS = 1000;
// Scan-based or recursive queries
constexpr int COMPLEX_ITERS = 100;

// ─── Timing helper ──────────────────────────────────────────────────────────
// Returns total elapsed time in microseconds after running fn() `iters` times.
template<typename Fn>
long long measureUs(int iters, Fn&& fn)
{
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i)
        fn();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
}

// ─── Memory helpers ──────────────────────────────────────────────────────────

/// Resident Set Size in kilobytes, read from /proc/self/status.
/// Returns 0 if the file is unavailable (non-Linux platforms).
static size_t getRssKb()
{
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line))
    {
        if (line.compare(0, 6, "VmRSS:") == 0)
        {
            size_t val = 0;
            // Format: "VmRSS:   12345 kB"
            std::istringstream iss(line.substr(6));
            iss >> val;
            return val;
        }
    }
#endif
    return 0;
}

/// Heap bytes currently allocated (in-use), via mallinfo2 (glibc ≥ 2.33)
/// or mallinfo as fallback.  Returns 0 on non-glibc platforms.
static size_t getHeapBytes()
{
#if defined(__linux__) && defined(_GNU_SOURCE)
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    struct mallinfo2 mi = mallinfo2();
    return static_cast<size_t>(mi.uordblks);
#else
    struct mallinfo mi = mallinfo();
    return static_cast<size_t>(static_cast<unsigned int>(mi.uordblks));
#endif
#else
    return 0;
#endif
}

/// Simple snapshot of both RSS and heap at a point in time.
struct MemSnapshot
{
    size_t rssKb = 0; ///< Resident Set Size in kB
    size_t heapBytes = 0; ///< In-use heap bytes

    static MemSnapshot take()
    {
        return {getRssKb(), getHeapBytes()};
    }

    MemSnapshot operator-(const MemSnapshot& base) const
    {
        return {rssKb >= base.rssKb ? rssKb - base.rssKb : 0,
                heapBytes >= base.heapBytes ? heapBytes - base.heapBytes : 0};
    }
};

// ─── Test fixture ───────────────────────────────────────────────────────────
class SqliteNodesPerfTest: public ::testing::Test
{
protected:
    mega::MegaApp mApp;
    NodeManager::MissingParentNodes mMissingParentNodes;
    std::shared_ptr<MegaClient> mClient;
    fs::path mTestDir;

    uint64_t mNextHandle = 1;

    // Handles collected during dataset construction
    NodeHandle mRootHandle;
    std::vector<NodeHandle> mTopFolderHandles;
    std::vector<NodeHandle> mSubFolderHandles;
    std::vector<NodeHandle> mFileHandles;

    // A representative leaf file used in single-node tests
    NodeHandle mLeafFileHandle;
    std::string mLeafFileName;
    std::string mLeafFileFingerprint; // serialised FileFingerprint blob
    NodeHandle mLeafParentHandle; // direct parent (sub-folder)

    // Representative leaf files for mime-filter tests (i=0, j=0)
    NodeHandle mPhotoLeafHandle; // k=20 → k%10==0 → .jpg
    std::string mPhotoLeafFileName;
    NodeHandle mVideoLeafHandle; // k=25 → k%10==5 → .mp4
    std::string mVideoLeafFileName;

    // A folder that has a share flag set
    NodeHandle mSharedFolderHandle;

    void SetUp() override
    {
        mTestDir = fs::current_path() / "sqlite_nodes_perf_test";
        // Remove any leftover directory from a previous crashed run to avoid
        // SQLite "database is locked" errors caused by stale WAL files.
        fs::remove_all(mTestDir);
        fs::create_directories(mTestDir);

        auto* dbAccess = new SqliteDbAccess(LocalPath::fromAbsolutePath(path_u8string(mTestDir)));

        mClient = mt::makeClient(mApp, dbAccess);
        // sid must be set so that opensctable() can derive a DB filename.
        mClient->sid =
            "AWA5YAbtb4JO-y2zWxmKZpSe5-6XM7CTEkA-3Nv7J4byQUpOazdfSC1ZUFlS-kah76gPKUEkTF9g7MeE";
        mClient->opensctable();

        populateDB();

        // Build indexes after bulk-insert for realistic query benchmarking
        // (both the search-index set and the lexicographic-sort index set).
        if (auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get()))
            sa->createIndexes(/*enableSearch=*/true, /*enableLexi=*/true);
    }

    void TearDown() override
    {
        mClient.reset();
        fs::remove_all(mTestDir);
    }

    // ── Low-level node factory ───────────────────────────────────────────────
    std::shared_ptr<Node> addNode(nodetype_t nodeType,
                                  std::shared_ptr<Node> parent,
                                  const std::string& name,
                                  bool fav = false,
                                  int label = 0)
    {
        NodeHandle handle = NodeHandle().set6byte(mNextHandle++);
        Node& nodeRef = mt::makeNode(*mClient, nodeType, handle, parent.get());
        auto node = std::shared_ptr<Node>(&nodeRef);

        // Name attribute
        static const nameid nameId = AttrMap::string2nameid("n");
        node->attrs.map[nameId] = name;

        // File-specific fields
        if (nodeType == FILENODE)
        {
            node->size = static_cast<m_off_t>(mNextHandle * 512);
            node->ctime = static_cast<m_time_t>(1700000000LL + static_cast<int64_t>(mNextHandle));
            node->mtime = node->ctime;
            node->crc[0] = static_cast<int32_t>(mNextHandle);
            node->crc[1] = static_cast<int32_t>(mNextHandle >> 8u);
            node->crc[2] = static_cast<int32_t>(mNextHandle >> 16u);
            node->crc[3] = static_cast<int32_t>(mNextHandle >> 24u);
            node->isvalid = true;
            node->serializefingerprint(&node->attrs.map['c']);
            node->setfingerprint();
        }

        if (fav)
        {
            static const nameid favId = AttrMap::string2nameid("fav");
            node->attrs.map[favId] = "1";
        }

        if (label > 0)
        {
            static const nameid labelId = AttrMap::string2nameid("lbl");
            node->attrs.map[labelId] = std::to_string(label);
        }

        mClient->mNodeManager.addNode(node,
                                      /*notify=*/false,
                                      /*isFetching=*/true,
                                      mMissingParentNodes);
        mClient->mNodeManager.saveNodeInDb(node.get());
        return node;
    }

    // ── Dataset construction ─────────────────────────────────────────────────
    void populateDB()
    {
        // Root nodes (parenthandle == UNDEF ↔ nullptr parent)
        auto rootNode = addNode(ROOTNODE, nullptr, "ROOT");
        mRootHandle = rootNode->nodeHandle();
        addNode(VAULTNODE, nullptr, "VAULT");
        addNode(RUBBISHNODE, nullptr, "RUBBISH");

        bool leafCaptured = false;

        for (int i = 0; i < NUM_TOP_FOLDERS; ++i)
        {
            auto topFolder = addNode(FOLDERNODE, rootNode, "Folder_" + std::to_string(i));
            mTopFolderHandles.push_back(topFolder->nodeHandle());

            for (int j = 0; j < NUM_SUB_PER_TOP; ++j)
            {
                auto subFolder =
                    addNode(FOLDERNODE,
                            topFolder,
                            "SubFolder_" + std::to_string(i) + "_" + std::to_string(j));
                if (i == 0 && j == 0)
                    mSharedFolderHandle = subFolder->nodeHandle();
                mSubFolderHandles.push_back(subFolder->nodeHandle());

                for (int k = 0; k < NUM_FILES_PER_SUB; ++k)
                {
                    bool isFav = (i == 0 && j == 0 && k == 0);

                    // Cycle through label values 0-6 (0 = unlabelled, 1-6 = colour labels).
                    // Using k%7 ensures a realistic mix across all label groups.
                    int fileLabel = k % 7;

                    // Mix file types: k%10 < 5 → .jpg (PHOTO),
                    //                 k%10 == 5 → .mp4 (VIDEO),
                    //                 k%10 >= 6 → .txt (DOCUMENT)
                    const char* ext = (k % 10 < 5) ? ".jpg" : (k % 10 == 5) ? ".mp4" : ".txt";
                    std::string fname = "file_" + std::to_string(i) + "_" + std::to_string(j) +
                                        "_" + std::to_string(k) + ext;

                    auto file = addNode(FILENODE, subFolder, fname, isFav, fileLabel);
                    mFileHandles.push_back(file->nodeHandle());

                    // Capture the middle file of the first sub-folder as our
                    // representative leaf used in single-node benchmarks.
                    // k=49 → 49%10==9 ≥ 6 → .txt
                    if (!leafCaptured && i == 0 && j == 0 && k == NUM_FILES_PER_SUB / 2)
                    {
                        mLeafFileHandle = file->nodeHandle();
                        mLeafFileName = fname;
                        mLeafParentHandle = subFolder->nodeHandle();

                        // Persist the fingerprint blob for later use.
                        auto it = file->attrs.map.find('c');
                        if (it != file->attrs.map.end())
                            mLeafFileFingerprint = it->second;

                        leafCaptured = true;
                    }

                    // Capture photo leaf: i=0, j=0, k=20 → k%10==0 → .jpg
                    if (i == 0 && j == 0 && k == 20)
                    {
                        mPhotoLeafHandle = file->nodeHandle();
                        mPhotoLeafFileName = fname;
                    }

                    // Capture video leaf: i=0, j=0, k=25 → k%10==5 → .mp4
                    if (i == 0 && j == 0 && k == 25)
                    {
                        mVideoLeafHandle = file->nodeHandle();
                        mVideoLeafFileName = fname;
                    }
                }
            }
        }
    }

    // ── Convenience accessor ─────────────────────────────────────────────────
    DBTableNodes* nodesTable()
    {
        return dynamic_cast<DBTableNodes*>(mClient->sctable.get());
    }

    // ── Build a cursor anchored at the leaf file for searchNodesByPage ────────
    // Only the sort-key optional relevant to `order` is populated so that the
    // WHERE clause generated by searchNodesByPage matches what production uses.
    NodeSearchCursorOffset leafCursor(int order) const
    {
        NodeSearchCursorOffset c;
        c.mLastName = mLeafFileName;
        c.mLastType = FILENODE;
        c.mLastHandle = mLeafFileHandle.as8byte();

        switch (order)
        {
            case OrderByClause::SIZE_ASC:
            case OrderByClause::SIZE_DESC:
                // sizeVirtual == 0 for all perf-test nodes (NodeCounter is not
                // pre-computed in populateDB). The name / handle tie-breaker
                // exercises the same SQL branches as production data.
                c.mLastSize = 0;
                break;
            case OrderByClause::MTIME_ASC:
            case OrderByClause::MTIME_DESC:
                // mtime is stored as a plain column and is always accurate.
                c.mLastMtime = 1700000000LL + static_cast<int64_t>(mLeafFileHandle.as8byte());
                break;
            case OrderByClause::LABEL_ASC:
            case OrderByClause::LABEL_DESC:
                c.mLastLabel = 0; // leaf file carries no label
                break;
            case OrderByClause::FAV_ASC:
            case OrderByClause::FAV_DESC:
                c.mLastFav = 0; // leaf file is not a favourite
                break;
            default:
                break; // DEFAULT_ASC / DEFAULT_DESC: name + handle suffice
        }
        return c;
    }

    // ── Cursor anchored at the representative .jpg leaf (i=0, j=0, k=20) ─────
    NodeSearchCursorOffset photoLeafCursor(int order) const
    {
        NodeSearchCursorOffset c;
        c.mLastName = mPhotoLeafFileName;
        c.mLastType = FILENODE;
        c.mLastHandle = mPhotoLeafHandle.as8byte();

        switch (order)
        {
            case OrderByClause::SIZE_ASC:
            case OrderByClause::SIZE_DESC:
                c.mLastSize = 0;
                break;
            case OrderByClause::MTIME_ASC:
            case OrderByClause::MTIME_DESC:
                c.mLastMtime = 1700000000LL + static_cast<int64_t>(mPhotoLeafHandle.as8byte());
                break;
            case OrderByClause::LABEL_ASC:
            case OrderByClause::LABEL_DESC:
                c.mLastLabel = 20 % 7; // k=20 → label = 6
                break;
            case OrderByClause::FAV_ASC:
            case OrderByClause::FAV_DESC:
                c.mLastFav = 0;
                break;
            default:
                break;
        }
        return c;
    }

    // ── Cursor anchored at the representative .mp4 leaf (i=0, j=0, k=25) ─────
    NodeSearchCursorOffset videoLeafCursor(int order) const
    {
        NodeSearchCursorOffset c;
        c.mLastName = mVideoLeafFileName;
        c.mLastType = FILENODE;
        c.mLastHandle = mVideoLeafHandle.as8byte();

        switch (order)
        {
            case OrderByClause::SIZE_ASC:
            case OrderByClause::SIZE_DESC:
                c.mLastSize = 0;
                break;
            case OrderByClause::MTIME_ASC:
            case OrderByClause::MTIME_DESC:
                c.mLastMtime = 1700000000LL + static_cast<int64_t>(mVideoLeafHandle.as8byte());
                break;
            case OrderByClause::LABEL_ASC:
            case OrderByClause::LABEL_DESC:
                c.mLastLabel = 25 % 7; // k=25 → label = 4
                break;
            case OrderByClause::FAV_ASC:
            case OrderByClause::FAV_DESC:
                c.mLastFav = 0;
                break;
            default:
                break;
        }
        return c;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Individual benchmarks
// ═══════════════════════════════════════════════════════════════════════════

// ─── 1. getNumberOfNodes ────────────────────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetNumberOfNodes)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    uint64_t count = 0;
    const long long us = measureUs(SIMPLE_ITERS,
                                   [&]
                                   {
                                       count = table->getNumberOfNodes();
                                   });

    EXPECT_GT(count, 0u);
    GTEST_LOG_(INFO) << "getNumberOfNodes [" << count << " nodes]: " << SIMPLE_ITERS
                     << " iters, total " << us << " us, avg " << us / SIMPLE_ITERS << " us/iter";
}

// ─── 2. getNode (single handle lookup) ──────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetNode)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);
    ASSERT_FALSE(mLeafFileHandle.isUndef());

    NodeSerialized ns;
    // Warm-up: ensure the statement is prepared.
    ASSERT_TRUE(table->getNode(mLeafFileHandle, ns));

    const long long us = measureUs(SIMPLE_ITERS,
                                   [&]
                                   {
                                       NodeSerialized tmp;
                                       table->getNode(mLeafFileHandle, tmp);
                                   });

    GTEST_LOG_(INFO) << "getNode (by handle): " << SIMPLE_ITERS << " iters, total " << us
                     << " us, avg " << us / SIMPLE_ITERS << " us/iter";
}

// ─── 3. getNumberOfChildren ─────────────────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetNumberOfChildren)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);
    ASSERT_FALSE(mSubFolderHandles.empty());

    const NodeHandle parent = mSubFolderHandles.front();
    uint64_t childCount = 0;

    const long long us = measureUs(SIMPLE_ITERS,
                                   [&]
                                   {
                                       childCount = table->getNumberOfChildren(parent);
                                   });

    EXPECT_EQ(childCount, static_cast<uint64_t>(NUM_FILES_PER_SUB));
    GTEST_LOG_(INFO) << "getNumberOfChildren [" << childCount << " children]: " << SIMPLE_ITERS
                     << " iters, total " << us << " us, avg " << us / SIMPLE_ITERS << " us/iter";
}

// ─── 4. getNumberOfChildrenByType ───────────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetNumberOfChildrenByType)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);
    ASSERT_FALSE(mSubFolderHandles.empty());

    const NodeHandle parent = mSubFolderHandles.front();
    uint64_t fileCount = 0;

    const long long us = measureUs(SIMPLE_ITERS,
                                   [&]
                                   {
                                       fileCount =
                                           table->getNumberOfChildrenByType(parent, FILENODE);
                                   });

    EXPECT_EQ(fileCount, static_cast<uint64_t>(NUM_FILES_PER_SUB));
    GTEST_LOG_(INFO) << "getNumberOfChildrenByType(FILENODE) [" << fileCount
                     << " children]: " << SIMPLE_ITERS << " iters, total " << us << " us, avg "
                     << us / SIMPLE_ITERS << " us/iter";
}

// ─── 5. childNodeByNameType ─────────────────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfChildNodeByNameType)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);
    ASSERT_FALSE(mLeafFileName.empty());

    std::pair<NodeHandle, NodeSerialized> result;
    // Verify the node is actually found before benchmarking.
    ASSERT_TRUE(table->childNodeByNameType(mLeafParentHandle, mLeafFileName, FILENODE, result));
    EXPECT_EQ(result.first, mLeafFileHandle);

    const long long us =
        measureUs(SIMPLE_ITERS,
                  [&]
                  {
                      std::pair<NodeHandle, NodeSerialized> tmp;
                      table->childNodeByNameType(mLeafParentHandle, mLeafFileName, FILENODE, tmp);
                  });

    GTEST_LOG_(INFO) << "childNodeByNameType: " << SIMPLE_ITERS << " iters, total " << us
                     << " us, avg " << us / SIMPLE_ITERS << " us/iter";
}

// ─── 6. getNodeSizeTypeAndFlags ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetNodeSizeTypeAndFlags)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);
    ASSERT_FALSE(mLeafFileHandle.isUndef());

    m_off_t size = 0;
    nodetype_t ntype = TYPE_UNKNOWN;
    uint64_t flags = 0;
    ASSERT_TRUE(table->getNodeSizeTypeAndFlags(mLeafFileHandle, size, ntype, flags));

    const long long us = measureUs(SIMPLE_ITERS,
                                   [&]
                                   {
                                       m_off_t s = 0;
                                       nodetype_t t = TYPE_UNKNOWN;
                                       uint64_t f = 0;
                                       table->getNodeSizeTypeAndFlags(mLeafFileHandle, s, t, f);
                                   });

    GTEST_LOG_(INFO) << "getNodeSizeTypeAndFlags: " << SIMPLE_ITERS << " iters, total " << us
                     << " us, avg " << us / SIMPLE_ITERS << " us/iter";
}

// ─── 7. getNodeByFingerprint ────────────────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetNodeByFingerprint)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);
    ASSERT_FALSE(mLeafFileFingerprint.empty());

    NodeSerialized ns;
    NodeHandle handle;
    ASSERT_TRUE(table->getNodeByFingerprint(mLeafFileFingerprint, ns, handle));

    const long long us = measureUs(SIMPLE_ITERS,
                                   [&]
                                   {
                                       NodeSerialized tmp;
                                       NodeHandle h;
                                       table->getNodeByFingerprint(mLeafFileFingerprint, tmp, h);
                                   });

    GTEST_LOG_(INFO) << "getNodeByFingerprint: " << SIMPLE_ITERS << " iters, total " << us
                     << " us, avg " << us / SIMPLE_ITERS << " us/iter";
}

// ─── 8. getNodesByOrigFingerprint ───────────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetNodesByOrigFingerprint)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);
    ASSERT_FALSE(mLeafFileFingerprint.empty());

    const long long us =
        measureUs(SIMPLE_ITERS,
                  [&]
                  {
                      std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                      table->getNodesByOrigFingerprint(mLeafFileFingerprint, nodes);
                  });

    GTEST_LOG_(INFO) << "getNodesByOrigFingerprint: " << SIMPLE_ITERS << " iters, total " << us
                     << " us, avg " << us / SIMPLE_ITERS << " us/iter";
}

// ─── 9. getRootNodes ────────────────────────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetRootNodes)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    std::vector<std::pair<NodeHandle, NodeSerialized>> roots;
    ASSERT_TRUE(table->getRootNodes(roots));
    EXPECT_EQ(roots.size(), 3u); // ROOT + VAULT + RUBBISH

    const long long us = measureUs(SIMPLE_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> tmp;
                                       table->getRootNodes(tmp);
                                   });

    GTEST_LOG_(INFO) << "getRootNodes [" << roots.size() << " roots]: " << SIMPLE_ITERS
                     << " iters, total " << us << " us, avg " << us / SIMPLE_ITERS << " us/iter";
}

// ─── 10. getChildren (no filter, default order) ─────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetChildren_NoFilter)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);
    ASSERT_FALSE(mSubFolderHandles.empty());

    const NodeHandle parent = mSubFolderHandles.front();

    NodeSearchFilter filter;
    filter.byLocationHandle(parent.as8byte());

    const long long us =
        measureUs(COMPLEX_ITERS,
                  [&]
                  {
                      std::vector<std::pair<NodeHandle, NodeSerialized>> children;
                      CancelToken ct;
                      NodeSearchPage page{0, 0}; // no paging limit
                      table->getChildren(filter, OrderByClause::DEFAULT_ASC, children, ct, page);
                  });

    std::vector<std::pair<NodeHandle, NodeSerialized>> children;
    CancelToken ct;
    NodeSearchPage page{0, 0};
    table->getChildren(filter, OrderByClause::DEFAULT_ASC, children, ct, page);

    GTEST_LOG_(INFO) << "getChildren (no filter) [" << children.size()
                     << " results]: " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter";
}

// ─── 11. getChildren (filter by name) ───────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetChildren_FilterByName)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);
    ASSERT_FALSE(mSubFolderHandles.empty());

    const NodeHandle parent = mSubFolderHandles.front();

    NodeSearchFilter filter;
    filter.byLocationHandle(parent.as8byte());
    filter.byName("file_0_0"); // prefix – should match several files

    const long long us =
        measureUs(COMPLEX_ITERS,
                  [&]
                  {
                      std::vector<std::pair<NodeHandle, NodeSerialized>> children;
                      CancelToken ct;
                      NodeSearchPage page{0, 0};
                      table->getChildren(filter, OrderByClause::DEFAULT_ASC, children, ct, page);
                  });

    std::vector<std::pair<NodeHandle, NodeSerialized>> children;
    CancelToken ct;
    NodeSearchPage page{0, 0};
    table->getChildren(filter, OrderByClause::DEFAULT_ASC, children, ct, page);

    GTEST_LOG_(INFO) << "getChildren (filter by name) [" << children.size()
                     << " results]: " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter";
}

// ─── 12. getChildren (ordered by size) ──────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetChildren_OrderBySize)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);
    ASSERT_FALSE(mSubFolderHandles.empty());

    const NodeHandle parent = mSubFolderHandles.front();

    NodeSearchFilter filter;
    filter.byLocationHandle(parent.as8byte());

    const long long us =
        measureUs(COMPLEX_ITERS,
                  [&]
                  {
                      std::vector<std::pair<NodeHandle, NodeSerialized>> children;
                      CancelToken ct;
                      NodeSearchPage page{0, 0};
                      table->getChildren(filter, OrderByClause::SIZE_ASC, children, ct, page);
                  });

    std::vector<std::pair<NodeHandle, NodeSerialized>> children;
    CancelToken ct;
    NodeSearchPage page{0, 0};
    table->getChildren(filter, OrderByClause::SIZE_ASC, children, ct, page);

    GTEST_LOG_(INFO) << "getChildren (order by size ASC) [" << children.size()
                     << " results]: " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter";
}

// ─── 13. getChildren (ordered by mtime) ─────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetChildren_OrderByMtime)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);
    ASSERT_FALSE(mSubFolderHandles.empty());

    const NodeHandle parent = mSubFolderHandles.front();

    NodeSearchFilter filter;
    filter.byLocationHandle(parent.as8byte());

    const long long us =
        measureUs(COMPLEX_ITERS,
                  [&]
                  {
                      std::vector<std::pair<NodeHandle, NodeSerialized>> children;
                      CancelToken ct;
                      NodeSearchPage page{0, 0};
                      table->getChildren(filter, OrderByClause::MTIME_DESC, children, ct, page);
                  });

    std::vector<std::pair<NodeHandle, NodeSerialized>> children;
    CancelToken ct;
    NodeSearchPage page{0, 0};
    table->getChildren(filter, OrderByClause::MTIME_DESC, children, ct, page);

    GTEST_LOG_(INFO) << "getChildren (order by mtime DESC) [" << children.size()
                     << " results]: " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter";
}

// ─── 14. getChildren (paginated) ────────────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetChildren_Paginated)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);
    ASSERT_FALSE(mSubFolderHandles.empty());

    const NodeHandle parent = mSubFolderHandles.front();

    NodeSearchFilter filter;
    filter.byLocationHandle(parent.as8byte());

    constexpr size_t PAGE_SIZE = 20;
    constexpr size_t PAGE_OFFSET = 10;

    const long long us =
        measureUs(COMPLEX_ITERS,
                  [&]
                  {
                      std::vector<std::pair<NodeHandle, NodeSerialized>> children;
                      CancelToken ct;
                      NodeSearchPage page{PAGE_OFFSET, PAGE_SIZE};
                      table->getChildren(filter, OrderByClause::DEFAULT_ASC, children, ct, page);
                  });

    std::vector<std::pair<NodeHandle, NodeSerialized>> children;
    CancelToken ct;
    NodeSearchPage page{PAGE_OFFSET, PAGE_SIZE};
    table->getChildren(filter, OrderByClause::DEFAULT_ASC, children, ct, page);

    EXPECT_LE(children.size(), PAGE_SIZE);
    GTEST_LOG_(INFO) << "getChildren (paginated offset=" << PAGE_OFFSET << " size=" << PAGE_SIZE
                     << ") [" << children.size() << " results]: " << COMPLEX_ITERS
                     << " iters, total " << us << " us, avg " << us / COMPLEX_ITERS << " us/iter";
}

// ─── 15. listChildNodesLexicographically (no offset) ────────────────────────
TEST_F(SqliteNodesPerfTest, PerfListChildNodesLexicographically_NoOffset)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);
    ASSERT_FALSE(mSubFolderHandles.empty());

    const handle parent = mSubFolderHandles.front().as8byte();

    const long long us =
        measureUs(COMPLEX_ITERS,
                  [&]
                  {
                      std::vector<std::pair<NodeHandle, NodeSerialized>> children;
                      CancelToken ct;
                      table->listChildNodesLexicographically(parent,
                                                             children,
                                                             ct,
                                                             /*maxElements=*/50,
                                                             /*offset=*/std::nullopt);
                  });

    std::vector<std::pair<NodeHandle, NodeSerialized>> children;
    CancelToken ct;
    table->listChildNodesLexicographically(parent, children, ct, 50, std::nullopt);

    GTEST_LOG_(INFO) << "listChildNodesLexicographically (no offset) [" << children.size()
                     << " results]: " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter";
}

// ─── 16. listChildNodesLexicographically (with offset) ──────────────────────
TEST_F(SqliteNodesPerfTest, PerfListChildNodesLexicographically_WithOffset)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);
    ASSERT_FALSE(mSubFolderHandles.empty());

    const handle parent = mSubFolderHandles.front().as8byte();

    NodeSearchLexicographicalOffset offset;
    offset.mLastName = "file_0_0_010.txt"; // arbitrary cursor position

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> children;
                                       CancelToken ct;
                                       table->listChildNodesLexicographically(parent,
                                                                              children,
                                                                              ct,
                                                                              /*maxElements=*/30,
                                                                              offset);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> children;
    CancelToken ct;
    table->listChildNodesLexicographically(parent, children, ct, 30, offset);

    GTEST_LOG_(INFO) << "listChildNodesLexicographically (with offset) [" << children.size()
                     << " results]: " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter";
}

// ─── 17. searchNodes (recursive, from root, no filter) ──────────────────────
TEST_F(SqliteNodesPerfTest, PerfSearchNodes_FromRoot_NoFilter)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});

    const long long us =
        measureUs(COMPLEX_ITERS,
                  [&]
                  {
                      std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                      CancelToken ct;
                      NodeSearchPage page{0, 0};
                      table->searchNodes(filter, OrderByClause::DEFAULT_ASC, nodes, ct, page);
                  });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    NodeSearchPage page{0, 0};
    table->searchNodes(filter, OrderByClause::DEFAULT_ASC, nodes, ct, page);

    GTEST_LOG_(INFO) << "searchNodes (from root, no filter) [" << nodes.size()
                     << " results]: " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter";
}

// ─── 18. searchNodes (recursive, from root, filter by name) ─────────────────
TEST_F(SqliteNodesPerfTest, PerfSearchNodes_FromRoot_FilterByName)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byName("file_0_0"); // substring – matches many files

    const long long us =
        measureUs(COMPLEX_ITERS,
                  [&]
                  {
                      std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                      CancelToken ct;
                      NodeSearchPage page{0, 0};
                      table->searchNodes(filter, OrderByClause::DEFAULT_ASC, nodes, ct, page);
                  });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    NodeSearchPage page{0, 0};
    table->searchNodes(filter, OrderByClause::DEFAULT_ASC, nodes, ct, page);

    GTEST_LOG_(INFO) << "searchNodes (from root, filter name='file_0_0') [" << nodes.size()
                     << " results]: " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter";
}

// ─── 19. searchNodes (recursive, from root, filter FILENODE type) ────────────
TEST_F(SqliteNodesPerfTest, PerfSearchNodes_FromRoot_FilterByType)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);

    const long long us =
        measureUs(COMPLEX_ITERS,
                  [&]
                  {
                      std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                      CancelToken ct;
                      NodeSearchPage page{0, 0};
                      table->searchNodes(filter, OrderByClause::DEFAULT_ASC, nodes, ct, page);
                  });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    NodeSearchPage page{0, 0};
    table->searchNodes(filter, OrderByClause::DEFAULT_ASC, nodes, ct, page);

    GTEST_LOG_(INFO) << "searchNodes (from root, type=FILENODE) [" << nodes.size()
                     << " results]: " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter";
}

// ─── 20. searchNodes (recursive, from root, order by ctime DESC) ─────────────
TEST_F(SqliteNodesPerfTest, PerfSearchNodes_FromRoot_OrderByCtime)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);

    const long long us =
        measureUs(COMPLEX_ITERS,
                  [&]
                  {
                      std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                      CancelToken ct;
                      NodeSearchPage page{0, 100}; // top-100 by ctime
                      table->searchNodes(filter, OrderByClause::CTIME_DESC, nodes, ct, page);
                  });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    NodeSearchPage page{0, 100};
    table->searchNodes(filter, OrderByClause::CTIME_DESC, nodes, ct, page);

    GTEST_LOG_(INFO) << "searchNodes (from root, FILENODE, order by ctime DESC, limit 100) ["
                     << nodes.size() << " results]: " << COMPLEX_ITERS << " iters, total " << us
                     << " us, avg " << us / COMPLEX_ITERS << " us/iter";
}

// ─── 21. searchNodes (recursive, ALL_VISUAL_MEDIA, page 51-100) ─────────────
TEST_F(SqliteNodesPerfTest, PerfSearchNodes_FromRoot_AllVisualMedia_Page51To100)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    constexpr size_t pageOffset = 50;
    constexpr size_t pageSize = 50;

    const long long us =
        measureUs(COMPLEX_ITERS,
                  [&]
                  {
                      std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                      CancelToken ct;
                      NodeSearchPage page{pageOffset, pageSize};
                      table->searchNodes(filter, OrderByClause::DEFAULT_ASC, nodes, ct, page);
                  });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    NodeSearchPage page{pageOffset, pageSize};
    table->searchNodes(filter, OrderByClause::DEFAULT_ASC, nodes, ct, page);

    std::vector<std::pair<NodeHandle, NodeSerialized>> reference;
    CancelToken referenceCt;
    NodeSearchPage allResults{0, 0};
    table->searchNodes(filter, OrderByClause::DEFAULT_ASC, reference, referenceCt, allResults);

    ASSERT_GT(reference.size(), pageOffset);
    ASSERT_EQ(nodes.size(), std::min(pageSize, reference.size() - pageOffset));
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        EXPECT_EQ(nodes[i].first, reference[pageOffset + i].first);
    }

    GTEST_LOG_(INFO) << "searchNodes (from root, ALL_VISUAL_MEDIA, page 51-100) [" << nodes.size()
                     << " results]: " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter";
}

// ─── 22. searchNodes (recursive, sub-tree only) ──────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfSearchNodes_SubTree)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);
    ASSERT_FALSE(mTopFolderHandles.empty());

    NodeSearchFilter filter;
    filter.byAncestors({mTopFolderHandles.front().as8byte(), UNDEF, UNDEF});

    const long long us =
        measureUs(COMPLEX_ITERS,
                  [&]
                  {
                      std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                      CancelToken ct;
                      NodeSearchPage page{0, 0};
                      table->searchNodes(filter, OrderByClause::DEFAULT_ASC, nodes, ct, page);
                  });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    NodeSearchPage page{0, 0};
    table->searchNodes(filter, OrderByClause::DEFAULT_ASC, nodes, ct, page);

    GTEST_LOG_(INFO) << "searchNodes (sub-tree from top-folder) [" << nodes.size()
                     << " results]: " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter";
}

// ─── 23. getRecentNodes ──────────────────────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetRecentNodes)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    // "since" = 0 → return everything
    const m_time_t since = 0;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       NodeSearchPage page{0, 200}; // top-200 recent
                                       table->getRecentNodes(page, since, nodes);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    NodeSearchPage page{0, 200};
    table->getRecentNodes(page, since, nodes);

    GTEST_LOG_(INFO) << "getRecentNodes (top 200, since=0) [" << nodes.size()
                     << " results]: " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter";
}

// ─── 24. getRecentNodes (with since filter) ──────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetRecentNodes_WithSince)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    // Use a timestamp that is in the middle of the inserted nodes' ctime range.
    const m_time_t since = 1700000000LL + static_cast<int64_t>(mNextHandle / 2);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       NodeSearchPage page{0, 0};
                                       table->getRecentNodes(page, since, nodes);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    NodeSearchPage page{0, 0};
    table->getRecentNodes(page, since, nodes);

    GTEST_LOG_(INFO) << "getRecentNodes (since mid-point) [" << nodes.size()
                     << " results]: " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter";
}

// ─── 25. getFavouritesHandles ────────────────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetFavouritesHandles)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const long long us =
        measureUs(COMPLEX_ITERS,
                  [&]
                  {
                      std::vector<NodeHandle> handles;
                      table->getFavouritesHandles(mRootHandle, /*count=*/0, handles);
                  });

    std::vector<NodeHandle> handles;
    table->getFavouritesHandles(mRootHandle, 0, handles);

    GTEST_LOG_(INFO) << "getFavouritesHandles (from root) [" << handles.size()
                     << " favourites]: " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter";
}

// ─── 26. isAncestor ──────────────────────────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfIsAncestor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);
    ASSERT_FALSE(mLeafFileHandle.isUndef());

    // Verify correctness first
    CancelToken ct;
    EXPECT_TRUE(table->isAncestor(mLeafFileHandle, mRootHandle, ct));

    const long long us = measureUs(SIMPLE_ITERS,
                                   [&]
                                   {
                                       CancelToken tmp;
                                       table->isAncestor(mLeafFileHandle, mRootHandle, tmp);
                                   });

    GTEST_LOG_(INFO) << "isAncestor (leaf → root): " << SIMPLE_ITERS << " iters, total " << us
                     << " us, avg " << us / SIMPLE_ITERS << " us/iter";
}

// ─── 26. getNodesWithSharesOrLink (in-shares) ────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetNodesWithSharesOrLink_InShares)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const long long us =
        measureUs(COMPLEX_ITERS,
                  [&]
                  {
                      std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                      table->getNodesWithSharesOrLink(nodes, ShareType_t::IN_SHARES);
                  });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    table->getNodesWithSharesOrLink(nodes, ShareType_t::IN_SHARES);

    GTEST_LOG_(INFO) << "getNodesWithSharesOrLink (IN_SHARES) [" << nodes.size()
                     << " results]: " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter";
}

// ─── 27. getNodesWithSharesOrLink (public links) ─────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetNodesWithSharesOrLink_Links)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       table->getNodesWithSharesOrLink(nodes, ShareType_t::LINK);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    table->getNodesWithSharesOrLink(nodes, ShareType_t::LINK);

    GTEST_LOG_(INFO) << "getNodesWithSharesOrLink (LINK) [" << nodes.size()
                     << " results]: " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter";
}

// ─── 28. getNodeTagsBelow ────────────────────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfGetNodeTagsBelow)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       CancelToken ct;
                                       table->getNodeTagsBelow(ct, mRootHandle, /*pattern=*/"");
                                   });

    CancelToken ct;
    auto tags = table->getNodeTagsBelow(ct, mRootHandle, "");

    GTEST_LOG_(INFO) << "getNodeTagsBelow (from root, no pattern) [" << (tags ? tags->size() : 0u)
                     << " distinct tags]: " << COMPLEX_ITERS << " iters, total " << us
                     << " us, avg " << us / COMPLEX_ITERS << " us/iter";
}

// ─── 29. put (INSERT OR REPLACE) ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfPutNode)
{
    auto* table = nodesTable();
    auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get());
    ASSERT_NE(table, nullptr);
    ASSERT_NE(sa, nullptr);

    // Create a fresh node outside the main dataset; re-insert it every iteration.
    NodeHandle freshHandle = NodeHandle().set6byte(mNextHandle++);
    Node& freshRef = mt::makeNode(*mClient,
                                  FILENODE,
                                  freshHandle,
                                  /*parent=*/nullptr);
    auto freshNode = std::shared_ptr<Node>(&freshRef);

    static const nameid nameId = AttrMap::string2nameid("n");
    freshNode->attrs.map[nameId] = "perf_test_node.dat";
    freshNode->size = 1024;
    freshNode->ctime = 1700000000LL;
    freshNode->mtime = 1700000000LL;
    freshNode->isvalid = false; // no CRC → fingerprint BLOB will be empty

    mClient->mNodeManager.addNode(freshNode, false, true, mMissingParentNodes);

    const long long us = measureUs(SIMPLE_ITERS,
                                   [&]
                                   {
                                       sa->put(freshNode.get());
                                   });

    GTEST_LOG_(INFO) << "put(Node*) (INSERT OR REPLACE) [NO listAll index]: " << SIMPLE_ITERS
                     << " iters, total " << us << " us, avg " << us / SIMPLE_ITERS << " us/iter";
}

// ─── 30. updateCounter ───────────────────────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfUpdateCounter)
{
    auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get());
    ASSERT_NE(sa, nullptr);
    ASSERT_FALSE(mLeafFileHandle.isUndef());

    const NodeCounter counter;
    const std::string blob = counter.serialize();

    const long long us = measureUs(SIMPLE_ITERS,
                                   [&]
                                   {
                                       sa->updateCounter(mLeafFileHandle, blob);
                                   });

    GTEST_LOG_(INFO) << "updateCounter: " << SIMPLE_ITERS << " iters, total " << us << " us, avg "
                     << us / SIMPLE_ITERS << " us/iter";
}

// ─── 31. updateCounterAndFlags ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfUpdateCounterAndFlags)
{
    auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get());
    ASSERT_NE(sa, nullptr);
    ASSERT_FALSE(mLeafFileHandle.isUndef());

    const NodeCounter counter;
    const std::string blob = counter.serialize();
    const uint64_t flags = 0;

    const long long us = measureUs(SIMPLE_ITERS,
                                   [&]
                                   {
                                       sa->updateCounterAndFlags(mLeafFileHandle, flags, blob);
                                   });

    GTEST_LOG_(INFO) << "updateCounterAndFlags: " << SIMPLE_ITERS << " iters, total " << us
                     << " us, avg " << us / SIMPLE_ITERS << " us/iter";
}

// ─── 32. remove (single node) ────────────────────────────────────────────────
TEST_F(SqliteNodesPerfTest, PerfRemoveNode)
{
    auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get());
    ASSERT_NE(sa, nullptr);
    ASSERT_FALSE(mFileHandles.empty());
    ASSERT_GE(mFileHandles.size(), static_cast<size_t>(SIMPLE_ITERS));

    const long long us = measureUs(SIMPLE_ITERS,
                                   [&, idx = 0]() mutable
                                   {
                                       sa->remove(mFileHandles[static_cast<size_t>(idx)]);
                                       ++idx;
                                   });

    GTEST_LOG_(INFO) << "remove(NodeHandle) [NO listAll index]: " << SIMPLE_ITERS
                     << " iters, total " << us << " us, avg " << us / SIMPLE_ITERS << " us/iter";
}

// ═══════════════════════════════════════════════════════════════════════════════
//  searchNodesByPage performance benchmarks  (SqliteNodesPerfSearchTest)
//
//  SqliteNodesPerfSearchTest extends SqliteNodesPerfTest; no extra indexes are
//  needed because the base SetUp() already creates the search index set
//  (enableSearch = true).
//
//  Test inventory (40 cases):
//    1–20  : MIME_TYPE_VIDEO        × 10 sort orders × {first page, mid cursor}
//    21–40 : MIME_TYPE_ALL_VISUAL_MEDIA × 10 sort orders × {first page, mid cursor}
//
//  Order: DEFAULT_ASC/DESC → MTIME_ASC/DESC → SIZE_ASC/DESC →
//         FAV_ASC/DESC → LABEL_ASC/DESC.
//
//  The mid-cursor is anchored at the representative .mp4 node (i=0, j=0, k=25)
//  via videoLeafCursor().  For SIZE_* orders the cursor uses mLastSize = 0
//  (sizeVirtual is not pre-computed), so name/handle tie-breaking drives
//  ordering — the SQL code path is identical to production.
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * @brief Groups all searchNodesByPage benchmarks under a single fixture.
 *
 * Inherits the fully-populated DB and the search index set from
 * SqliteNodesPerfTest::SetUp() without any additional configuration.
 */
class SqliteNodesPerfSearchTest: public SqliteNodesPerfTest
{};

// ─── 29. put (INSERT OR REPLACE) ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, PerfPutNode)
{
    auto* table = nodesTable();
    auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get());
    ASSERT_NE(table, nullptr);
    ASSERT_NE(sa, nullptr);

    // Create a fresh node outside the main dataset; re-insert it every iteration.
    NodeHandle freshHandle = NodeHandle().set6byte(mNextHandle++);
    Node& freshRef = mt::makeNode(*mClient,
                                  FILENODE,
                                  freshHandle,
                                  /*parent=*/nullptr);
    auto freshNode = std::shared_ptr<Node>(&freshRef);

    static const nameid nameId = AttrMap::string2nameid("n");
    freshNode->attrs.map[nameId] = "perf_test_node.dat";
    freshNode->size = 1024;
    freshNode->ctime = 1700000000LL;
    freshNode->mtime = 1700000000LL;
    freshNode->isvalid = false; // no CRC → fingerprint BLOB will be empty

    mClient->mNodeManager.addNode(freshNode, false, true, mMissingParentNodes);

    const long long us = measureUs(SIMPLE_ITERS,
                                   [&]
                                   {
                                       sa->put(freshNode.get());
                                   });

    GTEST_LOG_(INFO) << "put(Node*) (INSERT OR REPLACE) [NO listAll index]: " << SIMPLE_ITERS
                     << " iters, total " << us << " us, avg " << us / SIMPLE_ITERS << " us/iter";
}

TEST_F(SqliteNodesPerfSearchTest, PerfRemoveNode)
{
    auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get());
    ASSERT_NE(sa, nullptr);
    ASSERT_FALSE(mFileHandles.empty());
    ASSERT_GE(mFileHandles.size(), static_cast<size_t>(SIMPLE_ITERS));

    const long long us = measureUs(SIMPLE_ITERS,
                                   [&, idx = 0]() mutable
                                   {
                                       sa->remove(mFileHandles[static_cast<size_t>(idx)]);
                                       ++idx;
                                   });

    GTEST_LOG_(INFO) << "remove(NodeHandle) [NO listAll index]: " << SIMPLE_ITERS
                     << " iters, total " << us << " us, avg " << us / SIMPLE_ITERS << " us/iter";
}

// ═══════════════════════════════════════════════════════════════════════════
//  searchNodesByPage – MIME_TYPE_VIDEO
//  All 10 sort orders × {first page (no cursor), mid cursor} = 20 cases.
// ═══════════════════════════════════════════════════════════════════════════

// ─── 1. DEFAULT_ASC, first page ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_DefaultAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::DEFAULT_ASC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::DEFAULT_ASC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (DEFAULT_ASC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 2. DEFAULT_ASC, mid cursor ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_DefaultAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::DEFAULT_ASC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::DEFAULT_ASC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                cursor);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::DEFAULT_ASC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (DEFAULT_ASC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 3. DEFAULT_DESC, first page ────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_DefaultDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::DEFAULT_DESC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table
        ->searchNodesByPage(filter, OrderByClause::DEFAULT_DESC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (DEFAULT_DESC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 4. DEFAULT_DESC, mid cursor ────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_DefaultDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::DEFAULT_DESC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::DEFAULT_DESC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                cursor);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::DEFAULT_DESC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (DEFAULT_DESC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 5. MTIME_ASC, first page ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_MtimeAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::MTIME_ASC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::MTIME_ASC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (MTIME_ASC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 6. MTIME_ASC, mid cursor ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_MtimeAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::MTIME_ASC);

    const long long us = measureUs(
        COMPLEX_ITERS,
        [&]
        {
            std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
            CancelToken ct;
            table->searchNodesByPage(filter, OrderByClause::MTIME_ASC, nodes, ct, pageSize, cursor);
        });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::MTIME_ASC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (MTIME_ASC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 7. MTIME_DESC, first page ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_MtimeDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::MTIME_DESC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::MTIME_DESC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (MTIME_DESC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 8. MTIME_DESC, mid cursor ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_MtimeDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::MTIME_DESC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::MTIME_DESC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                cursor);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::MTIME_DESC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (MTIME_DESC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 9. SIZE_ASC, first page ────────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_SizeAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::SIZE_ASC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::SIZE_ASC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (SIZE_ASC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 10. SIZE_ASC, mid cursor ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_SizeAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::SIZE_ASC);

    const long long us = measureUs(
        COMPLEX_ITERS,
        [&]
        {
            std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
            CancelToken ct;
            table->searchNodesByPage(filter, OrderByClause::SIZE_ASC, nodes, ct, pageSize, cursor);
        });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::SIZE_ASC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (SIZE_ASC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 11. SIZE_DESC, first page ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_SizeDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::SIZE_DESC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::SIZE_DESC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (SIZE_DESC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 12. SIZE_DESC, mid cursor ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_SizeDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::SIZE_DESC);

    const long long us = measureUs(
        COMPLEX_ITERS,
        [&]
        {
            std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
            CancelToken ct;
            table->searchNodesByPage(filter, OrderByClause::SIZE_DESC, nodes, ct, pageSize, cursor);
        });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::SIZE_DESC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (SIZE_DESC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 13. FAV_ASC, first page ────────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_FavAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::FAV_ASC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::FAV_ASC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (FAV_ASC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 14. FAV_ASC, mid cursor ────────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_FavAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::FAV_ASC);

    const long long us = measureUs(
        COMPLEX_ITERS,
        [&]
        {
            std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
            CancelToken ct;
            table->searchNodesByPage(filter, OrderByClause::FAV_ASC, nodes, ct, pageSize, cursor);
        });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::FAV_ASC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (FAV_ASC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 15. FAV_DESC, first page ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_FavDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::FAV_DESC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::FAV_DESC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (FAV_DESC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 16. FAV_DESC, mid cursor ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_FavDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::FAV_DESC);

    const long long us = measureUs(
        COMPLEX_ITERS,
        [&]
        {
            std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
            CancelToken ct;
            table->searchNodesByPage(filter, OrderByClause::FAV_DESC, nodes, ct, pageSize, cursor);
        });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::FAV_DESC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (FAV_DESC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 17. LABEL_ASC, first page ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_LabelAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::LABEL_ASC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::LABEL_ASC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (LABEL_ASC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 18. LABEL_ASC, mid cursor ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_LabelAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::LABEL_ASC);

    const long long us = measureUs(
        COMPLEX_ITERS,
        [&]
        {
            std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
            CancelToken ct;
            table->searchNodesByPage(filter, OrderByClause::LABEL_ASC, nodes, ct, pageSize, cursor);
        });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::LABEL_ASC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (LABEL_ASC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 19. LABEL_DESC, first page ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_LabelDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::LABEL_DESC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::LABEL_DESC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (LABEL_DESC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 20. LABEL_DESC, mid cursor ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_Video_LabelDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_VIDEO);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::LABEL_DESC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::LABEL_DESC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                cursor);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::LABEL_DESC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (LABEL_DESC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ═══════════════════════════════════════════════════════════════════════════
//  searchNodesByPage – MIME_TYPE_ALL_VISUAL_MEDIA
//  All 10 sort orders × {first page (no cursor), mid cursor} = 20 cases.
//  Mid-cursor uses videoLeafCursor() (.mp4 node), which is within the
//  ALL_VISUAL_MEDIA result set.
// ═══════════════════════════════════════════════════════════════════════════

// ─── 21. DEFAULT_ASC, first page ────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_DefaultAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::DEFAULT_ASC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::DEFAULT_ASC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (DEFAULT_ASC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 22. DEFAULT_ASC, mid cursor ────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_DefaultAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::DEFAULT_ASC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::DEFAULT_ASC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                cursor);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::DEFAULT_ASC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (DEFAULT_ASC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 23. DEFAULT_DESC, first page ───────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_DefaultDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::DEFAULT_DESC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table
        ->searchNodesByPage(filter, OrderByClause::DEFAULT_DESC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (DEFAULT_DESC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 24. DEFAULT_DESC, mid cursor ───────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_DefaultDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::DEFAULT_DESC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::DEFAULT_DESC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                cursor);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::DEFAULT_DESC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (DEFAULT_DESC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 25. MTIME_ASC, first page ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_MtimeAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::MTIME_ASC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::MTIME_ASC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (MTIME_ASC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 26. MTIME_ASC, mid cursor ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_MtimeAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::MTIME_ASC);

    const long long us = measureUs(
        COMPLEX_ITERS,
        [&]
        {
            std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
            CancelToken ct;
            table->searchNodesByPage(filter, OrderByClause::MTIME_ASC, nodes, ct, pageSize, cursor);
        });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::MTIME_ASC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (MTIME_ASC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 27. MTIME_DESC, first page ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_MtimeDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::MTIME_DESC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::MTIME_DESC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (MTIME_DESC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 28. MTIME_DESC, mid cursor ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_MtimeDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::MTIME_DESC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::MTIME_DESC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                cursor);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::MTIME_DESC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (MTIME_DESC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 29. SIZE_ASC, first page ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_SizeAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::SIZE_ASC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::SIZE_ASC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (SIZE_ASC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 30. SIZE_ASC, mid cursor ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_SizeAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::SIZE_ASC);

    const long long us = measureUs(
        COMPLEX_ITERS,
        [&]
        {
            std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
            CancelToken ct;
            table->searchNodesByPage(filter, OrderByClause::SIZE_ASC, nodes, ct, pageSize, cursor);
        });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::SIZE_ASC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (SIZE_ASC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 31. SIZE_DESC, first page ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_SizeDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::SIZE_DESC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::SIZE_DESC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (SIZE_DESC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 32. SIZE_DESC, mid cursor ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_SizeDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::SIZE_DESC);

    const long long us = measureUs(
        COMPLEX_ITERS,
        [&]
        {
            std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
            CancelToken ct;
            table->searchNodesByPage(filter, OrderByClause::SIZE_DESC, nodes, ct, pageSize, cursor);
        });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::SIZE_DESC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (SIZE_DESC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 33. FAV_ASC, first page ────────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_FavAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::FAV_ASC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::FAV_ASC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (FAV_ASC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 34. FAV_ASC, mid cursor ────────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_FavAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::FAV_ASC);

    const long long us = measureUs(
        COMPLEX_ITERS,
        [&]
        {
            std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
            CancelToken ct;
            table->searchNodesByPage(filter, OrderByClause::FAV_ASC, nodes, ct, pageSize, cursor);
        });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::FAV_ASC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (FAV_ASC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 35. FAV_DESC, first page ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_FavDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::FAV_DESC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::FAV_DESC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (FAV_DESC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 36. FAV_DESC, mid cursor ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_FavDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::FAV_DESC);

    const long long us = measureUs(
        COMPLEX_ITERS,
        [&]
        {
            std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
            CancelToken ct;
            table->searchNodesByPage(filter, OrderByClause::FAV_DESC, nodes, ct, pageSize, cursor);
        });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::FAV_DESC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (FAV_DESC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 37. LABEL_ASC, first page ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_LabelAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::LABEL_ASC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::LABEL_ASC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (LABEL_ASC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 38. LABEL_ASC, mid cursor ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_LabelAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::LABEL_ASC);

    const long long us = measureUs(
        COMPLEX_ITERS,
        [&]
        {
            std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
            CancelToken ct;
            table->searchNodesByPage(filter, OrderByClause::LABEL_ASC, nodes, ct, pageSize, cursor);
        });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::LABEL_ASC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (LABEL_ASC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 39. LABEL_DESC, first page ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_LabelDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::LABEL_DESC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                std::nullopt);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::LABEL_DESC, nodes, ct, pageSize, std::nullopt);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "searchNodesByPage (LABEL_DESC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 40. LABEL_DESC, mid cursor ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfSearchTest, SearchByPage_AllVisualMedia_LabelDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::LABEL_DESC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->searchNodesByPage(filter,
                                                                OrderByClause::LABEL_DESC,
                                                                nodes,
                                                                ct,
                                                                pageSize,
                                                                cursor);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->searchNodesByPage(filter, OrderByClause::LABEL_DESC, nodes, ct, pageSize, cursor);

    GTEST_LOG_(INFO) << "searchNodesByPage (LABEL_DESC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ═══════════════════════════════════════════════════════════════════════════
//  listAllNodesByPage performance benchmarks  (SqliteNodesPerfListAllTest)
//
//  SqliteNodesPerfListAllTest extends SqliteNodesPerfTest with the full
//  search index set enabled (which includes the listallnodes* indexes), so
//  every query below uses the dedicated listallnodes* indexes rather than a full scan.
//
//  Test inventory (42 cases):
//    1–2   : PutNode / RemoveNode  — insert/delete cost with the index active
//    3–22  : MIME_TYPE_ALL_VISUAL_MEDIA × 10 sort orders × {first page, mid cursor}
//    23–42 : MIME_TYPE_VIDEO        × 10 sort orders × {first page, mid cursor}
//
//  No wall-clock assertions are made; the suite never fails on slow machines.
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Extends SqliteNodesPerfTest with all search indexes (including listallnodes* indexes)
 * enabled.
 *
 * SetUp() calls the parent's SetUp() (which populates the DB and creates the
 * search + lexicographic indexes), then re-invokes createIndexes with enableSearch=true
 * to ensure the listallnodes* indexes are present.
 * Using CREATE INDEX IF NOT EXISTS means the call is idempotent and safe even
 * if the parent already created some of the same indexes.
 */
class SqliteNodesPerfListAllTest: public SqliteNodesPerfTest
{
protected:
    void SetUp() override
    {
        SqliteNodesPerfTest::SetUp();

        if (auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get()))
        {
            // opensctable() leaves an open transaction (via sctable->begin()).
            // dropDBIndexes() inside createIndexes() asserts !inTransaction(), so
            // we must commit the open transaction first, then restart it afterwards.
            sa->commit();
            sa->createIndexes(/*enableSearch=*/true,
                              /*enableLexi=*/true);
            sa->begin();
        }
    }
};

// ─── 1. PutNode ─────────────────────────────────────────────────────────────
//
// Measures INSERT OR REPLACE latency with the listAllNodes index set active.
// Compare against the base-fixture PutNode to quantify index maintenance cost.
TEST_F(SqliteNodesPerfListAllTest, PutNode)
{
    auto* table = nodesTable();
    auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get());
    ASSERT_NE(table, nullptr);
    ASSERT_NE(sa, nullptr);

    NodeHandle freshHandle = NodeHandle().set6byte(mNextHandle++);
    Node& freshRef = mt::makeNode(*mClient, FILENODE, freshHandle, /*parent=*/nullptr);
    auto freshNode = std::shared_ptr<Node>(&freshRef);

    static const nameid nameId = AttrMap::string2nameid("n");
    freshNode->attrs.map[nameId] = "perf_test_node_with_index.dat";
    freshNode->size = 1024;
    freshNode->ctime = 1700000000LL;
    freshNode->mtime = 1700000000LL;
    freshNode->isvalid = false;

    mClient->mNodeManager.addNode(freshNode,
                                  /*notify=*/false,
                                  /*isFetching=*/true,
                                  mMissingParentNodes);

    const long long us = measureUs(SIMPLE_ITERS,
                                   [&]
                                   {
                                       sa->put(freshNode.get());
                                   });

    GTEST_LOG_(INFO) << "put(Node*) [WITH listAll index]: " << SIMPLE_ITERS << " iters, total "
                     << us << " us, avg " << us / SIMPLE_ITERS << " us/iter";
}

// ─── 2. RemoveNode ──────────────────────────────────────────────────────────
//
// Measures per-node removal latency with the listAllNodes index set active.
// Compare against the base-fixture RemoveNode to quantify index maintenance cost.
TEST_F(SqliteNodesPerfListAllTest, RemoveNode)
{
    auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get());
    ASSERT_NE(sa, nullptr);
    ASSERT_FALSE(mFileHandles.empty());
    ASSERT_GE(mFileHandles.size(), static_cast<size_t>(SIMPLE_ITERS));

    const long long us = measureUs(SIMPLE_ITERS,
                                   [&, idx = 0]() mutable
                                   {
                                       sa->remove(mFileHandles[static_cast<size_t>(idx)]);
                                       ++idx;
                                   });

    GTEST_LOG_(INFO) << "remove(NodeHandle) [WITH listAll index]: " << SIMPLE_ITERS
                     << " iters, total " << us << " us, avg " << us / SIMPLE_ITERS << " us/iter";
}

// ═══════════════════════════════════════════════════════════════════════════
//  listAllNodesByPage – MIME_TYPE_ALL_VISUAL_MEDIA
//
//  All 10 sort orders × {first page (no cursor), mid cursor} = 20 cases.
//  Order: DEFAULT_ASC/DESC → MTIME_ASC/DESC → SIZE_ASC/DESC →
//         FAV_ASC/DESC → LABEL_ASC/DESC.
//  The mid-cursor uses leafCursor(), anchored at the middle file of the first
//  sub-folder (k=490, a .jpg node), which is always within the result set.
// ═══════════════════════════════════════════════════════════════════════════

// ─── 3. DEFAULT_ASC, first page ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_DefaultAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::DEFAULT_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::DEFAULT_ASC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (DEFAULT_ASC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 4. DEFAULT_ASC, mid cursor ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_DefaultAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = leafCursor(OrderByClause::DEFAULT_ASC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::DEFAULT_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::DEFAULT_ASC,
                              nodes,
                              ct,
                              pageSize,
                              cursor,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    GTEST_LOG_(INFO) << "listAllNodesByPage (DEFAULT_ASC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 5. DEFAULT_DESC, first page ────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_DefaultDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::DEFAULT_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::DEFAULT_DESC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (DEFAULT_DESC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 6. DEFAULT_DESC, mid cursor ────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_DefaultDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = leafCursor(OrderByClause::DEFAULT_DESC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::DEFAULT_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::DEFAULT_DESC,
                              nodes,
                              ct,
                              pageSize,
                              cursor,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    GTEST_LOG_(INFO) << "listAllNodesByPage (DEFAULT_DESC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 7. MTIME_ASC, first page ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_MtimeAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::MTIME_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::MTIME_ASC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (MTIME_ASC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 8. MTIME_ASC, mid cursor ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_MtimeAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = leafCursor(OrderByClause::MTIME_ASC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::MTIME_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::MTIME_ASC,
                              nodes,
                              ct,
                              pageSize,
                              cursor,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    GTEST_LOG_(INFO) << "listAllNodesByPage (MTIME_ASC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 9. MTIME_DESC, first page ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_MtimeDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::MTIME_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::MTIME_DESC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (MTIME_DESC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 10. MTIME_DESC, mid cursor ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_MtimeDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = leafCursor(OrderByClause::MTIME_DESC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::MTIME_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::MTIME_DESC,
                              nodes,
                              ct,
                              pageSize,
                              cursor,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    GTEST_LOG_(INFO) << "listAllNodesByPage (MTIME_DESC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 11. SIZE_ASC, first page ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_SizeAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::SIZE_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::SIZE_ASC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (SIZE_ASC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 12. SIZE_ASC, mid cursor ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_SizeAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = leafCursor(OrderByClause::SIZE_ASC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::SIZE_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::SIZE_ASC,
                              nodes,
                              ct,
                              pageSize,
                              cursor,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    GTEST_LOG_(INFO) << "listAllNodesByPage (SIZE_ASC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 13. SIZE_DESC, first page ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_SizeDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::SIZE_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::SIZE_DESC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (SIZE_DESC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 14. SIZE_DESC, mid cursor ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_SizeDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = leafCursor(OrderByClause::SIZE_DESC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::SIZE_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::SIZE_DESC,
                              nodes,
                              ct,
                              pageSize,
                              cursor,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    GTEST_LOG_(INFO) << "listAllNodesByPage (SIZE_DESC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 15. FAV_ASC, first page ────────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_FavAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::FAV_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::FAV_ASC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (FAV_ASC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 16. FAV_ASC, mid cursor ────────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_FavAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = leafCursor(OrderByClause::FAV_ASC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::FAV_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::FAV_ASC,
                              nodes,
                              ct,
                              pageSize,
                              cursor,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    GTEST_LOG_(INFO) << "listAllNodesByPage (FAV_ASC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 17. FAV_DESC, first page ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_FavDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::FAV_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::FAV_DESC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (FAV_DESC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 18. FAV_DESC, mid cursor ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_FavDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = leafCursor(OrderByClause::FAV_DESC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::FAV_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::FAV_DESC,
                              nodes,
                              ct,
                              pageSize,
                              cursor,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    GTEST_LOG_(INFO) << "listAllNodesByPage (FAV_DESC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 19. LABEL_ASC, first page ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_LabelAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::LABEL_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::LABEL_ASC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (LABEL_ASC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 20. LABEL_ASC, mid cursor ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_LabelAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = leafCursor(OrderByClause::LABEL_ASC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::LABEL_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::LABEL_ASC,
                              nodes,
                              ct,
                              pageSize,
                              cursor,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    GTEST_LOG_(INFO) << "listAllNodesByPage (LABEL_ASC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 21. LABEL_DESC, first page ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_LabelDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::LABEL_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::LABEL_DESC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (LABEL_DESC, ALL_VISUAL_MEDIA, first page, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 22. LABEL_DESC, mid cursor ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_AllVisualMedia_LabelDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = leafCursor(OrderByClause::LABEL_DESC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::LABEL_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_ALL_VISUAL_MEDIA);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::LABEL_DESC,
                              nodes,
                              ct,
                              pageSize,
                              cursor,
                              MIME_TYPE_ALL_VISUAL_MEDIA);

    GTEST_LOG_(INFO) << "listAllNodesByPage (LABEL_DESC, ALL_VISUAL_MEDIA, mid cursor, limit "
                     << pageSize << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ═══════════════════════════════════════════════════════════════════════════
//  listAllNodesByPage – MIME_TYPE_VIDEO
//
//  All 10 sort orders × {first page (no cursor), mid cursor} = 20 cases.
//  Order: DEFAULT_ASC/DESC → MTIME_ASC/DESC → SIZE_ASC/DESC →
//         FAV_ASC/DESC → LABEL_ASC/DESC.
//  The mid-cursor uses videoLeafCursor(), anchored at the representative .mp4
//  node (i=0, j=0, k=25), which is always within the VIDEO result set.
// ═══════════════════════════════════════════════════════════════════════════

// ─── 23. DEFAULT_ASC, first page ────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_DefaultAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::DEFAULT_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::DEFAULT_ASC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_VIDEO);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (DEFAULT_ASC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 24. DEFAULT_ASC, mid cursor ────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_DefaultAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::DEFAULT_ASC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::DEFAULT_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::DEFAULT_ASC,
                              nodes,
                              ct,
                              pageSize,
                              cursor,
                              MIME_TYPE_VIDEO);

    GTEST_LOG_(INFO) << "listAllNodesByPage (DEFAULT_ASC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 25. DEFAULT_DESC, first page ───────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_DefaultDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::DEFAULT_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::DEFAULT_DESC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_VIDEO);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (DEFAULT_DESC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 26. DEFAULT_DESC, mid cursor ───────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_DefaultDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::DEFAULT_DESC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::DEFAULT_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::DEFAULT_DESC,
                              nodes,
                              ct,
                              pageSize,
                              cursor,
                              MIME_TYPE_VIDEO);

    GTEST_LOG_(INFO) << "listAllNodesByPage (DEFAULT_DESC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 27. MTIME_ASC, first page ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_MtimeAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::MTIME_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::MTIME_ASC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_VIDEO);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (MTIME_ASC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 28. MTIME_ASC, mid cursor ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_MtimeAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::MTIME_ASC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::MTIME_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::MTIME_ASC,
                              nodes,
                              ct,
                              pageSize,
                              cursor,
                              MIME_TYPE_VIDEO);

    GTEST_LOG_(INFO) << "listAllNodesByPage (MTIME_ASC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 29. MTIME_DESC, first page ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_MtimeDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::MTIME_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::MTIME_DESC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_VIDEO);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (MTIME_DESC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 30. MTIME_DESC, mid cursor ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_MtimeDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::MTIME_DESC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::MTIME_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::MTIME_DESC,
                              nodes,
                              ct,
                              pageSize,
                              cursor,
                              MIME_TYPE_VIDEO);

    GTEST_LOG_(INFO) << "listAllNodesByPage (MTIME_DESC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 31. SIZE_ASC, first page ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_SizeAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::SIZE_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::SIZE_ASC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_VIDEO);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (SIZE_ASC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 32. SIZE_ASC, mid cursor ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_SizeAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::SIZE_ASC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::SIZE_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table
        ->listAllNodesByPage(OrderByClause::SIZE_ASC, nodes, ct, pageSize, cursor, MIME_TYPE_VIDEO);

    GTEST_LOG_(INFO) << "listAllNodesByPage (SIZE_ASC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 33. SIZE_DESC, first page ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_SizeDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::SIZE_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::SIZE_DESC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_VIDEO);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (SIZE_DESC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 34. SIZE_DESC, mid cursor ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_SizeDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::SIZE_DESC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::SIZE_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::SIZE_DESC,
                              nodes,
                              ct,
                              pageSize,
                              cursor,
                              MIME_TYPE_VIDEO);

    GTEST_LOG_(INFO) << "listAllNodesByPage (SIZE_DESC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 35. FAV_ASC, first page ────────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_FavAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::FAV_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::FAV_ASC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_VIDEO);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (FAV_ASC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 36. FAV_ASC, mid cursor ────────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_FavAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::FAV_ASC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::FAV_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::FAV_ASC, nodes, ct, pageSize, cursor, MIME_TYPE_VIDEO);

    GTEST_LOG_(INFO) << "listAllNodesByPage (FAV_ASC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 37. FAV_DESC, first page ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_FavDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::FAV_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::FAV_DESC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_VIDEO);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (FAV_DESC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 38. FAV_DESC, mid cursor ───────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_FavDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::FAV_DESC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::FAV_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table
        ->listAllNodesByPage(OrderByClause::FAV_DESC, nodes, ct, pageSize, cursor, MIME_TYPE_VIDEO);

    GTEST_LOG_(INFO) << "listAllNodesByPage (FAV_DESC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 39. LABEL_ASC, first page ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_LabelAsc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::LABEL_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::LABEL_ASC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_VIDEO);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (LABEL_ASC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 40. LABEL_ASC, mid cursor ──────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_LabelAsc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::LABEL_ASC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::LABEL_ASC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::LABEL_ASC,
                              nodes,
                              ct,
                              pageSize,
                              cursor,
                              MIME_TYPE_VIDEO);

    GTEST_LOG_(INFO) << "listAllNodesByPage (LABEL_ASC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 41. LABEL_DESC, first page ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_LabelDesc_FirstPage)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::LABEL_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 std::nullopt,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::LABEL_DESC,
                              nodes,
                              ct,
                              pageSize,
                              std::nullopt,
                              MIME_TYPE_VIDEO);

    EXPECT_EQ(nodes.size(), pageSize);
    GTEST_LOG_(INFO) << "listAllNodesByPage (LABEL_DESC, VIDEO, first page, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ─── 42. LABEL_DESC, mid cursor ─────────────────────────────────────────────
TEST_F(SqliteNodesPerfListAllTest, ListAllByPage_Video_LabelDesc_MidCursor)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const size_t pageSize = 50;
    const auto cursor = videoLeafCursor(OrderByClause::LABEL_DESC);

    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(OrderByClause::LABEL_DESC,
                                                                 nodes,
                                                                 ct,
                                                                 pageSize,
                                                                 cursor,
                                                                 MIME_TYPE_VIDEO);
                                   });

    std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
    CancelToken ct;
    table->listAllNodesByPage(OrderByClause::LABEL_DESC,
                              nodes,
                              ct,
                              pageSize,
                              cursor,
                              MIME_TYPE_VIDEO);

    GTEST_LOG_(INFO) << "listAllNodesByPage (LABEL_DESC, VIDEO, mid cursor, limit " << pageSize
                     << "): " << COMPLEX_ITERS << " iters, total " << us << " us, avg "
                     << us / COMPLEX_ITERS << " us/iter, " << nodes.size() << " results";
}

// ═══════════════════════════════════════════════════════════════════════════════
//  searchNodesByPageWithSnapshot — ALL_VISUAL_MEDIA snapshot cache benchmarks
//
//  These tests compare the first-call (cold DB fetch) latency with subsequent
//  cache-hit latency, and measure offset-based pagination throughput once the
//  snapshot is warm.
// ═══════════════════════════════════════════════════════════════════════════════

// ─── N+1. Cold fetch vs cached read ──────────────────────────────────────────
// Measures the average latency of:
//   (a) a cold call that goes to SQLite (snapshot miss)
//   (b) a warm call served entirely from the in-memory snapshot
// The test asserts that the cached call is strictly faster and also reports
// the memory cost of building and holding the snapshot.
TEST_F(SqliteNodesPerfTest, PerfSearchByPageSnapshot_AllVisualMedia_FirstCallVsCachedCall)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    CancelToken ct;

    // ── Baseline memory before any snapshot exists ───────────────────────────
    const auto memBefore = MemSnapshot::take();

    // ── Warmup + correctness ─────────────────────────────────────────────────
    {
        std::string key;
        std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
        ASSERT_TRUE(table->searchNodesByPageWithSnapshot(filter,
                                                         OrderByClause::DEFAULT_ASC,
                                                         nodes,
                                                         ct,
                                                         0,
                                                         0,
                                                         key));
        ASSERT_GT(nodes.size(), 0u);
        ASSERT_FALSE(key.empty());
    }

    // ── Memory after snapshot is built ───────────────────────────────────────
    const auto memAfterSnapshot = MemSnapshot::take();
    const auto snapshotDelta = memAfterSnapshot - memBefore;

    GTEST_LOG_(INFO) << "Memory after ALL_VISUAL_MEDIA snapshot build"
                     << " — RSS delta: " << snapshotDelta.rssKb << " kB"
                     << "  |  heap delta: " << snapshotDelta.heapBytes / 1024 << " kB" << "  ("
                     << snapshotDelta.heapBytes << " B)";

    // ── Cold path: force a cache miss each iteration by alternating sort order
    //    so buildSnapshotKey() produces a different key each time.
    const long long coldUs =
        measureUs(COMPLEX_ITERS,
                  [&]
                  {
                      std::string freshKey;
                      std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                      table->searchNodesByPageWithSnapshot(filter,
                                                           OrderByClause::DEFAULT_ASC,
                                                           nodes,
                                                           ct,
                                                           0,
                                                           50,
                                                           freshKey);
                      // Evict snapshot by requesting a different order.
                      std::string freshKey2;
                      std::vector<std::pair<NodeHandle, NodeSerialized>> nodes2;
                      table->searchNodesByPageWithSnapshot(filter,
                                                           OrderByClause::DEFAULT_DESC,
                                                           nodes2,
                                                           ct,
                                                           0,
                                                           50,
                                                           freshKey2);
                  });
    const long long coldAvg = coldUs / (COMPLEX_ITERS * 2);

    // ── Warm path: populate snapshot once, then read from cache repeatedly. ───
    std::string cachedKey;
    {
        std::vector<std::pair<NodeHandle, NodeSerialized>> all;
        ASSERT_TRUE(table->searchNodesByPageWithSnapshot(filter,
                                                         OrderByClause::DEFAULT_ASC,
                                                         all,
                                                         ct,
                                                         0,
                                                         0,
                                                         cachedKey));
    }
    ASSERT_FALSE(cachedKey.empty());

    // Memory while snapshot is active.
    const auto memWhileWarm = MemSnapshot::take();
    const auto warmDelta = memWhileWarm - memBefore;

    GTEST_LOG_(INFO) << "Memory while ALL_VISUAL_MEDIA snapshot is warm (cache-hit reads)"
                     << " — RSS delta: " << warmDelta.rssKb << " kB"
                     << "  |  heap delta: " << warmDelta.heapBytes / 1024 << " kB";

    const long long warmUs =
        measureUs(COMPLEX_ITERS,
                  [&]
                  {
                      std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                      table->searchNodesByPageWithSnapshot(filter,
                                                           OrderByClause::DEFAULT_ASC,
                                                           nodes,
                                                           ct,
                                                           0,
                                                           50,
                                                           cachedKey);
                  });
    const long long warmAvg = warmUs / COMPLEX_ITERS;

    GTEST_LOG_(INFO) << "searchNodesByPageWithSnapshot (ALL_VISUAL_MEDIA) — cold (DB fetch) avg: "
                     << coldAvg << " us/call" << "  |  warm (cache hit) avg: " << warmAvg
                     << " us/call"
                     << "  |  speedup: " << (coldAvg > 0 ? coldAvg / std::max(warmAvg, 1LL) : 0LL)
                     << "x";

    EXPECT_LT(warmAvg, coldAvg) << "cache-hit path must be faster than cold DB fetch";
}

// ─── N+2. Offset-based pagination throughput ─────────────────────────────────
// Populates the ALL_VISUAL_MEDIA snapshot once then measures how long it takes
// to walk through all matching nodes using 50-node pages served from cache.
// Memory is tracked at three points:
//   1. Before the cold call (baseline)
//   2. After the snapshot is built (snapshot overhead)
//   3. While pages are being read (page-slice overhead)
TEST_F(SqliteNodesPerfTest, PerfSearchByPageSnapshot_AllVisualMedia_PaginationThroughput)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    NodeSearchFilter filter;
    filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
    filter.byNodeType(FILENODE);
    filter.byCategory(MIME_TYPE_ALL_VISUAL_MEDIA);

    CancelToken ct;

    // ── Baseline memory before snapshot creation ─────────────────────────────
    const auto memBaseline = MemSnapshot::take();

    // ── Cold call to populate snapshot and discover total count ──────────────
    std::string cacheKey;
    size_t totalNodes = 0;
    {
        std::vector<std::pair<NodeHandle, NodeSerialized>> all;
        ASSERT_TRUE(table->searchNodesByPageWithSnapshot(filter,
                                                         OrderByClause::DEFAULT_ASC,
                                                         all,
                                                         ct,
                                                         0,
                                                         0,
                                                         cacheKey));
        totalNodes = all.size();
        ASSERT_GT(totalNodes, 0u);
    }

    // ── Memory after full snapshot is held in RAM ─────────────────────────────
    const auto memAfterCold = MemSnapshot::take();
    const auto snapshotDelta = memAfterCold - memBaseline;

    GTEST_LOG_(INFO) << "ALL_VISUAL_MEDIA snapshot memory overhead (" << totalNodes << " nodes)"
                     << " — RSS delta: " << snapshotDelta.rssKb << " kB"
                     << "  |  heap delta: " << snapshotDelta.heapBytes / 1024 << " kB" << "  ("
                     << snapshotDelta.heapBytes << " B)" << "  |  bytes/node: "
                     << (totalNodes > 0 ? snapshotDelta.heapBytes / totalNodes : 0u);

    constexpr size_t pageSize = 50;
    const size_t expectedPages = (totalNodes + pageSize - 1) / pageSize;

    constexpr int FULLTHRU_ITERS = 10;

    size_t pagesRead = 0;
    size_t nodesVisited = 0;

    // ── Peak memory while iterating pages ────────────────────────────────────
    // Captured on the first page of the first iteration to show the transient
    // page-slice allocation on top of the snapshot.
    MemSnapshot memDuringIteration{};
    bool memDuringCaptured = false;

    const long long us =
        measureUs(FULLTHRU_ITERS,
                  [&]
                  {
                      pagesRead = 0;
                      nodesVisited = 0;
                      size_t offset = 0;

                      while (true)
                      {
                          std::vector<std::pair<NodeHandle, NodeSerialized>> page;
                          table->searchNodesByPageWithSnapshot(filter,
                                                               OrderByClause::DEFAULT_ASC,
                                                               page,
                                                               ct,
                                                               offset,
                                                               pageSize,
                                                               cacheKey);
                          if (page.empty())
                              break;

                          // Capture memory on the first page of the first iteration.
                          if (!memDuringCaptured)
                          {
                              memDuringIteration = MemSnapshot::take();
                              memDuringCaptured = true;
                          }

                          nodesVisited += page.size();
                          offset += page.size();
                          ++pagesRead;
                      }
                  });

    EXPECT_EQ(nodesVisited, totalNodes);
    EXPECT_EQ(pagesRead, expectedPages);

    if (memDuringCaptured)
    {
        const auto iterDelta = memDuringIteration - memBaseline;
        GTEST_LOG_(INFO) << "Memory while reading an ALL_VISUAL_MEDIA page slice"
                         << " — RSS delta: " << iterDelta.rssKb << " kB"
                         << "  |  heap delta: " << iterDelta.heapBytes / 1024 << " kB";
    }

    const long long avgUs = us / FULLTHRU_ITERS;
    const long long usPerPage = (pagesRead > 0) ? avgUs / static_cast<long long>(pagesRead) : 0LL;

    GTEST_LOG_(INFO) << "searchNodesByPageWithSnapshot ALL_VISUAL_MEDIA pagination throughput"
                     << " (" << totalNodes << " nodes, page=" << pageSize << ", " << pagesRead
                     << " pages): " << FULLTHRU_ITERS << " iters, total " << us << " us, avg "
                     << avgUs << " us/iter, " << usPerPage << " us/page";
}

} // anonymous namespace

#endif // USE_SQLITE
