/**
 * @file examples/simple_client/simple_client.cpp
 * @brief Example app
 *
 * (c) 2013-2025 by Mega Limited, Auckland, New Zealand
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
#include <mega/log_level.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <megaapi.h>
#include <mutex>
#include <string>
#include <thread>
#include <time.h>

// Credentials are read from the MEGA_EMAIL and MEGA_PWD environment variables.
// Get your APP_KEY for free at https://mega.io/developers#source-code
#define APP_KEY "9gETCbhB"
#define USER_AGENT "Simple-Client example app"

using namespace mega;

namespace
{

class SimpleClientLogger: public MegaLogger
{
public:
    SimpleClientLogger(const std::string& path, bool logToStdout):
        mStream(path, std::ios::app),
        mLogToStdout(logToStdout)
    {
        if (!mStream)
        {
            std::cerr << "SimpleClientLogger: failed to open " << path << std::endl;
        }
    }

    void log(const char* /*time*/,
             int loglevel,
             const char* source,
             const char* message
#ifdef ENABLE_LOG_PERFORMANCE
             ,
             const char** directMessages = nullptr,
             size_t* directMessagesSizes = nullptr,
             int numberMessages = 0
#endif
             ) override
    {
        std::lock_guard<std::mutex> lk(mMutex);

        // Build HH:MM:SS.mmm timestamp ourselves to match MegaCLILogger's format.
        using namespace std::chrono;
        const auto now = system_clock::now();
        const auto t = system_clock::to_time_t(now);
        const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
        char hms[16];
        std::strftime(hms, sizeof(hms), "%H:%M:%S", std::localtime(&t));
        char timestamp[32];
        std::snprintf(timestamp, sizeof(timestamp), "%s.%03lld", hms, static_cast<long long>(ms));

        // Left-pad the level name to 7 chars (longest is "WARNING"/"VERBOSE")
        // so log columns line up.
        char level[8];
        std::snprintf(level, sizeof(level), "%-7s", toString(static_cast<LogLevel>(loglevel)));

        std::string s;
        s.reserve(1024);
        s += '[';
        s += timestamp;
        s += "] ";
        s += level;
        s += " | ";
        if (message)
            s += message;
#ifdef ENABLE_LOG_PERFORMANCE
        for (int i = 0; i < numberMessages; ++i)
        {
            s.append(directMessages[i], directMessagesSizes[i]);
        }
#endif
        if (source && *source != '\0')
        {
            s += " [";
            s += source;
            s += "]";
        }
        s += '\n';

        auto write = [&](std::ostream& os)
        {
            os << s;
        };

        if (mStream)
        {
            write(mStream);
            mStream.flush();
        }
        if (mLogToStdout)
        {
            write(std::cout);
        }
    }

private:
    std::ofstream mStream;
    bool mLogToStdout;
    std::mutex mMutex;
};

} // namespace

class MyListener: public MegaListener
{
public:
    bool finished{};

    virtual void onRequestFinish(MegaApi* api, MegaRequest* request, MegaError* e)
    {
        if (e->getErrorCode() != MegaError::API_OK)
        {
            finished = true;
            return;
        }

        switch (request->getType())
        {
            case MegaRequest::TYPE_LOGIN:
            {
                api->fetchNodes();
                break;
            }
            case MegaRequest::TYPE_FETCH_NODES:
            {
                std::cout << "***** Showing files/folders in the root folder:" << std::endl;
                MegaNode* root = api->getRootNode();
                MegaNodeList* list = api->getChildren(root);

                for (int i = 0; i < list->size(); i++)
                {
                    MegaNode* node = list->get(i);
                    if (node->isFile())
                        std::cout << "*****   File:   ";
                    else
                        std::cout << "*****   Folder: ";

                    std::cout << node->getName() << std::endl;
                }
                std::cout << "***** Done" << std::endl;

                delete list;

                std::cout << "***** Uploading the image MEGA.png" << std::endl;

                MegaUploadOptions uploadOptions;
                uploadOptions.mtime = 0;
                api->startUpload(std::string{"MEGA.png"}, root, nullptr, &uploadOptions, nullptr);

                delete root;

                break;
            }
            default:
                break;
        }
    }

    // Currently, this callback is only valid for the request fetchNodes()
    virtual void onRequestUpdate(MegaApi*, MegaRequest* request)
    {
        std::cout << "***** Loading filesystem " << request->getTransferredBytes() << " / "
                  << request->getTotalBytes() << std::endl;
    }

    virtual void onRequestTemporaryError(MegaApi*, MegaRequest*, MegaError* error)
    {
        std::cout << "***** Temporary error in request: " << error->getErrorString() << std::endl;
    }

