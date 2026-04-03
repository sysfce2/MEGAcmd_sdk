#include "megaapi.h"
#include "SdkTestNodesSetUp.h"

#include <gmock/gmock.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

using namespace sdk_test;
using namespace testing;
using namespace std::chrono_literals;

namespace
{

std::vector<std::string> toNames(MegaNodeList* nodes)
{
    std::vector<std::string> result;
    result.reserve(static_cast<size_t>(nodes->size()));
    for (int i = 0; i < nodes->size(); ++i)
        result.emplace_back(nodes->get(i)->getName());
    return result;
}

/**
 * @brief Build a MegaSearchCursorOffset from the last node of a page.
 *
 * Sets only the fields required by the given sort @p order.
 */
std::unique_ptr<MegaSearchCursorOffset> makeCursor(MegaNode* node, int order)
{
    std::unique_ptr<MegaSearchCursorOffset> cursor(MegaSearchCursorOffset::createInstance());
    cursor->setLastName(node->getName());
    cursor->setLastHandle(node->getHandle());
    switch (order)
    {
        case MegaApi::ORDER_SIZE_ASC:
        case MegaApi::ORDER_SIZE_DESC:
            cursor->setLastSize(node->getSize());
            break;
        case MegaApi::ORDER_MODIFICATION_ASC:
        case MegaApi::ORDER_MODIFICATION_DESC:
            cursor->setLastMtime(node->getModificationTime());
            break;
        case MegaApi::ORDER_LABEL_ASC:
        case MegaApi::ORDER_LABEL_DESC:
            cursor->setLastLabel(node->getLabel());
            break;
        case MegaApi::ORDER_FAV_ASC:
        case MegaApi::ORDER_FAV_DESC:
            cursor->setLastFav(node->isFavourite() ? 1 : 0);
            break;
        default:
            break;
    }
    return cursor;
}

/**
 * @brief Describes one pagination scenario: sort order and the expected page contents.
 *
 * pageSize is fixed at 2 throughout the pagination suite, so:
 *   page1 holds the first two results,
 *   page2 holds the remainder (1 node for the 3-node PHOTO set).
 */
struct PaginationTestCase
{
    const char* name;
    int order;
    std::vector<std::string> page1; // expected names on page 1
    std::vector<std::string> page2; // expected names on page 2
};

/**
 * @brief Drives one full pagination sequence (page1 → page2 → empty) for a single test case.
 *
 * Uses SCOPED_TRACE so failures are attributed to @p tc.name.
 * ASSERT_* calls return from this helper on failure; the caller's loop continues.
 */
void checkPagination(MegaApi* api, int mimeType, const PaginationTestCase& tc, size_t pageSize)
{
    SCOPED_TRACE(tc.name);

    std::unique_ptr<MegaNodeList> page1(
        api->listAllNodesByPage(mimeType, tc.order, nullptr, pageSize, nullptr));
    ASSERT_NE(page1, nullptr);
    ASSERT_EQ(page1->size(), static_cast<int>(tc.page1.size()));
    EXPECT_THAT(toNames(page1.get()), ElementsAreArray(tc.page1));

    ASSERT_GT(page1->size(), 0);
    MegaNode* lastOfPage1 = page1->get(page1->size() - 1);
    ASSERT_NE(lastOfPage1, nullptr);
    auto cursor1 = makeCursor(lastOfPage1, tc.order);
    std::unique_ptr<MegaNodeList> page2(
        api->listAllNodesByPage(mimeType, tc.order, nullptr, pageSize, cursor1.get()));
    ASSERT_NE(page2, nullptr);
    ASSERT_EQ(page2->size(), static_cast<int>(tc.page2.size()));
    EXPECT_THAT(toNames(page2.get()), ElementsAreArray(tc.page2));

    ASSERT_GT(page2->size(), 0);
    MegaNode* lastOfPage2 = page2->get(page2->size() - 1);
    ASSERT_NE(lastOfPage2, nullptr);
    auto cursor2 = makeCursor(lastOfPage2, tc.order);
    std::unique_ptr<MegaNodeList> page3(
        api->listAllNodesByPage(mimeType, tc.order, nullptr, pageSize, cursor2.get()));
    ASSERT_NE(page3, nullptr);
    EXPECT_EQ(page3->size(), 0);
}

} // namespace

/**
 * Test suite for MegaApi::listAllNodesByPage().
 *
 * Node tree (3 photos, 2 videos, 2 docs):
 *
 *  Name             Ext    Type      Size  Mtime  Label         Fav
 *  alpha            .jpg   PHOTO      100    7h   RED(1)        false
 *  bravo            .mp4   VIDEO      200    6h   ORANGE(2)     false
 *  charlie          .docx  DOCUMENT   300    5h   YELLOW(3)     true
 *  delta            .jpg   PHOTO      400    4h   GREEN(4)      true
 *  echo             .mp4   VIDEO      500    3h   BLUE(5)       false
 *  foxtrot          .docx  DOCUMENT   600    2h   PURPLE(6)     false
 *  golf             .jpg   PHOTO      700    1h   GREY(7)       true
 *
 * PHOTO filter matches: alpha.jpg, delta.jpg, golf.jpg
 * ALL_VISUAL_MEDIA filter matches: alpha.jpg, bravo.mp4, delta.jpg, echo.mp4, golf.jpg
 * Docs (charlie.docx, foxtrot.docx) must not appear in either query.
 */
