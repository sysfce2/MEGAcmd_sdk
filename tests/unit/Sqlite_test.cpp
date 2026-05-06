/**
 * @file Sqlite_test.cpp
 * @brief Unit tests for Sqlite-backed node storage, including
 *        listAllNodesByPage() cursor-based (keyset) pagination.
 */

#include <gtest/gtest.h>
#include <mega/db/sqlite.h>
#include <mega/localpath.h>

#include <filesystem>
#include <mega.h>
#include <stdfs.h>
#include <string>

using namespace mega;

/**
 * @brief Validate renameDBFiles method
 *
 * Steps:
 *  - Create new data base
 *  - Call to renameDBFiles
 *  - Check if all files has been renamed
 */
#ifdef WIN32
// As DB is opened when files are renamed, windows doesn't allow rename DB files
TEST(Sqlite, DISABLED_renameDB)
{
#else
TEST(Sqlite, renameDB)
{
#endif
    auto pathString{std::filesystem::current_path() / "folder"};

    const MrProper cleanUp(
        [pathString]()
        {
            std::filesystem::remove_all(pathString);
        });

    std::filesystem::create_directory(pathString);
    LocalPath folderPath = LocalPath::fromAbsolutePath(path_u8string(pathString));
    SqliteDbAccess dbAccess{folderPath};

    // Create and open DB
    std::unique_ptr<FileSystemAccess> fsaccess{new FSACCESS_CLASS};
    const std::string dbName{"dbName"};
    LocalPath currentDataBasePath{dbAccess.databasePath(*fsaccess, dbName, DbAccess::DB_VERSION)};
    PrnGen rng;
    constexpr int flags = 0;
    std::unique_ptr<SqliteDbTable> db{dbAccess.open(rng, *fsaccess, dbName, flags, nullptr)};
    if (!db)
    {
        ASSERT_TRUE(false) << "Failure opening DB";
    }

    // Insert elements
    for (int i = 1; i < 10; ++i)
    {
        std::string content = "content " + std::to_string(i);
        db->put(static_cast<uint32_t>((i += DbTable::IDSPACING) | MegaClient::CACHEDUSER),
                static_cast<char*>(content.data()),
                static_cast<unsigned>(content.length()));
    }

    // check if auxiliar files exist
    LocalPath shmPath = currentDataBasePath;
    shmPath.append(LocalPath::fromRelativePath("-shm"));
    bool shmExists = std::filesystem::exists(shmPath.toPath(false));
    EXPECT_TRUE(shmExists) << "Unexpected behavior, -shm file doesn't exist";

    LocalPath walPath = currentDataBasePath;
    walPath.append(LocalPath::fromRelativePath("-wal"));
    bool walExists = std::filesystem::exists(walPath.toPath(false));
    EXPECT_TRUE(walExists) << "Unexpected behavior, -wal file doesn't exist";

    // Determine new path
    const std::string dbNewName{"dbNewName"};
    LocalPath newDataBasePath{dbAccess.databasePath(*fsaccess, dbNewName, DbAccess::DB_VERSION)};

    // Rename DB
    EXPECT_TRUE(dbAccess.renameDBFiles(*fsaccess, currentDataBasePath, newDataBasePath))
        << "Failure to rename files (maybe they are in use)";

    // Verify if auxiliar files exist
    if (shmExists)
    {
        shmPath = newDataBasePath;
        shmPath.append(LocalPath::fromRelativePath("-shm"));
        std::string aux = shmPath.toPath(false);
        EXPECT_TRUE(std::filesystem::exists(aux))
            << "File " << aux << "doesn't exit when it should";
    }

    if (walExists)
    {
        walPath = newDataBasePath;
        walPath.append(LocalPath::fromRelativePath("-wal"));
        std::string aux = walPath.toPath(false);
        EXPECT_TRUE(std::filesystem::exists(aux))
            << "File " << aux << "doesn't exit when it should";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SQLite-backed node storage tests (listAllNodesByPage cursor-based pagination)
// ─────────────────────────────────────────────────────────────────────────────

#ifdef USE_SQLITE

#include "utils.h"

#include <mega/megaapp.h>
#include <mega/megaclient.h>
#include <mega/nodemanager.h>

namespace fs = std::filesystem;

namespace
{

// ─── Per-node metadata captured at insertion time ────────────────────────────
// Used to build NodeSearchCursorOffset without relying on attribute decryption.
struct NodeMeta
{
    std::string name;
    nodetype_t type = FILENODE;
    int64_t size = 0;
    int64_t mtime = 0;
    int label = 0; ///< 0 = unlabelled, 1-7 = colour
    int fav = 0; ///< 0 or 1
};

// Attribute name IDs used across multiple test fixtures and test cases.
const nameid kNameId = AttrMap::string2nameid("n");
const nameid kFavId = AttrMap::string2nameid("fav");
const nameid kLabelId = AttrMap::string2nameid("lbl");

// Sort order + page size pair used by parameterised pagination tests.
struct OrderAndPageSize
{
    int order;
    size_t pageSize;
};

// ─── Shared test fixture ───────────────────────────────────────────────────────
/**
 * Base fixture used by ListAllNodesByPageTest and GroupedListAllNodesByPageTest.
 *
 * Dataset (created in SetUp):
 *   ROOT folder
 *   5 sub-folders   "Folder_A" … "Folder_E"
 *   20 file nodes   "file_01.txt" … "file_20.txt"
 *     size  = i * 100  (i = 1 … 20)
 *     mtime = 1'700'000'000 + i
 *     label = i % 4      (0 = unlabelled, 1 / 2 / 3 cycling)
 *     fav   = (i % 5 == 0) ? 1 : 0   (files 5, 10, 15, 20)
 */
class SearchByPageTest: public ::testing::Test
{
protected:
    mega::MegaApp mApp;
    NodeManager::MissingParentNodes mMissingParentNodes;
    std::shared_ptr<MegaClient> mClient;
    fs::path mTestDir;

    uint64_t mNextHandle = 1;
    NodeHandle mRootHandle;
    std::map<handle, NodeMeta> mMeta; ///< Metadata for all inserted nodes

    static constexpr int NUM_FILES = 20;
    static constexpr int NUM_FOLDERS = 5;

    void SetUp() override
    {
        mTestDir = fs::current_path() / "search_by_page_test";
        // Remove any leftover directory from a previous crashed run to avoid
        // SQLite "database is locked" errors caused by stale WAL files.
        fs::remove_all(mTestDir);
        fs::create_directories(mTestDir);

        auto* dbAccess = new SqliteDbAccess(LocalPath::fromAbsolutePath(path_u8string(mTestDir)));

        mClient = mt::makeClient(mApp, dbAccess);
        mClient->sid =
            "AWA5YAbtb4JO-y2zWxmKZpSe5-6XM7CTEkA-3Nv7J4byQUpOazdfSC1ZUFlS-kah76gPKUEkTF9g7MeE";
        mClient->opensctable();

        populateDB();

        if (auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get()))
            sa->createIndexes(/*enableSearch=*/true, /*enableLexi=*/true);
    }

    void TearDown() override
    {
        mClient.reset();
        fs::remove_all(mTestDir);
    }

    // ── Low-level node factory ────────────────────────────────────────────────
    std::shared_ptr<Node> addNode(nodetype_t type,
                                  std::shared_ptr<Node> parent,
                                  const NodeMeta& meta)
    {
        NodeHandle h = NodeHandle().set6byte(mNextHandle++);
        Node& ref = mt::makeNode(*mClient, type, h, parent.get());
        auto node = std::shared_ptr<Node>(&ref);

        node->attrs.map[kNameId] = meta.name;

        if (type == FILENODE)
        {
            node->size = static_cast<m_off_t>(meta.size);
            node->mtime = static_cast<m_time_t>(meta.mtime);
            node->ctime = node->mtime;
            node->crc[0] = static_cast<int32_t>(mNextHandle);
            node->isvalid = true;
            node->serializefingerprint(&node->attrs.map['c']);
            node->setfingerprint();

            // sizeVirtual in the DB is derived from NodeCounter.storage.
            // Without this, ORDER BY sizeVirtual returns 0 for all files and
            // cursor-based pagination for SIZE sorts would not advance.
            NodeCounter nc;
            nc.files = 1;
            nc.storage = meta.size;
            node->setCounter(nc);
        }

        if (meta.fav)
            node->attrs.map[kFavId] = "1";

        if (meta.label > 0)
            node->attrs.map[kLabelId] = std::to_string(meta.label);

        mClient->mNodeManager.addNode(node,
                                      /*notify=*/false,
                                      /*isFetching=*/true,
                                      mMissingParentNodes);
        mClient->mNodeManager.saveNodeInDb(node.get());

        auto& stored = mMeta[h.as8byte()] = meta;
        stored.type = type;
        return node;
    }

    // ── Dataset construction ──────────────────────────────────────────────────
    virtual void populateDB()
    {
        NodeMeta rootMeta{"ROOT", ROOTNODE, 0, 0, 0, 0};
        auto root = addNode(ROOTNODE, nullptr, rootMeta);
        mRootHandle = root->nodeHandle();

        const std::string alpha = "ABCDE";
        for (int i = 0; i < NUM_FOLDERS; ++i)
        {
            NodeMeta fm;
            fm.name = "Folder_" + std::string(1, alpha[static_cast<size_t>(i)]);
            fm.type = FOLDERNODE;
            addNode(FOLDERNODE, root, fm);
        }

        // File nodes
        for (int i = 1; i <= NUM_FILES; ++i)
        {
            NodeMeta fm;
            // Zero-padded name so lexicographic order == numeric order
            const std::string pad = (i < 10 ? "0" : "");
            fm.name = "file_" + pad + std::to_string(i) + ".txt";
            fm.size = static_cast<int64_t>(i) * 100;
            fm.mtime = 1'700'000'000LL + i;
            fm.label = i % 4; // 0 = unlabelled, 1/2/3 cycling
            fm.fav = (i % 5 == 0) ? 1 : 0;
            addNode(FILENODE, root, fm);
        }
    }

    // ── Multi-page accumulation helper ─────────────────────────────────────────
    // Accumulates all pages until empty, starting from `startCursor` (default:
    // first page). Bounded to mMeta.size()+2 iterations so that a stuck cursor
    // produces a test failure rather than an infinite hang.
    std::vector<NodeHandle>
        collectAllByPage(int order,
                         size_t pageSize,
                         MimeType_t mimeType,
                         std::optional<NodeSearchCursorOffset> startCursor = std::nullopt) const
    {
        std::vector<NodeHandle> result;
        std::optional<NodeSearchCursorOffset> cursor = startCursor;

        const size_t maxPages = mMeta.size() + 2;
        size_t pageCount = 0;

        while (pageCount < maxPages)
        {
            ++pageCount;
            auto page = mClient->mNodeManager.listAllNodesByPage(mimeType,
                                                                 order,
                                                                 CancelToken{},
                                                                 pageSize,
                                                                 cursor);
            if (page.empty())
                break;
            for (const auto& n: page)
                result.push_back(n->nodeHandle());
            cursor = cursorFor(page.back()->nodeHandle(), order);
        }

        if (pageCount >= maxPages)
        {
            ADD_FAILURE() << "collectAllByPage: exceeded " << maxPages
                          << " pages for order=" << order
                          << " – cursor likely not advancing (possible infinite loop)";
        }

        return result;
    }

    // ── Build a cursor anchored at node h for the given sort order ────────────
    NodeSearchCursorOffset cursorFor(NodeHandle h, int order) const
    {
        const NodeMeta& m = mMeta.at(h.as8byte());

        NodeSearchCursorOffset c;
        c.mLastName = m.name;
        c.mLastHandle = h.as8byte();

        switch (order)
        {
            case OrderByClause::SIZE_ASC:
            case OrderByClause::SIZE_DESC:
                c.mLastSize = m.size;
                break;
            case OrderByClause::MTIME_ASC:
            case OrderByClause::MTIME_DESC:
                c.mLastMtime = m.mtime;
                break;
            case OrderByClause::LABEL_ASC:
            case OrderByClause::LABEL_DESC:
                c.mLastLabel = m.label;
                break;
            case OrderByClause::FAV_ASC:
            case OrderByClause::FAV_DESC:
                c.mLastFav = m.fav;
                break;
            default:
                break;
        }
        return c;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  listAllNodesByPage – cursor-based global pagination tests
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Fixture for listAllNodesByPage tests.
 *
 * Inherits the dataset from SearchByPageTest (ROOT + 5 folders + 20 files)
 * and the cursorFor() helper so cursor fields are always correctly populated.
 * Adds two helpers:
 *   referenceAll()    – single-call with no limit (ground truth for order / count)
 *   collectAllByPage() – multi-page accumulation (verifies no skips / duplicates)
 */
class ListAllNodesByPageTest: public SearchByPageTest
{
protected:
    // All test files are named *.txt → MIME_TYPE_DOCUMENT.
    static constexpr MimeType_t TEST_MIME = MIME_TYPE_DOCUMENT;

    // Returns every node matching TEST_MIME in `order` in one call (no limit, no cursor).
    std::vector<NodeHandle> referenceAll(int order) const
    {
        auto nodes = mClient->mNodeManager.listAllNodesByPage(TEST_MIME,
                                                              order,
                                                              CancelToken{},
                                                              /*maxElements=*/0,
                                                              std::nullopt);

        std::vector<NodeHandle> result;
        result.reserve(nodes.size());
        for (const auto& n: nodes)
            result.push_back(n->nodeHandle());
        return result;
    }

    // Convenience overload using the fixture's TEST_MIME.
    // `startCursor` defaults to nullopt (first page); pass a cursor to resume mid-sequence.
    std::vector<NodeHandle>
        collectAllByPage(int order,
                         size_t pageSize,
                         std::optional<NodeSearchCursorOffset> startCursor = std::nullopt) const
    {
        return SearchByPageTest::collectAllByPage(order, pageSize, TEST_MIME, startCursor);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Helper: assert handles collected by listAllNodesByPage == reference
// ─────────────────────────────────────────────────────────────────────────────

void assertListAllMatchesReference(const std::vector<NodeHandle>& paged,
                                   const std::vector<NodeHandle>& reference,
                                   const std::string& label)
{
    ASSERT_EQ(paged.size(), reference.size()) << label << ": total count mismatch";
    for (size_t i = 0; i < reference.size(); ++i)
    {
        EXPECT_EQ(paged[i], reference[i]) << label << ": handle mismatch at index " << i;
    }
    const std::set<NodeHandle> unique(paged.begin(), paged.end());
    EXPECT_EQ(unique.size(), paged.size()) << label << ": duplicate handles found";
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group A – Input validation (error / guard paths)
// ═══════════════════════════════════════════════════════════════════════════

// A1. MIME_TYPE_UNKNOWN is rejected – returns empty result without crash.
//     The implementation guards against MIME_TYPE_UNKNOWN at the SQLite layer
//     and returns false; the NodeManager must surface an empty vector.
TEST_F(ListAllNodesByPageTest, UnknownMimeType_ReturnsEmpty)
{
    auto nodes = mClient->mNodeManager.listAllNodesByPage(MIME_TYPE_UNKNOWN,
                                                          OrderByClause::DEFAULT_ASC,
                                                          CancelToken{},
                                                          10,
                                                          std::nullopt);

    EXPECT_TRUE(nodes.empty()) << "MIME_TYPE_UNKNOWN must return an empty result";
}

// A2. Valid MIME type with no matching nodes → empty result (no crash).
//     Exercises the zero-row result path for a valid, non-UNKNOWN type.
TEST_F(ListAllNodesByPageTest, ValidMimeTypeNoMatches_ReturnsEmpty)
{
    // Dataset contains only .txt files; MIME_TYPE_AUDIO has no matches.
    auto nodes = mClient->mNodeManager.listAllNodesByPage(MIME_TYPE_AUDIO,
                                                          OrderByClause::DEFAULT_ASC,
                                                          CancelToken{},
                                                          10,
                                                          std::nullopt);

    EXPECT_TRUE(nodes.empty()) << "MIME_TYPE_AUDIO with no matching nodes must return empty";
}

// A3. Cursor/order mismatch → returns empty, no crash.
//
//     The implementation validates that the cursor contains the optional field
//     required by the chosen sort order before binding any SQL parameters.
//     When the required field is absent (cursor was built for a different order),
//     bindCursorParamsForListAll() returns false and listAllNodesByPage() returns
//     an empty vector without executing the query.
//
//     Mismatch matrix (cursor built for row, query order is column):
//       DEFAULT cursor → SIZE / MTIME / FAV / LABEL orders  (4 cases)
//       SIZE    cursor → MTIME / FAV / LABEL orders          (3 cases)
//       MTIME   cursor → SIZE / FAV / LABEL orders           (3 cases)
//       FAV     cursor → SIZE / MTIME / LABEL orders         (3 cases)
//       LABEL   cursor → SIZE / MTIME / FAV orders           (3 cases)
//     Total: 16 mismatch cases.
TEST_F(ListAllNodesByPageTest, CursorOrderMismatch_ReturnsEmpty)
{
    // Pick a stable mid-dataset handle (valid for all mMeta lookups).
    const auto reference = referenceAll(OrderByClause::DEFAULT_ASC);
    ASSERT_GE(reference.size(), 2u);
    const NodeHandle midHandle = reference[reference.size() / 2];

    struct MismatchCase
    {
        int cursorOrder; ///< order used to build the cursor (sets its optional fields)
        int queryOrder; ///< order passed to listAllNodesByPage (needs a different field)
        const char* label;
    };

    // clang-format off
    const std::vector<MismatchCase> cases = {
        // DEFAULT cursor has no optional fields → fails for any order that needs one
        {OrderByClause::DEFAULT_ASC,  OrderByClause::SIZE_ASC,   "DEFAULT→SIZE"},
        {OrderByClause::DEFAULT_ASC,  OrderByClause::MTIME_ASC,  "DEFAULT→MTIME"},
        {OrderByClause::DEFAULT_ASC,  OrderByClause::FAV_ASC,    "DEFAULT→FAV"},
        {OrderByClause::DEFAULT_ASC,  OrderByClause::LABEL_ASC,  "DEFAULT→LABEL"},
        // SIZE cursor has mLastSize but not mLastMtime / mLastFav / mLastLabel
        {OrderByClause::SIZE_ASC,     OrderByClause::MTIME_ASC,  "SIZE→MTIME"},
        {OrderByClause::SIZE_ASC,     OrderByClause::FAV_ASC,    "SIZE→FAV"},
        {OrderByClause::SIZE_ASC,     OrderByClause::LABEL_ASC,  "SIZE→LABEL"},
        // MTIME cursor has mLastMtime but not mLastSize / mLastFav / mLastLabel
        {OrderByClause::MTIME_ASC,    OrderByClause::SIZE_ASC,   "MTIME→SIZE"},
        {OrderByClause::MTIME_ASC,    OrderByClause::FAV_ASC,    "MTIME→FAV"},
        {OrderByClause::MTIME_ASC,    OrderByClause::LABEL_ASC,  "MTIME→LABEL"},
        // FAV cursor has mLastFav but not mLastSize / mLastMtime / mLastLabel
        {OrderByClause::FAV_ASC,      OrderByClause::SIZE_ASC,   "FAV→SIZE"},
        {OrderByClause::FAV_ASC,      OrderByClause::MTIME_ASC,  "FAV→MTIME"},
        {OrderByClause::FAV_ASC,      OrderByClause::LABEL_ASC,  "FAV→LABEL"},
        // LABEL cursor has mLastLabel but not mLastSize / mLastMtime / mLastFav
        {OrderByClause::LABEL_ASC,    OrderByClause::SIZE_ASC,   "LABEL→SIZE"},
        {OrderByClause::LABEL_ASC,    OrderByClause::MTIME_ASC,  "LABEL→MTIME"},
        {OrderByClause::LABEL_ASC,    OrderByClause::FAV_ASC,    "LABEL→FAV"},
    };
    // clang-format on

    for (const auto& c: cases)
    {
        auto cursor = cursorFor(midHandle, c.cursorOrder);
        auto result = mClient->mNodeManager.listAllNodesByPage(TEST_MIME,
                                                               c.queryOrder,
                                                               CancelToken{},
                                                               10,
                                                               cursor);
        EXPECT_TRUE(result.empty())
            << c.label << ": expected empty result for cursor/order mismatch";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group B – First-page basics (no cursor)
// ═══════════════════════════════════════════════════════════════════════════

// B1. MIME filter excludes non-file nodes – only .txt files are returned.
TEST_F(ListAllNodesByPageTest, NoCursor_DocumentMimeFilter_ReturnsAllFiles)
{
    auto nodes = mClient->mNodeManager.listAllNodesByPage(TEST_MIME,
                                                          OrderByClause::DEFAULT_ASC,
                                                          CancelToken{},
                                                          /*maxElements=*/0,
                                                          std::nullopt);

    // Dataset has NUM_FILES .txt files; folders/root have no MIME type so are excluded.
    EXPECT_EQ(nodes.size(), static_cast<size_t>(NUM_FILES));

    const std::set<NodeHandle> unique(
        [&]
        {
            std::vector<NodeHandle> hs;
            for (const auto& n: nodes)
                hs.push_back(n->nodeHandle());
            return std::set<NodeHandle>(hs.begin(), hs.end());
        }());
    EXPECT_EQ(unique.size(), nodes.size()) << "duplicate handles in result";
}

// B2. pageSize larger than total – all results fit in one page.
TEST_F(ListAllNodesByPageTest, PageSizeLargerThanTotal_AllInOnePage)
{
    auto page = mClient->mNodeManager.listAllNodesByPage(TEST_MIME,
                                                         OrderByClause::DEFAULT_ASC,
                                                         CancelToken{},
                                                         1000,
                                                         std::nullopt);

    EXPECT_EQ(page.size(), static_cast<size_t>(NUM_FILES));
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group C – Sort order correctness
// ═══════════════════════════════════════════════════════════════════════════

// C1. FAV_DESC – non-fav nodes precede fav nodes in the result.
TEST_F(ListAllNodesByPageTest, FavDesc_AllTypes_NonFavPrecedesFav)
{
    // Dataset: fav=1 for files where i%5==0 (file_05, file_10, file_15, file_20).
    // FAV_DESC (fav ASC) → fav=0 nodes first, then fav=1 nodes.

    const auto reference = referenceAll(OrderByClause::FAV_DESC);
    ASSERT_EQ(reference.size(), static_cast<size_t>(NUM_FILES));

    bool seenFav = false;
    for (const auto& h: reference)
    {
        auto node = mClient->mNodeManager.getNodeByHandle(h);
        ASSERT_NE(node, nullptr);
        const bool isFav = node->attrs.map.count(kFavId) && node->attrs.map.at(kFavId) == "1";
        if (isFav)
            seenFav = true;
        if (seenFav)
        {
            EXPECT_TRUE(isFav) << "non-fav node after fav node in FAV_DESC result: "
                               << node->displayname();
        }
    }
}

// C2. LABEL_DESC – label values are non-increasing across the result.
TEST_F(ListAllNodesByPageTest, LabelDesc_AllTypes_LabelsAreNonIncreasing)
{
    // Dataset: label = i%4 cycling (0,1,2,3).
    // LABEL_DESC: ORDER BY label DESC → label 3 group first, then 2, 1, 0.

    const auto reference = referenceAll(OrderByClause::LABEL_DESC);
    ASSERT_EQ(reference.size(), static_cast<size_t>(NUM_FILES));

    int prevLabel = INT_MAX;
    for (const auto& h: reference)
    {
        auto node = mClient->mNodeManager.getNodeByHandle(h);
        ASSERT_NE(node, nullptr);
        const auto it = node->attrs.map.find(kLabelId);
        const int lbl = (it != node->attrs.map.end()) ? std::stoi(it->second) : 0;
        // Tie-breaking within a label group is name ASC then nodehandle ASC.
        // This check only verifies that the label never increases across the result.
        EXPECT_LE(lbl, prevLabel) << "label increased at node " << node->displayname();
        prevLabel = lbl;
    }
}

// C3. DEFAULT_DESC – name is non-increasing across the paged result.
//     Validates buildOrderByForListAll/buildCursorWhereForListAll for DEFAULT_DESC
//     without relying on referenceAll() as the oracle.
//     Dataset names are zero-padded so natural-case and lexicographic order agree.
TEST_F(ListAllNodesByPageTest, DefaultDesc_AllTypes_DefaultOrderIsNonIncreasing)
{
    const auto paged = collectAllByPage(OrderByClause::DEFAULT_DESC, 7);
    ASSERT_EQ(paged.size(), static_cast<size_t>(NUM_FILES));

    // DEFAULT_DESC: ORDER BY name DESC, nodehandle DESC.
    // All names in our dataset are unique → the sequence must be strictly decreasing.
    std::string prevName;
    for (const auto& h: paged)
    {
        const std::string& name = mMeta.at(h.as8byte()).name;
        if (!prevName.empty())
        {
            EXPECT_GE(prevName, name)
                << "name increased in DEFAULT_DESC result: prev=" << prevName << " curr=" << name;
        }
        prevName = name;
    }
}

// C4. SIZE_ASC – sizes are non-decreasing across the paged result.
//     Constrains buildOrderByForListAll for SIZE_ASC independently of referenceAll().
TEST_F(ListAllNodesByPageTest, SizeAsc_AllTypes_SizesAreNonDecreasing)
{
    const auto paged = collectAllByPage(OrderByClause::SIZE_ASC, 5);
    ASSERT_EQ(paged.size(), static_cast<size_t>(NUM_FILES));

    // Dataset: size = i*100 (i=1..20), all unique → strictly increasing.
    int64_t prevSize = INT64_MIN;
    for (const auto& h: paged)
    {
        const int64_t size = mMeta.at(h.as8byte()).size;
        EXPECT_LE(prevSize, size) << "size not non-decreasing in SIZE_ASC result";
        prevSize = size;
    }
}

// C5. MTIME_DESC – mtimes are non-increasing across the paged result.
//     Constrains buildOrderByForListAll for MTIME_DESC independently of referenceAll().
TEST_F(ListAllNodesByPageTest, MtimeDesc_AllTypes_MtimeIsNonIncreasing)
{
    const auto paged = collectAllByPage(OrderByClause::MTIME_DESC, 8);
    ASSERT_EQ(paged.size(), static_cast<size_t>(NUM_FILES));

    // Dataset: mtime = 1_700_000_000 + i (i=1..20), all unique → strictly decreasing.
    int64_t prevMtime = INT64_MAX;
    for (const auto& h: paged)
    {
        const int64_t mtime = mMeta.at(h.as8byte()).mtime;
        EXPECT_GE(prevMtime, mtime) << "mtime not non-increasing in MTIME_DESC result";
        prevMtime = mtime;
    }
}

// C6. LABEL_ASC – labelled nodes (label > 0) appear before unlabelled (label = 0),
//     and within the labelled section the label value is non-decreasing.
//     Tests the isZero ASC, label ASC primary keys without using referenceAll().
TEST_F(ListAllNodesByPageTest, LabelAsc_AllTypes_LabeledBeforeUnlabeledAndNonDecreasing)
{
    // LABEL_ASC: ORDER BY (CASE WHEN label=0 THEN 1 ELSE 0 END) ASC, label ASC, name ASC, …
    // labelled (isZero=0) block comes first in non-decreasing label order,
    // then unlabelled (isZero=1, label=0) block.
    const auto paged = collectAllByPage(OrderByClause::LABEL_ASC, 6);
    ASSERT_EQ(paged.size(), static_cast<size_t>(NUM_FILES));

    bool inUnlabeledSection = false;
    int prevNonZeroLabel = 0;

    for (const auto& h: paged)
    {
        auto node = mClient->mNodeManager.getNodeByHandle(h);
        ASSERT_NE(node, nullptr);
        const auto it = node->attrs.map.find(kLabelId);
        const int lbl = (it != node->attrs.map.end()) ? std::stoi(it->second) : 0;

        if (lbl == 0)
        {
            inUnlabeledSection = true;
        }
        else
        {
            EXPECT_FALSE(inUnlabeledSection)
                << "labeled node appeared after unlabeled section: " << node->displayname();
            EXPECT_LE(prevNonZeroLabel, lbl)
                << "label decreased within labeled section: prev=" << prevNonZeroLabel
                << " curr=" << lbl << " at " << node->displayname();
            prevNonZeroLabel = lbl;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group D – Pagination boundary conditions
// ═══════════════════════════════════════════════════════════════════════════

// D1. pageSize equals total count → first page is full, second page is empty.
TEST_F(ListAllNodesByPageTest, PageSizeEqualsTotal_SinglePage)
{
    auto page1 = mClient->mNodeManager.listAllNodesByPage(TEST_MIME,
                                                          OrderByClause::DEFAULT_ASC,
                                                          CancelToken{},
                                                          /*maxElements=*/NUM_FILES,
                                                          std::nullopt);
    ASSERT_EQ(page1.size(), static_cast<size_t>(NUM_FILES));

    auto cursor = cursorFor(page1.back()->nodeHandle(), OrderByClause::DEFAULT_ASC);
    auto page2 = mClient->mNodeManager.listAllNodesByPage(TEST_MIME,
                                                          OrderByClause::DEFAULT_ASC,
                                                          CancelToken{},
                                                          NUM_FILES,
                                                          cursor);

    EXPECT_TRUE(page2.empty()) << "Page after last element must be empty";
}

// D2. pageSize=1 – advancing one node at a time produces no skips and no
//     duplicates; concatenated pages equal the single-call reference.
TEST_F(ListAllNodesByPageTest, PageSizeOne_AllTypes_NoSkipsNoDuplicates)
{
    const auto reference = referenceAll(OrderByClause::DEFAULT_ASC);
    const auto paged = collectAllByPage(OrderByClause::DEFAULT_ASC, 1);

    assertListAllMatchesReference(paged, reference, "PageSizeOne ALL DEFAULT_ASC");
}

// D3. Cursor past the last element → next page is empty.
TEST_F(ListAllNodesByPageTest, CursorPastLastElement_ReturnsEmpty)
{
    const auto reference = referenceAll(OrderByClause::DEFAULT_ASC);
    ASSERT_FALSE(reference.empty());

    auto cursor = cursorFor(reference.back(), OrderByClause::DEFAULT_ASC);
    auto page = mClient->mNodeManager.listAllNodesByPage(TEST_MIME,
                                                         OrderByClause::DEFAULT_ASC,
                                                         CancelToken{},
                                                         10,
                                                         cursor);

    EXPECT_TRUE(page.empty()) << "Page after last element must be empty";
}

// D4. Midpoint cursor – tail pages cover exactly reference[splitAt..end].
TEST_F(ListAllNodesByPageTest, MidpointCursor_TailMatchesReference)
{
    const auto reference = referenceAll(OrderByClause::DEFAULT_ASC);
    ASSERT_GE(reference.size(), 4u);

    const size_t splitAt = reference.size() / 2;
    const auto tail =
        collectAllByPage(OrderByClause::DEFAULT_ASC,
                         5,
                         cursorFor(reference[splitAt - 1], OrderByClause::DEFAULT_ASC));

    const std::vector<NodeHandle> expected(reference.begin() + static_cast<int>(splitAt),
                                           reference.end());
    ASSERT_EQ(tail.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        EXPECT_EQ(tail[i], expected[i]) << "tail index " << i;
}

// D5. DEFAULT_ASC with MIME filter – full paged result equals single-call
//     reference, and every returned node is a FILENODE.
TEST_F(ListAllNodesByPageTest, DefaultAsc_DocumentMime_PaginationMatchesReference)
{
    const auto reference = referenceAll(OrderByClause::DEFAULT_ASC);
    const auto paged = collectAllByPage(OrderByClause::DEFAULT_ASC, 8);

    assertListAllMatchesReference(paged, reference, "DEFAULT_ASC DOCUMENT");

    // With MIME_TYPE_DOCUMENT, only file nodes are returned (folders have no MIME type).
    for (const auto& h: paged)
    {
        auto node = mClient->mNodeManager.getNodeByHandle(h);
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(node->type, FILENODE) << "non-file node returned with DOCUMENT mime filter";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group E – Stability under concurrent modification
// ═══════════════════════════════════════════════════════════════════════════

// E1. Deletion between pages – cursor-based pagination must not skip any
//     remaining nodes when a node adjacent to the page boundary is deleted
//     before the next page is fetched.
//
//     With offset-based pagination, deleting the first item of page 2 would
//     shift all subsequent items up by one, causing the old first item of page 3
//     to be skipped entirely.  Cursor-based pagination is immune to this because
//     the cursor encodes a sort-key position, not an offset.
TEST_F(ListAllNodesByPageTest, Deletion_BetweenPages_NoSkip)
{
    // Get first page (5 nodes) and save the full reference ordering.
    auto page1 = mClient->mNodeManager.listAllNodesByPage(TEST_MIME,
                                                          OrderByClause::DEFAULT_ASC,
                                                          CancelToken{},
                                                          5,
                                                          std::nullopt);
    ASSERT_EQ(page1.size(), 5u);

    const auto fullReference = referenceAll(OrderByClause::DEFAULT_ASC);
    ASSERT_EQ(fullReference.size(), static_cast<size_t>(NUM_FILES));

    // Physically delete the 6th node from the DB (first item of page 2 in
    // offset terms) so subsequent cursor queries never see it.
    const NodeHandle toDelete = fullReference[5];
    auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get());
    ASSERT_NE(sa, nullptr);
    ASSERT_TRUE(sa->remove(toDelete)) << "failed to delete node from DB";

    // Collect all remaining pages using the cursor after page 1.
    const auto page2Plus =
        collectAllByPage(OrderByClause::DEFAULT_ASC,
                         5,
                         cursorFor(page1.back()->nodeHandle(), OrderByClause::DEFAULT_ASC));

    const std::set<NodeHandle> page2Set(page2Plus.begin(), page2Plus.end());

    // The deleted node must not appear.
    EXPECT_EQ(page2Set.count(toDelete), 0u) << "deleted node appeared in page 2+";

    // Every node after the deleted one in the reference ordering must appear –
    // this is the no-skip guarantee that cursor pagination provides.
    // fullReference[0..4] = page1, [5] = deleted, [6..NUM_FILES-1] = expected in page2+.
    for (size_t i = 6; i < fullReference.size(); ++i)
    {
        EXPECT_EQ(page2Set.count(fullReference[i]), 1u)
            << "node at reference index " << i << " was skipped after deletion";
    }

    // Sanity: no page-1 handle should reappear.
    for (const auto& n: page1)
    {
        EXPECT_EQ(page2Set.count(n->nodeHandle()), 0u)
            << "page-1 handle " << n->nodeHandle() << " repeated in page 2+";
    }
}

// E2. SizeAsc_DeletionBetweenPages_NoSkip – same guarantee as E1 but using
//     SIZE_ASC, which exercises a different cursor predicate path
//     (sizeVirtual/name/nodehandle triple) vs the DEFAULT name/handle pair.
//
//     DEFAULT_ASC passing does NOT imply SIZE_ASC is also correct because the
//     cursor WHERE clauses are generated by separate branches in
//     buildCursorWhereForListAll and bound by separate branches in
//     bindCursorParamsForListAll.
TEST_F(ListAllNodesByPageTest, SizeAsc_DeletionBetweenPages_NoSkip)
{
    auto page1 = mClient->mNodeManager.listAllNodesByPage(TEST_MIME,
                                                          OrderByClause::SIZE_ASC,
                                                          CancelToken{},
                                                          5,
                                                          std::nullopt);
    ASSERT_EQ(page1.size(), 5u);

    const auto fullReference = referenceAll(OrderByClause::SIZE_ASC);
    ASSERT_EQ(fullReference.size(), static_cast<size_t>(NUM_FILES));

    // Delete the 6th node (first of page 2 in offset terms).
    const NodeHandle toDelete = fullReference[5];
    auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get());
    ASSERT_NE(sa, nullptr);
    ASSERT_TRUE(sa->remove(toDelete)) << "failed to delete node from DB";

    const auto page2Plus =
        collectAllByPage(OrderByClause::SIZE_ASC,
                         5,
                         cursorFor(page1.back()->nodeHandle(), OrderByClause::SIZE_ASC));

    const std::set<NodeHandle> page2Set(page2Plus.begin(), page2Plus.end());

    EXPECT_EQ(page2Set.count(toDelete), 0u) << "deleted node appeared in page 2+";

    // Every node after the deleted one in the SIZE_ASC ordering must appear.
    for (size_t i = 6; i < fullReference.size(); ++i)
    {
        EXPECT_EQ(page2Set.count(fullReference[i]), 1u)
            << "node at SIZE_ASC reference index " << i << " was skipped after deletion";
    }

    for (const auto& n: page1)
    {
        EXPECT_EQ(page2Set.count(n->nodeHandle()), 0u) << "page-1 handle repeated in page 2+";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group F – Exhaustive sort-order coverage (simple MIME path)
// ═══════════════════════════════════════════════════════════════════════════

// F1. All 10 sort orders × distinct page sizes – paged result equals reference.
//     Each page size is chosen to exercise a different boundary condition
//     (non-divisible remainder, exact fit, mid-run, etc.).
TEST_F(ListAllNodesByPageTest, AllOrders_AllTypes_PaginationMatchesReference)
{
    const std::vector<OrderAndPageSize> cases = {
        {OrderByClause::DEFAULT_ASC, 7}, // 20 nodes → pages [7,7,6]
        {OrderByClause::DEFAULT_DESC, 6}, // pages [6,6,6,2]
        {OrderByClause::SIZE_ASC, 5}, // pages [5,5,5,5]
        {OrderByClause::SIZE_DESC, 9}, // pages [9,9,2]
        {OrderByClause::MTIME_ASC, 4}, // pages [4,4,4,4,4]
        {OrderByClause::MTIME_DESC, 8}, // pages [8,8,4]
        {OrderByClause::LABEL_ASC, 6}, // pages [6,6,6,2]
        {OrderByClause::LABEL_DESC, 3}, // pages [3,3,3,3,3,3,2]
        {OrderByClause::FAV_ASC, 5}, // pages [5,5,5,5]
        {OrderByClause::FAV_DESC, 7}, // pages [7,7,6]
    };

    for (const auto& [order, pageSize]: cases)
    {
        const auto reference = referenceAll(order);
        const auto paged = collectAllByPage(order, pageSize);
        assertListAllMatchesReference(paged,
                                      reference,
                                      "ALL order=" + std::to_string(order) +
                                          " pageSize=" + std::to_string(pageSize));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  TieBreakTest
//  Minimal fixture whose entire dataset shares the same (size, mtime, label, name).
//  Only the nodehandle differs between nodes, so every sort order that reaches the
//  nodehandle tiebreaker is exercised in its pure form.
//
//  Dataset: ROOT + 5 file nodes, all "tied.txt" (MIME_TYPE_DOCUMENT)
//    size  = 500        (constant → SIZE sorts tie after primary key)
//    mtime = 1'900'000'000  (constant → MTIME sorts tie)
//    label = 2          (nonzero → LABEL_ASC isZero=0; all in same label bucket)
//    fav   = 0
//  Handles are assigned sequentially during insertion:
//    mTiedHandles[0] has the smallest handle, mTiedHandles[4] the largest.
// ─────────────────────────────────────────────────────────────────────────────

class TieBreakTest: public SearchByPageTest
{
protected:
    static constexpr MimeType_t TEST_MIME = MIME_TYPE_DOCUMENT;
    static constexpr int64_t kTiedSize = 500;
    static constexpr int64_t kTiedMtime = 1'900'000'000LL;
    static constexpr int kTiedLabel = 2;
    static constexpr int kNumTied = 5;

    std::vector<NodeHandle> mTiedHandles; ///< insertion order = ascending handle order

    void populateDB() override
    {
        auto root = addNode(ROOTNODE, nullptr, NodeMeta{"ROOT", ROOTNODE, 0, 0, 0, 0});
        mRootHandle = root->nodeHandle();

        for (int i = 0; i < kNumTied; ++i)
        {
            NodeMeta m;
            m.name = "tied.txt";
            m.size = kTiedSize;
            m.mtime = kTiedMtime;
            m.label = kTiedLabel;
            m.fav = 0;
            auto node = addNode(FILENODE, root, m);
            mTiedHandles.push_back(node->nodeHandle());
        }
        // mTiedHandles[0].as8byte() < … < mTiedHandles[4].as8byte()
    }

    std::vector<NodeHandle> collectAllByPage(int order, size_t pageSize) const
    {
        return SearchByPageTest::collectAllByPage(order, pageSize, TEST_MIME);
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Group G – Nodehandle tiebreaker: split at page boundary inside a tied group
//
//  Assertions for each test:
//    1. No duplicate handles across the full result.
//    2. The relative handle order across every consecutive pair is consistent
//       with the sort direction (ASC → strictly increasing, DESC → strictly
//       decreasing). This proves the cursor correctly resumed inside the tied
//       group rather than repeating or skipping entries.
//    3. The full result equals the expected handle sequence.
// ═══════════════════════════════════════════════════════════════════════════

// G1. LABEL_ASC with tied (isZero, label, name) – nodehandle ASC breaks the tie.
//     Exercises buildCursorWhereForListAll (LABEL_ASC, sqlite.cpp:2686-2698):
//       isZero = p1 AND label = p2 AND name = p3 AND nodehandle > p4
TEST_F(TieBreakTest, LabelAsc_TiedLabelAndName_UsesNodehandleTieBreak)
{
    // All nodes share isZero=0, label=kTiedLabel, name="tied.txt".
    // LABEL_ASC reduces to nodehandle ASC → expected: mTiedHandles[0..4].
    const auto paged = collectAllByPage(OrderByClause::LABEL_ASC, 2);
    ASSERT_EQ(paged.size(), static_cast<size_t>(kNumTied));

    const std::set<NodeHandle> unique(paged.begin(), paged.end());
    ASSERT_EQ(unique.size(), paged.size()) << "duplicate handles in LABEL_ASC tied result";

    for (size_t i = 1; i < paged.size(); ++i)
    {
        EXPECT_GT(paged[i].as8byte(), paged[i - 1].as8byte())
            << "nodehandle did not advance (ASC) in LABEL_ASC tied sequence at index " << i;
    }

    EXPECT_EQ(paged, mTiedHandles) << "LABEL_ASC tied result does not match handle-ascending order";
}

// G2. SIZE_DESC with tied (size, name) – nodehandle DESC breaks the tie.
//     Exercises buildCursorWhereForListAll (SIZE_DESC, sqlite.cpp:2646-2652):
//       sizeVirtual = p1 AND name = p2 AND nodehandle < p3
TEST_F(TieBreakTest, SizeDesc_TiedSizeAndName_UsesNodehandleTieBreak)
{
    // All nodes share size=kTiedSize, name="tied.txt".
    // SIZE_DESC: (size DESC, name DESC, nodehandle DESC) → expected: reversed handles.
    const auto paged = collectAllByPage(OrderByClause::SIZE_DESC, 2);
    ASSERT_EQ(paged.size(), static_cast<size_t>(kNumTied));

    const std::set<NodeHandle> unique(paged.begin(), paged.end());
    ASSERT_EQ(unique.size(), paged.size()) << "duplicate handles in SIZE_DESC tied result";

    for (size_t i = 1; i < paged.size(); ++i)
    {
        EXPECT_LT(paged[i].as8byte(), paged[i - 1].as8byte())
            << "nodehandle did not decrease (DESC) in SIZE_DESC tied sequence at index " << i;
    }

    const std::vector<NodeHandle> expected(mTiedHandles.rbegin(), mTiedHandles.rend());
    EXPECT_EQ(paged, expected) << "SIZE_DESC tied result does not match handle-descending order";
}

// G3. MTIME_ASC with tied (mtime, name) – nodehandle ASC breaks the tie.
//     Exercises buildCursorWhereForListAll (MTIME_ASC, sqlite.cpp:2654-2660):
//       mtime = p1 AND name = p2 AND nodehandle > p3
TEST_F(TieBreakTest, MtimeAsc_TiedMtimeAndName_UsesNodehandleTieBreak)
{
    // All nodes share mtime=kTiedMtime, name="tied.txt".
    // MTIME_ASC: (mtime ASC, name ASC, nodehandle ASC) → expected: mTiedHandles[0..4].
    const auto paged = collectAllByPage(OrderByClause::MTIME_ASC, 2);
    ASSERT_EQ(paged.size(), static_cast<size_t>(kNumTied));

    const std::set<NodeHandle> unique(paged.begin(), paged.end());
    ASSERT_EQ(unique.size(), paged.size()) << "duplicate handles in MTIME_ASC tied result";

    for (size_t i = 1; i < paged.size(); ++i)
    {
        EXPECT_GT(paged[i].as8byte(), paged[i - 1].as8byte())
            << "nodehandle did not advance (ASC) in MTIME_ASC tied sequence at index " << i;
    }

    EXPECT_EQ(paged, mTiedHandles) << "MTIME_ASC tied result does not match handle-ascending order";
}

// G4. FAV_DESC with tied (fav, name) – nodehandle ASC breaks the tie.
//     Exercises buildCursorWhereForListAll (FAV_DESC, sqlite.cpp:2678-2684):
//       fav = p1 AND name = p2 AND nodehandle > p3
//
//     This is the "reverse" counterpart to G2 (SIZE_DESC): although both sort
//     orders carry "DESC" in their name, their tiebreaker directions differ.
//     SIZE_DESC uses  nodehandle < p3  (descending),
//     FAV_DESC  uses  nodehandle > p3  (ascending, because fav/name are both ASC
//     in the underlying ORDER BY clause).  A bug that swapped > / < for one
//     of them would not be caught by the other test.
TEST_F(TieBreakTest, FavDesc_TiedFavAndName_UsesNodehandleAscTieBreak)
{
    // All nodes share fav=0 and name="tied.txt".
    // FAV_DESC: ORDER BY fav ASC, name ASC, nodehandle ASC
    //   → nodehandle tiebreaker is ASC (same direction as LABEL_ASC / MTIME_ASC,
    //     opposite to SIZE_DESC which uses nodehandle DESC).
    const auto paged = collectAllByPage(OrderByClause::FAV_DESC, 2);
    ASSERT_EQ(paged.size(), static_cast<size_t>(kNumTied));

    const std::set<NodeHandle> unique(paged.begin(), paged.end());
    ASSERT_EQ(unique.size(), paged.size()) << "duplicate handles in FAV_DESC tied result";

    for (size_t i = 1; i < paged.size(); ++i)
    {
        EXPECT_GT(paged[i].as8byte(), paged[i - 1].as8byte())
            << "nodehandle did not advance (ASC) in FAV_DESC tied sequence at index " << i
            << " – check buildCursorWhereForListAll FAV_DESC uses nodehandle > p3, not <";
    }

    EXPECT_EQ(paged, mTiedHandles) << "FAV_DESC tied result does not match handle-ascending order";
}

// ─────────────────────────────────────────────────────────────────────────────
//  GroupedListAllNodesByPageTest
//  Tests the grouped (UNION ALL CTE) code path used for MIME_TYPE_ALL_DOCS and
//  MIME_TYPE_ALL_VISUAL_MEDIA, plus the simple path for their constituent types
//  (MIME_TYPE_VIDEO, MIME_TYPE_PHOTO) individually.
//
//  Additional dataset (appended on top of the base 20 .txt files):
//    ALL_DOCS members:
//      report_alpha.pdf, notes_delta.pdf  → MIME_TYPE_PDF
//      slides_beta.pptx                  → MIME_TYPE_PRESENTATION
//      sheet_gamma.xlsx                  → MIME_TYPE_SPREADSHEET
//    ALL_VISUAL_MEDIA members:
//      photo_alpha.jpg, photo_beta.jpg, photo_gamma.jpg  → MIME_TYPE_PHOTO
//      video_alpha.mp4, video_beta.mp4, video_gamma.mp4  → MIME_TYPE_VIDEO
// ─────────────────────────────────────────────────────────────────────────────

class GroupedListAllNodesByPageTest: public SearchByPageTest
{
protected:
    void SetUp() override
    {
        SearchByPageTest::SetUp();

        auto root = mClient->mNodeManager.getNodeByHandle(mRootHandle);
        ASSERT_NE(root, nullptr);

        const int64_t baseMtime = 1'800'000'000LL;

        addNode(FILENODE, root, NodeMeta{"report_alpha.pdf", FILENODE, 410, baseMtime + 1, 1, 0});
        addNode(FILENODE, root, NodeMeta{"slides_beta.pptx", FILENODE, 240, baseMtime + 2, 2, 1});
        addNode(FILENODE, root, NodeMeta{"sheet_gamma.xlsx", FILENODE, 510, baseMtime + 3, 3, 0});
        addNode(FILENODE, root, NodeMeta{"notes_delta.pdf", FILENODE, 180, baseMtime + 4, 0, 1});

        addNode(FILENODE, root, NodeMeta{"photo_alpha.jpg", FILENODE, 90, baseMtime + 11, 1, 0});
        addNode(FILENODE, root, NodeMeta{"photo_beta.jpg", FILENODE, 120, baseMtime + 12, 0, 1});
        addNode(FILENODE, root, NodeMeta{"photo_gamma.jpg", FILENODE, 80, baseMtime + 13, 2, 0});
        addNode(FILENODE, root, NodeMeta{"video_alpha.mp4", FILENODE, 900, baseMtime + 21, 0, 1});
        addNode(FILENODE, root, NodeMeta{"video_beta.mp4", FILENODE, 760, baseMtime + 22, 3, 0});
        addNode(FILENODE, root, NodeMeta{"video_gamma.mp4", FILENODE, 830, baseMtime + 23, 1, 1});
    }

    // Ground-truth via searchNodes() (single call, no cursor).
    std::vector<NodeHandle> referenceBySearch(int order, MimeType_t mimeType) const
    {
        NodeSearchFilter filter;
        filter.byAncestors({mRootHandle.as8byte(), UNDEF, UNDEF});
        filter.byNodeType(FILENODE);
        filter.byCategory(mimeType);

        auto nodes =
            mClient->mNodeManager.searchNodes(filter, order, CancelToken{}, NodeSearchPage{0, 0});

        std::vector<NodeHandle> result;
        result.reserve(nodes.size());
        for (const auto& n: nodes)
            result.push_back(n->nodeHandle());
        return result;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Group A – MIME_TYPE_ALL_DOCS (grouped path: UNION ALL CTE over
//            DOCUMENT + PDF + PRESENTATION + SPREADSHEET routes)
//  Dataset: 20 .txt + 2 .pdf + 1 .pptx + 1 .xlsx = 24 nodes
// ═══════════════════════════════════════════════════════════════════════════

// A1. Spot-check: DEFAULT_ASC pagination matches searchNodes reference.
TEST_F(GroupedListAllNodesByPageTest, DefaultAsc_AllDocs_PaginationMatchesSearchReference)
{
    const auto reference = referenceBySearch(OrderByClause::DEFAULT_ASC, MIME_TYPE_ALL_DOCS);
    const auto paged = collectAllByPage(OrderByClause::DEFAULT_ASC, 5, MIME_TYPE_ALL_DOCS);

    assertListAllMatchesReference(paged, reference, "DEFAULT_ASC ALL_DOCS");
}

// A2. Spot-check: SIZE_DESC pagination matches searchNodes reference.
TEST_F(GroupedListAllNodesByPageTest, SizeDesc_AllDocs_PaginationMatchesSearchReference)
{
    const auto reference = referenceBySearch(OrderByClause::SIZE_DESC, MIME_TYPE_ALL_DOCS);
    const auto paged = collectAllByPage(OrderByClause::SIZE_DESC, 4, MIME_TYPE_ALL_DOCS);

    assertListAllMatchesReference(paged, reference, "SIZE_DESC ALL_DOCS");
}

// A3. All 10 sort orders – paged result equals searchNodes reference for each.
TEST_F(GroupedListAllNodesByPageTest, AllOrders_AllDocs_PaginationMatchesSearchReference)
{
    const std::vector<OrderAndPageSize> cases = {
        {OrderByClause::DEFAULT_ASC, 7},
        {OrderByClause::DEFAULT_DESC, 6},
        {OrderByClause::SIZE_ASC, 5},
        {OrderByClause::SIZE_DESC, 9},
        {OrderByClause::MTIME_ASC, 4},
        {OrderByClause::MTIME_DESC, 8},
        {OrderByClause::LABEL_ASC, 6},
        {OrderByClause::LABEL_DESC, 3},
        {OrderByClause::FAV_ASC, 5},
        {OrderByClause::FAV_DESC, 7},
    };

    for (const auto& [order, pageSize]: cases)
    {
        const auto reference = referenceBySearch(order, MIME_TYPE_ALL_DOCS);
        const auto paged = collectAllByPage(order, pageSize, MIME_TYPE_ALL_DOCS);
        assertListAllMatchesReference(paged,
                                      reference,
                                      "ALL_DOCS order=" + std::to_string(order) +
                                          " pageSize=" + std::to_string(pageSize));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group B – MIME_TYPE_ALL_VISUAL_MEDIA (grouped path: UNION ALL CTE over
//            PHOTO + VIDEO routes)
//  Dataset: 3 .jpg + 3 .mp4 = 6 nodes
// ═══════════════════════════════════════════════════════════════════════════

// B1. All 10 sort orders – paged result equals searchNodes reference for each.
TEST_F(GroupedListAllNodesByPageTest, AllOrders_AllVisualMedia_PaginationMatchesSearchReference)
{
    const std::vector<OrderAndPageSize> cases = {
        {OrderByClause::DEFAULT_ASC, 3},
        {OrderByClause::DEFAULT_DESC, 2},
        {OrderByClause::SIZE_ASC, 3},
        {OrderByClause::SIZE_DESC, 2},
        {OrderByClause::MTIME_ASC, 3},
        {OrderByClause::MTIME_DESC, 2},
        {OrderByClause::LABEL_ASC, 3},
        {OrderByClause::LABEL_DESC, 2},
        {OrderByClause::FAV_ASC, 3},
        {OrderByClause::FAV_DESC, 2},
    };

    for (const auto& [order, pageSize]: cases)
    {
        const auto reference = referenceBySearch(order, MIME_TYPE_ALL_VISUAL_MEDIA);
        const auto paged = collectAllByPage(order, pageSize, MIME_TYPE_ALL_VISUAL_MEDIA);
        assertListAllMatchesReference(paged,
                                      reference,
                                      "ALL_VISUAL_MEDIA order=" + std::to_string(order) +
                                          " pageSize=" + std::to_string(pageSize));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group C – Simple path for individual MIME types that belong to groups
//  Verifies that the non-grouped SQL path (mimetypeVirtual = ?) is exercised
//  correctly for MIME_TYPE_VIDEO and MIME_TYPE_PHOTO, which also appear as
//  constituent members inside the ALL_VISUAL_MEDIA grouped path.
// ═══════════════════════════════════════════════════════════════════════════

// C1. MIME_TYPE_VIDEO individually – 3 .mp4 files, all 10 sort orders.
TEST_F(GroupedListAllNodesByPageTest, AllOrders_Video_SimplePathPaginationMatchesSearchReference)
{
    const std::vector<OrderAndPageSize> cases = {
        {OrderByClause::DEFAULT_ASC, 2},
        {OrderByClause::DEFAULT_DESC, 3},
        {OrderByClause::SIZE_ASC, 2},
        {OrderByClause::SIZE_DESC, 3},
        {OrderByClause::MTIME_ASC, 2},
        {OrderByClause::MTIME_DESC, 3},
        {OrderByClause::LABEL_ASC, 2},
        {OrderByClause::LABEL_DESC, 3},
        {OrderByClause::FAV_ASC, 2},
        {OrderByClause::FAV_DESC, 3},
    };

    for (const auto& [order, pageSize]: cases)
    {
        const auto reference = referenceBySearch(order, MIME_TYPE_VIDEO);
        const auto paged = collectAllByPage(order, pageSize, MIME_TYPE_VIDEO);
        ASSERT_EQ(reference.size(), 3u) << "unexpected VIDEO count for order=" << order;
        assertListAllMatchesReference(paged,
                                      reference,
                                      "VIDEO order=" + std::to_string(order) +
                                          " pageSize=" + std::to_string(pageSize));
    }
}

// C2. MIME_TYPE_PHOTO individually – 3 .jpg files, all 10 sort orders.
TEST_F(GroupedListAllNodesByPageTest, AllOrders_Photo_SimplePathPaginationMatchesSearchReference)
{
    const std::vector<OrderAndPageSize> cases = {
        {OrderByClause::DEFAULT_ASC, 2},
        {OrderByClause::DEFAULT_DESC, 3},
        {OrderByClause::SIZE_ASC, 2},
        {OrderByClause::SIZE_DESC, 3},
        {OrderByClause::MTIME_ASC, 2},
        {OrderByClause::MTIME_DESC, 3},
        {OrderByClause::LABEL_ASC, 2},
        {OrderByClause::LABEL_DESC, 3},
        {OrderByClause::FAV_ASC, 2},
        {OrderByClause::FAV_DESC, 3},
    };

    for (const auto& [order, pageSize]: cases)
    {
        const auto reference = referenceBySearch(order, MIME_TYPE_PHOTO);
        const auto paged = collectAllByPage(order, pageSize, MIME_TYPE_PHOTO);
        ASSERT_EQ(reference.size(), 3u) << "unexpected PHOTO count for order=" << order;
        assertListAllMatchesReference(paged,
                                      reference,
                                      "PHOTO order=" + std::to_string(order) +
                                          " pageSize=" + std::to_string(pageSize));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Group D – Stability under concurrent modification (grouped MIME path)
// ═══════════════════════════════════════════════════════════════════════════

// D1. AllVisualMedia_DefaultDesc_DeletionBetweenPages_NoSkip
//     Mirrors ListAllNodesByPageTest::Deletion_BetweenPages_NoSkip (E1) but uses
//     the grouped MIME path (MIME_TYPE_ALL_VISUAL_MEDIA → UNION ALL CTE over
//     PHOTO + VIDEO, sqlite.cpp:2878).  A delete between pages must not cause
//     cursor-based pagination to skip any remaining visual-media nodes.
//
//     Dataset: 3 .jpg + 3 .mp4 = 6 nodes. DEFAULT_DESC orders them by name DESC:
//       video_gamma.mp4, video_beta.mp4, video_alpha.mp4,
//       photo_gamma.jpg, photo_beta.jpg, photo_alpha.jpg
//     Page 1 (size=3) = first three. Deleted = photo_gamma.jpg (4th in ordering).
//     Expected in page 2+: photo_beta.jpg, photo_alpha.jpg.
TEST_F(GroupedListAllNodesByPageTest, AllVisualMedia_DefaultDesc_DeletionBetweenPages_NoSkip)
{
    constexpr MimeType_t mime = MIME_TYPE_ALL_VISUAL_MEDIA;
    constexpr int order = OrderByClause::DEFAULT_DESC;
    constexpr size_t pageSize = 3;

    const auto fullReference = referenceBySearch(order, mime);
    ASSERT_EQ(fullReference.size(), 6u);

    auto page1 = mClient->mNodeManager.listAllNodesByPage(mime,
                                                          order,
                                                          CancelToken{},
                                                          pageSize,
                                                          std::nullopt);
    ASSERT_EQ(page1.size(), pageSize);

    // Delete the first node of what would be page 2 in offset terms.
    const NodeHandle toDelete = fullReference[pageSize];
    auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get());
    ASSERT_NE(sa, nullptr);
    ASSERT_TRUE(sa->remove(toDelete)) << "failed to delete node from DB";

    const auto page2Plus =
        collectAllByPage(order, pageSize, mime, cursorFor(page1.back()->nodeHandle(), order));

    const std::set<NodeHandle> page2Set(page2Plus.begin(), page2Plus.end());

    EXPECT_EQ(page2Set.count(toDelete), 0u) << "deleted node appeared in page 2+";

    // Every node after the deleted one in the reference ordering must appear.
    for (size_t i = pageSize + 1; i < fullReference.size(); ++i)
    {
        EXPECT_EQ(page2Set.count(fullReference[i]), 1u)
            << "visual-media node at reference index " << i << " was skipped after deletion";
    }

    for (const auto& n: page1)
    {
        EXPECT_EQ(page2Set.count(n->nodeHandle()), 0u) << "page-1 handle repeated in page 2+";
    }
}

} // anonymous namespace

#endif // USE_SQLITE