    virtual void onTransferFinish(MegaApi*, MegaTransfer*, MegaError* error)
    {
        if (error->getErrorCode())
        {
            std::cout << "***** Transfer finished with error: " << error->getErrorString()
                      << std::endl;
        }
        else
        {
            std::cout << "***** Transfer finished OK" << std::endl;
        }

        finished = true;
    }

    virtual void onTransferUpdate(MegaApi*, MegaTransfer* transfer)
    {
        std::cout << "***** Transfer progress: " << transfer->getTransferredBytes() << "/"
                  << transfer->getTotalBytes() << std::endl;
    }

    virtual void onTransferTemporaryError(MegaApi*, MegaTransfer*, MegaError* error)
    {
        std::cout << "***** Temporary error in transfer: " << error->getErrorString() << std::endl;
    }

    virtual void onUsersUpdate(MegaApi*, MegaUserList* users)
    {
        if (users == NULL)
        {
            // Full account reload
            return;
        }
        std::cout << "***** There are " << users->size() << " new or updated users in your account"
                  << std::endl;
    }

    virtual void onNodesUpdate(MegaApi*, MegaNodeList* nodes)
    {
        if (nodes == NULL)
        {
            // Full account reload
            return;
        }

        std::cout << "***** There are " << nodes->size() << " new or updated node/s in your account"
                  << std::endl;
    }

    virtual void onSetsUpdate(MegaApi*, MegaSetList* sets)
    {
        if (sets)
        {
            std::cout << "***** There are " << sets->size()
                      << " new or updated Set/s in your account" << std::endl;
        }
    }

    virtual void onSetElementsUpdate(MegaApi*, MegaSetElementList* elements)
    {
        if (elements)
        {
            std::cout << "***** There are " << elements->size()
                      << " new or updated Set-Element/s in your account" << std::endl;
        }
    }
};

std::string displayTime(time_t t)
{
    char timebuf[32];
    strftime(timebuf, sizeof timebuf, "%c", localtime(&t));
    return timebuf;
}

int main()
{
    // Route SDK logs to a file. Override the path with MEGA_LOG_FILE.
    const char* envLogPath = std::getenv("MEGA_LOG_FILE");
    const std::string logFilePath = (envLogPath && *envLogPath) ? envLogPath : "simple_client.log";

    // Tee logs to stdout by default. Disable with MEGA_LOG_STDOUT=0 (or =false).
    const char* envStdout = std::getenv("MEGA_LOG_STDOUT");
    const bool logToStdout = !envStdout || *envStdout == '\0' ||
                             (envStdout[0] != '0' && envStdout[0] != 'f' && envStdout[0] != 'F');

    static SimpleClientLogger fileLogger(logFilePath, logToStdout);
    MegaApi::addLoggerObject(&fileLogger, /*singleExclusiveLogger=*/true);
    MegaApi::setLogLevel(MegaApi::LOG_LEVEL_INFO);

    // Check the documentation of MegaApi to know how to enable local caching
    MegaApi* megaApi = new MegaApi(APP_KEY, ".", USER_AGENT);

    MyListener listener;

    // Listener to receive information about all request and transfers
    // It is also possible to register a different listener per request/transfer
    megaApi->addListener(&listener);

    const char* envEmail = std::getenv("MEGA_EMAIL");
    const char* envPassword = std::getenv("MEGA_PWD");
    if (!envEmail || !*envEmail || !envPassword || !*envPassword)
    {
        std::cerr << "Set the MEGA_EMAIL and MEGA_PWD environment variables before running."
                  << std::endl;
        return 1;
    }

    // Login. You can get the result in the onRequestFinish callback of your listener
    megaApi->login(envEmail, envPassword);

    // You can use the main thread to show a GUI or anything else. MegaApi runs in a background
    // thread.
    while (!listener.finished)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    // Add code here to exercise MegaApi

#ifdef HAVE_LIBUV
    std::cout << "Do you want to enable the local HTTP server (y/n)?" << std::endl;
    int c = getchar();
    if (c == 'y' || c == 'Y')
    {
        megaApi->httpServerStart();
        megaApi->httpServerSetRestrictedMode(MegaApi::HTTP_SERVER_ALLOW_ALL);
        megaApi->httpServerEnableFileServer(true);
        megaApi->httpServerEnableFolderServer(true);
        std::cout << "You can browse your account now! http://127.0.0.1:4443/" << std::endl;
    }
#endif

    std::cout << "Press Enter to exit the app..." << std::endl;
    getchar();

#ifdef HAVE_LIBUV
    megaApi->httpServerStop();
#endif

    megaApi->removeListener(&listener);
    delete megaApi;
    MegaApi::removeLoggerObject(&fileLogger);
    return 0;
}