class SdkTestListAllNodesByPage: public SdkTestNodesSetUp
{
    const std::vector<NodeInfo>& getElements() const override
    {
        // name               label                        fav    size  mtime-age
        static const std::vector<NodeInfo> ELEMENTS{
            FileNodeInfo("alpha.jpg", MegaNode::NODE_LBL_RED, false, 100, 7h),
            FileNodeInfo("bravo.mp4", MegaNode::NODE_LBL_ORANGE, false, 200, 6h),
            FileNodeInfo("charlie.docx", MegaNode::NODE_LBL_YELLOW, true, 300, 5h),
            FileNodeInfo("delta.jpg", MegaNode::NODE_LBL_GREEN, true, 400, 4h),
            FileNodeInfo("echo.mp4", MegaNode::NODE_LBL_BLUE, false, 500, 3h),
            FileNodeInfo("foxtrot.docx", MegaNode::NODE_LBL_PURPLE, false, 600, 2h),
            FileNodeInfo("golf.jpg", MegaNode::NODE_LBL_GREY, true, 700, 1h),
        };
        return ELEMENTS;
    }

    const std::string& getRootTestDir() const override
    {
        static const std::string dirName{"SDK_TEST_LISTALLNODESBYPAGE"};
        return dirName;
    }

    bool keepDifferentCreationTimes() override
    {
        return false;
    }
};

// ─── Group 1: MIME type filtering, no cursor (all results) ─────

TEST_F(SdkTestListAllNodesByPage, MimeFilter_AllResults)
{
    struct AllResultsCase
    {
        const char* name;
        int mimeType;
        int order;
        std::vector<std::string> expected;
    };

    // clang-format off
    static const std::vector<AllResultsCase> kCases{
        {"PHOTO/DEFAULT_ASC",
         MegaApi::FILE_TYPE_PHOTO, MegaApi::ORDER_DEFAULT_ASC,
         {"alpha.jpg", "delta.jpg", "golf.jpg"}},
        {"ALL_VISUAL_MEDIA/DEFAULT_ASC",
         MegaApi::FILE_TYPE_ALL_VISUAL_MEDIA, MegaApi::ORDER_DEFAULT_ASC,
         {"alpha.jpg", "bravo.mp4", "delta.jpg", "echo.mp4", "golf.jpg"}},
    };
    // clang-format on

    for (const auto& tc: kCases)
    {
        SCOPED_TRACE(tc.name);
        std::unique_ptr<MegaNodeList> results(
            megaApi[0]->listAllNodesByPage(tc.mimeType, tc.order, nullptr, 0, nullptr));
        ASSERT_NE(results, nullptr);
        EXPECT_THAT(toNames(results.get()), ElementsAreArray(tc.expected));
    }
}

// ─── Group 2: All orders, PHOTO filter, pageSize=2 ───────────────────────────
//
// All 10 sort orders are driven by the table below.  Each row specifies the
// sort order and the two non-empty pages expected from the 3-node PHOTO set
// {alpha.jpg(size=100,mtime=-7h,label=1,fav=0),
//  delta.jpg(size=400,mtime=-4h,label=4,fav=1),
//  golf.jpg (size=700,mtime=-1h,label=7,fav=1)}.

TEST_F(SdkTestListAllNodesByPage, AllOrders_Photo_Pagination)
{
    // clang-format off
    static const std::vector<PaginationTestCase> kCases{
        // name             order                            page1                          page2
        {"DEFAULT_ASC",  MegaApi::ORDER_DEFAULT_ASC,  {"alpha.jpg", "delta.jpg"}, {"golf.jpg"}},
        {"DEFAULT_DESC", MegaApi::ORDER_DEFAULT_DESC, {"golf.jpg",  "delta.jpg"}, {"alpha.jpg"}},
        {"SIZE_ASC",     MegaApi::ORDER_SIZE_ASC,     {"alpha.jpg", "delta.jpg"}, {"golf.jpg"}},
        {"SIZE_DESC",    MegaApi::ORDER_SIZE_DESC,    {"golf.jpg",  "delta.jpg"}, {"alpha.jpg"}},
        {"MTIME_ASC",    MegaApi::ORDER_MODIFICATION_ASC,  {"alpha.jpg", "delta.jpg"}, {"golf.jpg"}},
        {"MTIME_DESC",   MegaApi::ORDER_MODIFICATION_DESC, {"golf.jpg",  "delta.jpg"}, {"alpha.jpg"}},
        {"LABEL_ASC",    MegaApi::ORDER_LABEL_ASC,    {"alpha.jpg", "delta.jpg"}, {"golf.jpg"}},
        {"LABEL_DESC",   MegaApi::ORDER_LABEL_DESC,   {"golf.jpg",  "delta.jpg"}, {"alpha.jpg"}},
        // FAV_ASC  = ORDER BY fav DESC, name ASC → favs (delta,golf) first, then alpha
        {"FAV_ASC",      MegaApi::ORDER_FAV_ASC,      {"delta.jpg", "golf.jpg"},  {"alpha.jpg"}},
        // FAV_DESC = ORDER BY fav ASC,  name ASC → non-fav (alpha) first, then delta,golf
        {"FAV_DESC",     MegaApi::ORDER_FAV_DESC,     {"alpha.jpg", "delta.jpg"}, {"golf.jpg"}},
    };
    // clang-format on

    for (const auto& tc: kCases)
        ASSERT_NO_FATAL_FAILURE(checkPagination(megaApi[0].get(), MegaApi::FILE_TYPE_PHOTO, tc, 2));
}

