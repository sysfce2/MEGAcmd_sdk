/**
 * (c) 2025 by Mega Limited, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include "mega/base64.h"
#include "mega/db/sqlite.h"
#include "mega/megaapp.h"
#include "mega/megaclient.h"
#include "mega/sc_streaming_parser.h"
#include "utils.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace mega;

namespace
{

class ScStreamingParserTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        app = std::make_shared<MegaApp>();
        client = mt::makeClient(*app);
        scStreamingParser = std::make_shared<ScStreamingParser>(*client);
    }

    void TearDown() override
    {
        scStreamingParser.reset();
        client.reset();
        app.reset();
    }

    std::shared_ptr<Node> initNodes()
    {
        client->dbaccess = new SqliteDbAccess(LocalPath::fromAbsolutePath("."));
        client->sid =
            "AWA5YAbtb4JO-y2zWxmKZpSe5-6XM7CTEkA-3Nv7J4byQUpOazdfSC1ZUFlS-kah76gPKUEkTF9g7MeE";
        client->opensctable();

        // Create root node
        const std::string rootNodeHandleString = "Ll5VkSJZ";
        byte buf[8] = {0};
        Base64::atob(rootNodeHandleString.c_str(), buf, MegaClient::NODEHANDLE);
        const handle rootNodeHandle = MemAccess::get<handle>((const char*)buf);
        std::shared_ptr<Node> rootNode = addNode(ROOTNODE, rootNodeHandle);

        // Create vault node and rubbish node
        addNode(VAULTNODE, 1);
        addNode(RUBBISHNODE, 2);

        return rootNode;
    }

    std::shared_ptr<Node> addNode(nodetype_t nodeType, handle nodeHandle)
    {
        auto& nodeRef =
            mt::makeNode(*client, nodeType, mega::NodeHandle().set6byte(nodeHandle), nullptr);
        std::shared_ptr<Node> node(&nodeRef);

        NodeManager::MissingParentNodes missingParentNodes;

        client->mNodeManager.addNode(node, false, false, missingParentNodes);

        return node;
    }

    std::shared_ptr<MegaApp> app;
    std::shared_ptr<MegaClient> client;
    std::shared_ptr<ScStreamingParser> scStreamingParser;
};

void testFinishedProcess(std::shared_ptr<ScStreamingParser>& scStreamingParser,
                         MegaClient& client,
                         std::string& json,
                         std::string&& url,
                         const char* sn)
{
    ASSERT_TRUE(scStreamingParser->process(json.c_str()) > 0);

    ASSERT_TRUE(scStreamingParser->hasStarted());
    ASSERT_TRUE(scStreamingParser->isFinished());
    ASSERT_FALSE(scStreamingParser->isFailed());
    ASSERT_FALSE(scStreamingParser->isPaused());

    ASSERT_TRUE(client.scnotifyurl == url);

    handle snHdl;
    Base64::atob(sn, (byte*)&snHdl, sizeof(snHdl));
    ASSERT_TRUE(client.scsn.getHandle() == snHdl);
}

TEST_F(ScStreamingParserTest, InitAndClear)
{
    // Init
    scStreamingParser->init();

    ASSERT_FALSE(scStreamingParser->hasStarted());
    ASSERT_FALSE(scStreamingParser->isFinished());
    ASSERT_FALSE(scStreamingParser->isFailed());
    ASSERT_FALSE(scStreamingParser->isPaused());

    // Clear
    scStreamingParser->clear();

    ASSERT_FALSE(scStreamingParser->hasStarted());
    ASSERT_FALSE(scStreamingParser->isFinished());
    ASSERT_FALSE(scStreamingParser->isFailed());
    ASSERT_FALSE(scStreamingParser->isPaused());
}

TEST_F(ScStreamingParserTest, SetLastReceived)
{
    ASSERT_FALSE(scStreamingParser->isLastReceived());

    scStreamingParser->setLastReceived();

    ASSERT_TRUE(scStreamingParser->isLastReceived());

    scStreamingParser->clear();

    ASSERT_FALSE(scStreamingParser->isLastReceived());
}

TEST_F(ScStreamingParserTest, Process)
{
    std::string json =
        R"({"a":[{"a":"ua","st":"!?>VwgM","u":"k-N5-o6LLkE","ua":["^!stbmp"],"v":["Gt-009Sr-XA"]}],"w":"https://g.api.mega.co.nz/wsc/5lhYq8nqzgIEE8j9OqymmA","sn":"Gt-009Sr-XA"})";

    scStreamingParser->init();

    // Process 1st SC packet
    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        json,
                        "https://g.api.mega.co.nz/wsc/5lhYq8nqzgIEE8j9OqymmA",
                        "Gt-009Sr-XA");

    // Clear for next process
    scStreamingParser->clear();

    ASSERT_FALSE(scStreamingParser->hasStarted());
    ASSERT_FALSE(scStreamingParser->isFinished());
    ASSERT_FALSE(scStreamingParser->isFailed());
    ASSERT_FALSE(scStreamingParser->isPaused());

    // Process 2nd SC packet
    json =
        R"({"a":[{"a":"ipc","st":"!?>?e)G","p":"gjqrEkfohxc","m":"sdk-jenkins+005-19@mega.nz","msg":"","ps":0,"ts":1761821624,"uts":1761821624}],"w":"https://g.api.mega.co.nz/wsc/wZ9inPVUsAufT1YgyZ_AIA","sn":"P-LLOv0J3u0"})";

    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        json,
                        "https://g.api.mega.co.nz/wsc/wZ9inPVUsAufT1YgyZ_AIA",
                        "P-LLOv0J3u0");
}

TEST_F(ScStreamingParserTest, ProcessWithActionNameNotPresent)
{
    std::string json =
        R"({"a":[{"st":"!?>VwgM","u":"k-N5-o6LLkE","ua":["^!stbmp"],"v":["Gt-009Sr-XA"]}],"w":"https://g.api.mega.co.nz/wsc/5lhYq8nqzgIEE8j9OqymmA","sn":"Gt-009Sr-XA"})";

    scStreamingParser->init();

    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        json,
                        "https://g.api.mega.co.nz/wsc/5lhYq8nqzgIEE8j9OqymmA",
                        "Gt-009Sr-XA");
}

TEST_F(ScStreamingParserTest, ProcessWithSessionIdPresent)
{
    std::string json1 = R"({"a":[{"a":"mcc","i":")";
    std::string json2 =
        R"(","id":"FpldU29F-WY","u":[{"u":"tUd0zWN1Qn4","p":3},{"u":"ecgkxzbjRsU","p":2}],"cs":0,"ts":1761821986,"g":1,"ou":"tUd0zWN1Qn4"}],"w":"https://g.api.mega.co.nz/wsc/EiDoieE-5Tuklb0sF-bRcw","sn":"xyp4UX7xFZ4"})";

    scStreamingParser->init();

    // Non self-originated
    std::string sessionId = "cpbtkgwuai";
    std::string json = json1 + sessionId + json2;
    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        json,
                        "https://g.api.mega.co.nz/wsc/EiDoieE-5Tuklb0sF-bRcw",
                        "xyp4UX7xFZ4");

    scStreamingParser->clear();

    // Self-originated
    sessionId = string(client->sessionid, sizeof(client->sessionid));
    json = json1 + sessionId + json2;
    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        json,
                        "https://g.api.mega.co.nz/wsc/EiDoieE-5Tuklb0sF-bRcw",
                        "xyp4UX7xFZ4");
}

TEST_F(ScStreamingParserTest, ProcessWithSeqTagNotPresent)
{
    std::string json =
        R"({"a":[{"a":"ua", "u":"k-N5-o6LLkE","ua":["^!stbmp"],"v":["Gt-009Sr-XA"]}],"w":"https://g.api.mega.co.nz/wsc/5lhYq8nqzgIEE8j9OqymmA","sn":"Gt-009Sr-XA"})";

    scStreamingParser->init();

    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        json,
                        "https://g.api.mega.co.nz/wsc/5lhYq8nqzgIEE8j9OqymmA",
                        "Gt-009Sr-XA");
}

TEST_F(ScStreamingParserTest, ProcessChunks)
{
    std::string json =
        R"({"a":[{"a":"ua","isn":"xyp4UX7xFZ4","st":"!?>AmY:","u":"MOte1pKQgDI","ua":["^!prd"],"v":["Dvh72Rs7JBM"]},{"a":"ua","st":"!?>VwgM","u":"k-N5-o6LLkE","ua":["^!stbmp"],"v":["Gt-009Sr-XA"]}],"w":"https://g.api.)";

    scStreamingParser->init();

    // Process the incomplete packet
    size_t consumed = (size_t)scStreamingParser->process(json.c_str());

    ASSERT_TRUE(consumed > 0);
    ASSERT_TRUE(scStreamingParser->hasStarted());
    ASSERT_FALSE(scStreamingParser->isFinished());
    ASSERT_FALSE(scStreamingParser->isFailed());
    ASSERT_FALSE(scStreamingParser->isPaused());
    ASSERT_TRUE(client->scnotifyurl.empty());

    const char* isn = "xyp4UX7xFZ4";
    handle isnHdl;
    Base64::atob(isn, (byte*)&isnHdl, sizeof(isnHdl));
    ASSERT_TRUE(client->scsn.getHandle() == isnHdl);

    // Purge
    json.erase(0, consumed);

    // Process the remained
    std::string jsonRemained =
        json + R"(mega.co.nz/wsc/5lhYq8nqzgIEE8j9OqymmA","sn":"Gt-009Sr-XA"})";

    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        jsonRemained,
                        "https://g.api.mega.co.nz/wsc/5lhYq8nqzgIEE8j9OqymmA",
                        "Gt-009Sr-XA");
}

TEST_F(ScStreamingParserTest, ProcessChunksByMove)
{
    initNodes();

    std::string jsonCreate =
        R"({"a":[{"a":"t","st":"!G(<Cq+","t":{"f":[{"h":"i4om2BrJ","t":1,"a":"k8mDAq0-TfskLMyMqcxssQ","k":"vN8A0kvxmC0:cXtlV5RG0QHpBxl-sZWQxA","p":"Ll5VkSJZ","ts":1770364711,"u":"vN8A0kvxmC0","i":0}]},"ou":"vN8A0kvxmC0"}, {"a":"t","st":"!G(<IS$","t":{"f":[{"h":"HtpQjbba","t":1,"a":"STYGCigAjt9fSXwkPSYxVA","k":"vN8A0kvxmC0:3BrxInvwc6gysUP-NmZ8Fg","p":"Ll5VkSJZ","ts":1770364718,"u":"vN8A0kvxmC0","i":0}]},"ou":"vN8A0kvxmC0"}],"w":"https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ","sn":"JcnE8wc8Xz0"})";
    std::string jsonMovePart1 =
        R"({"a":[{"a":"d","isn":"xyp4UX7xFZ4","i":"fbtekrfnlb","st":"!G(<L_j","n":"i4om2BrJ","m":1,"ou":"vN8A0kvxmC0"},)";
    std::string jsonMovePart2 =
        R"({"a":"t","i":"fbtekrfnlb","t":{"f":[{"h":"i4om2BrJ","p":"HtpQjbba","u":"vN8A0kvxmC0","t":1,"a":"k8mDAq0-TfskLMyMqcxssQ","k":"vN8A0kvxmC0:cXtlV5RG0QHpBxl-sZWQxA","ts":1770364711}],"u":[{"u":"vN8A0kvxmC0","m":"jye+test3@mega.co.nz","m2":["jye+test3@mega.co.nz"],"pubk":"CADLwMeFsNUFHY5vIHrDF73XMk6lZvRPQlhh67NGgg8WdiZF7vtM4HrCoftYvQEFLM-JF498dEFLNo9f76KydgWSdl_IEFT4KpnuexvJpfwI0eyJRzU1Wt5wegWJw9WPEKxpHGP91VllLPWB31X2FH48angXw9Mf_3nyjNh-q83tMEbT5XEWuL3KqiMdP80XeqJk3TOuCcaSmq5tzj84rzc1sNnsoNkgYPYuGjOqzA8VzURVU6Tp7BV2eJm0x68dCwoJMHiZVNor1fz0I-iB1Vj2Hurr1NmIGLP9AEJOSKE1iwBSmseTOci3KaLcMrwy1rQMx8r9y-KV5eBnt7kPRSCdACAAAAEB","+puCu255":"dLFE7BT0zCf3Q2f0EXVGRUZRtVuY0J_TPZVMkhAI3Hs","+puEd255":"owpFu55NPN4Y93GX8-iDVcbmktEvd-LcYtgMJUILLxU","+sigCu255":"AAAAAGj5nvcOQZQogXSCNe7RkRLP21nkWcr3D3zx2zY0xC0BqY5_Ny_O1JMaN95YtbkO8dgcYFGiHaW1CWnu9njDcGtnKm8A","+sigPubk":"AAAAAGj5nvgwEi2YXMbNhHdsV1IDYs62RzF1p60M4PkVxXx68HGqF7nBQSg69KUbh_yydf36nneGFMm-7UnKr_yZyFei2wsD"}]},"ou":"vN8A0kvxmC0"}],)";
    std::string jsonMovePart3 =
        R"("w":"https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ","sn":"YydQMr-woLo"})";

    scStreamingParser->init();

    // Create 2 folder
    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        jsonCreate,
                        "https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ",
                        "JcnE8wc8Xz0");

    // Clear for next process
    scStreamingParser->clear();

    // Process "d" part
    std::string jsonMove = jsonMovePart1;

    size_t consumed = (size_t)scStreamingParser->process(jsonMove.c_str());

    ASSERT_TRUE(consumed > 0);
    ASSERT_TRUE(scStreamingParser->hasStarted());
    ASSERT_FALSE(scStreamingParser->isFinished());
    ASSERT_FALSE(scStreamingParser->isFailed());
    ASSERT_FALSE(scStreamingParser->isPaused());

    // isn is received, but is not stored yet.
    const char* sn = "JcnE8wc8Xz0";
    handle snHdl;
    Base64::atob(sn, (byte*)&snHdl, sizeof(snHdl));
    ASSERT_TRUE(client->scsn.getHandle() == snHdl);

    // Purge
    jsonMove.erase(0, consumed);

    // Process "t" part
    jsonMove += jsonMovePart2;

    consumed = (size_t)scStreamingParser->process(jsonMove.c_str());

    ASSERT_TRUE(consumed > 0);
    ASSERT_TRUE(scStreamingParser->hasStarted());
    ASSERT_FALSE(scStreamingParser->isFinished());
    ASSERT_FALSE(scStreamingParser->isFailed());
    ASSERT_FALSE(scStreamingParser->isPaused());

    const char* isn = "xyp4UX7xFZ4";
    handle isnHdl;
    Base64::atob(isn, (byte*)&isnHdl, sizeof(isnHdl));
    ASSERT_TRUE(client->scsn.getHandle() == isnHdl);

    // Purge
    jsonMove.erase(0, consumed);

    // Process the remained
    jsonMove += jsonMovePart3;

    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        jsonMove,
                        "https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ",
                        "YydQMr-woLo");
}

TEST_F(ScStreamingParserTest, ProcessAndPause)
{
    std::string json =
        R"({"a":[{"a":"ipc","st":"!?>?e)G","p":"gjqrEkfohxc","m":"sdk-jenkins+005-19@mega.nz","msg":"","ps":0,"ts":1761821624,"uts":1761821624}],"w":"https://g.api.mega.co.nz/wsc/wZ9inPVUsAufT1YgyZ_AIA","sn":"P-LLOv0J3u0"})";
    std::string jsonRes =
        R"([["!?>?e)G",{"p":"gjqrEkfohxc","m":"sdk-jenkins+005-20@mega.nz","e":"sdk-jenkins+005-19@mega.nz","msg":"Hi contact. This is a testing message","ts":1761821627,"uts":1761821627,"rts":1761821627}]])";

    scStreamingParser->init();

    // Simulate the case: command is sent, but response has not been received
    client->queueCommand(new CommandSetPendingContact(client.get(), "", (opcactions_t)0));
    bool dummy1;
    string dummy2;
    client->reqs.serverrequest(dummy1, client.get(), dummy2);

    size_t consumed = (size_t)scStreamingParser->process(json.c_str());

    ASSERT_TRUE(consumed > 0);
    ASSERT_TRUE(scStreamingParser->hasStarted());
    ASSERT_FALSE(scStreamingParser->isFinished());
    ASSERT_FALSE(scStreamingParser->isFailed());
    ASSERT_TRUE(scStreamingParser->isPaused());
    ASSERT_TRUE(client->scnotifyurl.empty());
    ASSERT_FALSE(client->scsn.ready());

    // Purge
    json.erase(0, consumed);

    // Response is received
    client->reqs.serverresponse(std::move(jsonRes), client.get());

    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        json,
                        "https://g.api.mega.co.nz/wsc/wZ9inPVUsAufT1YgyZ_AIA",
                        "P-LLOv0J3u0");
}

TEST_F(ScStreamingParserTest, ProcessPacketTreeByMove)
{
    initNodes();

    std::string jsonCreate =
        R"({"a":[{"a":"t","st":"!G(<Cq+","t":{"f":[{"h":"i4om2BrJ","t":1,"a":"k8mDAq0-TfskLMyMqcxssQ","k":"vN8A0kvxmC0:cXtlV5RG0QHpBxl-sZWQxA","p":"Ll5VkSJZ","ts":1770364711,"u":"vN8A0kvxmC0","i":0}]},"ou":"vN8A0kvxmC0"}, {"a":"t","st":"!G(<IS$","t":{"f":[{"h":"HtpQjbba","t":1,"a":"STYGCigAjt9fSXwkPSYxVA","k":"vN8A0kvxmC0:3BrxInvwc6gysUP-NmZ8Fg","p":"Ll5VkSJZ","ts":1770364718,"u":"vN8A0kvxmC0","i":0}]},"ou":"vN8A0kvxmC0"}],"w":"https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ","sn":"JcnE8wc8Xz0"})";
    std::string jsonMove =
        R"({"a":[{"a":"d","i":"fbtekrfnlb","st":"!G(<L_j","n":"i4om2BrJ","m":1,"ou":"vN8A0kvxmC0"},{"a":"t","i":"fbtekrfnlb","t":{"f":[{"h":"i4om2BrJ","p":"HtpQjbba","u":"vN8A0kvxmC0","t":1,"a":"k8mDAq0-TfskLMyMqcxssQ","k":"vN8A0kvxmC0:cXtlV5RG0QHpBxl-sZWQxA","ts":1770364711}],"u":[{"u":"vN8A0kvxmC0","m":"jye+test3@mega.co.nz","m2":["jye+test3@mega.co.nz"],"pubk":"CADLwMeFsNUFHY5vIHrDF73XMk6lZvRPQlhh67NGgg8WdiZF7vtM4HrCoftYvQEFLM-JF498dEFLNo9f76KydgWSdl_IEFT4KpnuexvJpfwI0eyJRzU1Wt5wegWJw9WPEKxpHGP91VllLPWB31X2FH48angXw9Mf_3nyjNh-q83tMEbT5XEWuL3KqiMdP80XeqJk3TOuCcaSmq5tzj84rzc1sNnsoNkgYPYuGjOqzA8VzURVU6Tp7BV2eJm0x68dCwoJMHiZVNor1fz0I-iB1Vj2Hurr1NmIGLP9AEJOSKE1iwBSmseTOci3KaLcMrwy1rQMx8r9y-KV5eBnt7kPRSCdACAAAAEB","+puCu255":"dLFE7BT0zCf3Q2f0EXVGRUZRtVuY0J_TPZVMkhAI3Hs","+puEd255":"owpFu55NPN4Y93GX8-iDVcbmktEvd-LcYtgMJUILLxU","+sigCu255":"AAAAAGj5nvcOQZQogXSCNe7RkRLP21nkWcr3D3zx2zY0xC0BqY5_Ny_O1JMaN95YtbkO8dgcYFGiHaW1CWnu9njDcGtnKm8A","+sigPubk":"AAAAAGj5nvgwEi2YXMbNhHdsV1IDYs62RzF1p60M4PkVxXx68HGqF7nBQSg69KUbh_yydf36nneGFMm-7UnKr_yZyFei2wsD"}]},"ou":"vN8A0kvxmC0"}],"w":"https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ","sn":"YydQMr-woLo"})";

    scStreamingParser->init();

    // Create 2 folder
    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        jsonCreate,
                        "https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ",
                        "JcnE8wc8Xz0");

    // Clear for next process
    scStreamingParser->clear();

    // Move one folder to another
    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        jsonMove,
                        "https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ",
                        "YydQMr-woLo");
}

TEST_F(ScStreamingParserTest, ProcessPacketTreeByPutNewVersion)
{
    initNodes();

    std::string jsonCreate =
        R"({"a":[{"a":"t","st":"!G(P`#}","t":{"f":[{"h":"r14EgLRY","t":0,"a":"ZLzvJzDefwAGc8lePmEWhU0fO8ET129XDWyA92FcyDw49VI1I-Zi5oB0Fi7lsCo7T4qjpMqT7EfPpgPNs0P6Fw","k":"vN8A0kvxmC0:ypiDHHhWkrbulm5UUMFy3gODiHsoBIZ37wEbzSSQZGU","p":"Ll5VkSJZ","ts":1770366829,"u":"vN8A0kvxmC0","s":0,"i":0}]},"ou":"vN8A0kvxmC0"}],"w":"https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ","sn":"lUNj6w_irSk"})";
    std::string jsonCreateVer =
        R"({"a":[{"a":"d","st":"!G(Utm1","n":"r14EgLRY","m":1,"v":1,"ou":"vN8A0kvxmC0"},{"a":"t","st":"!G(Utm1","t":{"f":[{"h":"G0wgRQhS","t":0,"a":"PzbbN2J1r_-j_rAq5raGC4SHahy1FZH11tPQbW6SszI94wBAL0uyWgiDtc4zvXThFHjQAlSvirhem-RYH6piKA","k":"vN8A0kvxmC0:vf0AQehM8yISzlRdHhPqZ-icwiL0RZJZJqFoLI6SM0c","p":"Ll5VkSJZ","ts":1770367362,"u":"vN8A0kvxmC0","s":2,"i":0}],"f2":[{"h":"r14EgLRY","p":"G0wgRQhS","u":"vN8A0kvxmC0","t":0,"a":"ZLzvJzDefwAGc8lePmEWhU0fO8ET129XDWyA92FcyDw49VI1I-Zi5oB0Fi7lsCo7T4qjpMqT7EfPpgPNs0P6Fw","k":"vN8A0kvxmC0:ypiDHHhWkrbulm5UUMFy3gODiHsoBIZ37wEbzSSQZGU","s":0,"ts":1770366829}]},"ou":"vN8A0kvxmC0"}],"w":"https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ","sn":"a7JBKiUvv5Y"})";

    scStreamingParser->init();

    // Create a file
    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        jsonCreate,
                        "https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ",
                        "lUNj6w_irSk");

    // Clear for next process
    scStreamingParser->clear();

    // Create a new version for the file
    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        jsonCreateVer,
                        "https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ",
                        "a7JBKiUvv5Y");
}

TEST_F(ScStreamingParserTest, ProcessPacketTreeByCmdPutNodes)
{
    std::shared_ptr<Node> rootNode = initNodes();

    std::string jsonRes = R"([["!G(P`#}",{"e":[],"fh":["G0wgRQhS:5GXvAZORkho"]}]])";
    std::string jsonCreate =
        R"({"a":[{"a":"t","st":"!G(P`#}","t":{"f":[{"h":"r14EgLRY","t":0,"a":"ZLzvJzDefwAGc8lePmEWhU0fO8ET129XDWyA92FcyDw49VI1I-Zi5oB0Fi7lsCo7T4qjpMqT7EfPpgPNs0P6Fw","k":"vN8A0kvxmC0:ypiDHHhWkrbulm5UUMFy3gODiHsoBIZ37wEbzSSQZGU","p":"Ll5VkSJZ","ts":1770366829,"u":"vN8A0kvxmC0","s":0,"i":0}]},"ou":"vN8A0kvxmC0"}],"w":"https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ","sn":"lUNj6w_irSk"})";

    scStreamingParser->init();

    client->scsn.setScsn(0xFFFFFFFF);

    // Simulate command PutNodes
    vector<NewNode> dummyNodeList(1);
    NewNode& dummyNode = dummyNodeList.front();
    dummyNode.source = NEW_NODE;
    dummyNode.type = FILENODE;
    dummyNode.nodehandle = 10;
    dummyNode.attrstring = std::make_unique<std::string>("{}");
    dummyNode.nodekey.resize(FILENODEKEYLENGTH);
    PrnGen rng;
    rng.genblock(reinterpret_cast<byte*>(dummyNode.nodekey.data()), FILENODEKEYLENGTH);

    byte masterKey[SymmCipher::KEYLENGTH];
    rng.genblock(masterKey, SymmCipher::KEYLENGTH);
    client->key.setkey(masterKey);

    client->putnodes(rootNode->nodeHandle(),
                     VersioningOption::ClaimOldVersion,
                     std::move(dummyNodeList),
                     nullptr,
                     0,
                     false);

    bool dummy1;
    string dummy2;
    client->reqs.serverrequest(dummy1, client.get(), dummy2);
    client->reqs.serverresponse(std::move(jsonRes), client.get());

    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        jsonCreate,
                        "https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ",
                        "lUNj6w_irSk");
}

TEST_F(ScStreamingParserTest, ProcessPacketTreeByCmdPutNodesFailure)
{
    std::shared_ptr<Node> rootNode = initNodes();

    std::string jsonRes = R"([["!G(P`#}",{"e":[-1],"fh":["G0wgRQhS:5GXvAZORkho"]}]])";
    std::string jsonCreate =
        R"({"a":[{"a":"t","st":"!G(P`#}","t":{"f":[]},"ou":"vN8A0kvxmC0"}],"w":"https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ","sn":"lUNj6w_irSk"})";

    scStreamingParser->init();

    client->scsn.setScsn(0xFFFFFFFF);

    // Simulate command PutNodes
    vector<NewNode> dummyNodeList(1);
    NewNode& dummyNode = dummyNodeList.front();
    dummyNode.source = NEW_NODE;
    dummyNode.type = FILENODE;
    dummyNode.nodehandle = 10;
    dummyNode.attrstring = std::make_unique<std::string>("{}");
    dummyNode.nodekey.resize(FILENODEKEYLENGTH);
    PrnGen rng;
    rng.genblock(reinterpret_cast<byte*>(dummyNode.nodekey.data()), FILENODEKEYLENGTH);

    byte masterKey[SymmCipher::KEYLENGTH];
    rng.genblock(masterKey, SymmCipher::KEYLENGTH);
    client->key.setkey(masterKey);

    client->putnodes(rootNode->nodeHandle(),
                     VersioningOption::ClaimOldVersion,
                     std::move(dummyNodeList),
                     nullptr,
                     0,
                     false);

    bool dummy1;
    string dummy2;
    client->reqs.serverrequest(dummy1, client.get(), dummy2);
    client->reqs.serverresponse(std::move(jsonRes), client.get());

    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        jsonCreate,
                        "https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ",
                        "lUNj6w_irSk");
}

TEST_F(ScStreamingParserTest, ProcessPacketTreeMixOthers)
{
    initNodes();

    std::string json =
        R"({"a":[{"a":"t","st":"!G(<Cq+","t":{"f":[{"h":"i4om2BrJ","t":1,"a":"k8mDAq0-TfskLMyMqcxssQ","k":"vN8A0kvxmC0:cXtlV5RG0QHpBxl-sZWQxA","p":"Ll5VkSJZ","ts":1770364711,"u":"vN8A0kvxmC0","i":0}]},"ou":"vN8A0kvxmC0"}, {"a":"s2","st":"!?>Qzzr","n":"hH4SgJJZ","o":"UH4jcqAYP8o","ok":"AAAAAAAAAAAAAAAAAAAAAA","ha":"AAAAAAAAAAAAAAAAAAAAAA","u":"JoPrmG9jv98","r":1,"ts":1761823710,"p":"ShhP2ZnftRQ","op":1}],"w":"https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ","sn":"JcnE8wc8Xz0"})";

    scStreamingParser->init();

    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        json,
                        "https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ",
                        "JcnE8wc8Xz0");

    // Clear for next process
    scStreamingParser->clear();

    // Verify "t" packet without "ou" field
    std::string jsonWithoutOu =
        R"({"a":[{"a":"t","st":"!G(<Cq+","t":{"f":[{"h":"r14EgLRY","t":1,"a":"k8mDAq0-TfskLMyMqcxssQ","k":"vN8A0kvxmC0:cXtlV5RG0QHpBxl-sZWQxA","p":"Ll5VkSJZ","ts":1770364711,"u":"vN8A0kvxmC0","i":0}]}}, {"a":"s2","st":"!?>Qzzr","n":"hH4SgJJZ","o":"UH4jcqAYP8o","ok":"AAAAAAAAAAAAAAAAAAAAAA","ha":"AAAAAAAAAAAAAAAAAAAAAA","u":"JoPrmG9jv98","r":1,"ts":1761823710,"p":"ShhP2ZnftRQ","op":1}],"w":"https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ","sn":"JcnE8wc8Xz0"})";

    testFinishedProcess(scStreamingParser,
                        *client.get(),
                        jsonWithoutOu,
                        "https://g.api.mega.co.nz/wsc/WS_LMPJFp8VlKndvCVoSBQ",
                        "JcnE8wc8Xz0");
}
}
