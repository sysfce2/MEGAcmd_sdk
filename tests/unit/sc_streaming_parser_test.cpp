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
    sessionId = client->sessionid;
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
        R"({"a":[{"a":"ua","st":"!?>VwgM","u":"k-N5-o6LLkE","ua":["^!stbmp"],"v":["Gt-009Sr-XA"]}],"w":"https://g.api.)";

    scStreamingParser->init();

    // Process the incomplete packet
    size_t consumed = (size_t)scStreamingParser->process(json.c_str());

    ASSERT_TRUE(consumed > 0);
    ASSERT_TRUE(scStreamingParser->hasStarted());
    ASSERT_FALSE(scStreamingParser->isFinished());
    ASSERT_FALSE(scStreamingParser->isFailed());
    ASSERT_FALSE(scStreamingParser->isPaused());
    ASSERT_TRUE(client->scnotifyurl.empty());
    ASSERT_FALSE(client->scsn.ready());

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

}