// ─── Group 3: Invalid inputs ──────────────────────────────────────────────────

TEST_F(SdkTestListAllNodesByPage, InvalidInputs_ReturnEmpty)
{
    auto expectEmpty = [&](int mimeType, int order, MegaSearchCursorOffset* cursor)
    {
        std::unique_ptr<MegaNodeList> r(
            megaApi[0]->listAllNodesByPage(mimeType, order, nullptr, 0, cursor));
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(r->size(), 0);
    };

    // ── Invalid MIME types ────────────────────────────────────────────────────
    // mimeType <= FILE_TYPE_DEFAULT (0), > FILE_TYPE_LAST, or negative are all disallowed.

    struct InvalidMimeCase
    {
        const char* name;
        int mimeType;
    };

    // clang-format off
    const std::vector<InvalidMimeCase> mimeCases{
        {"FILE_TYPE_DEFAULT", MegaApi::FILE_TYPE_DEFAULT},
        {"FILE_TYPE_LAST+1",  MegaApi::FILE_TYPE_LAST + 1},
        {"negative (-1)",     -1},
    };
    // clang-format on

    for (const auto& tc: mimeCases)
    {
        SCOPED_TRACE(tc.name);
        expectEmpty(tc.mimeType, MegaApi::ORDER_DEFAULT_ASC, nullptr);
    }

    // ── Unsupported order ─────────────────────────────────────────────────────
    // Rejected regardless of whether a cursor is supplied.
    expectEmpty(MegaApi::FILE_TYPE_PHOTO, MegaApi::ORDER_CREATION_ASC, nullptr);

    // ── Invalid cursors ───────────────────────────────────────────────────────

    const std::unique_ptr<MegaNode> alpha(
        megaApi[0]->getNodeByPath(convertToTestPath("alpha.jpg").c_str()));
    ASSERT_NE(alpha, nullptr);

    // Each case: a description, the sort order, and a lambda that configures the cursor.
    // Fields not set by the lambda stay at their invalid default values.
    struct InvalidCursorCase
    {
        const char* name;
        int order;
        std::function<void(MegaSearchCursorOffset*)> setup;
    };

    // clang-format off
    const std::vector<InvalidCursorCase> cursorCases{
        {"empty name (omit setLastName)",         MegaApi::ORDER_DEFAULT_ASC,
         [&](MegaSearchCursorOffset* c){ c->setLastHandle(alpha->getHandle()); }},
        {"invalid handle (omit setLastHandle)",   MegaApi::ORDER_DEFAULT_ASC,
         [&](MegaSearchCursorOffset* c){ c->setLastName(alpha->getName()); }},
        {"missing size for SIZE order",           MegaApi::ORDER_SIZE_ASC,
         [&](MegaSearchCursorOffset* c){ c->setLastName(alpha->getName()); c->setLastHandle(alpha->getHandle()); }},
        {"missing mtime for MODIFICATION order",  MegaApi::ORDER_MODIFICATION_ASC,
         [&](MegaSearchCursorOffset* c){ c->setLastName(alpha->getName()); c->setLastHandle(alpha->getHandle()); }},
        {"out-of-range label (99) for LABEL order", MegaApi::ORDER_LABEL_ASC,
         [&](MegaSearchCursorOffset* c){ c->setLastName(alpha->getName()); c->setLastHandle(alpha->getHandle()); c->setLastLabel(99); }},
        {"invalid fav (2) for FAV order",         MegaApi::ORDER_FAV_ASC,
         [&](MegaSearchCursorOffset* c){ c->setLastName(alpha->getName()); c->setLastHandle(alpha->getHandle()); c->setLastFav(2); }},
        {"unsupported order with cursor (ORDER_CREATION_ASC)", MegaApi::ORDER_CREATION_ASC,
         [&](MegaSearchCursorOffset* c){ c->setLastName(alpha->getName()); c->setLastHandle(alpha->getHandle()); }},
    };
    // clang-format on

    for (const auto& tc: cursorCases)
    {
        SCOPED_TRACE(tc.name);
        std::unique_ptr<MegaSearchCursorOffset> cursor(MegaSearchCursorOffset::createInstance());
        tc.setup(cursor.get());
        expectEmpty(MegaApi::FILE_TYPE_PHOTO, tc.order, cursor.get());
    }
}
