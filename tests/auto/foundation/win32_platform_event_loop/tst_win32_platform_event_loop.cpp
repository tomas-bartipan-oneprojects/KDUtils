/*
  This file is part of KDUtils.

  SPDX-FileCopyrightText: 2018-2023 Klarälvdalens Datakonsult AB, a KDAB Group company <info@kdab.com>
  Author: Paul Lemire <paul.lemire@kdab.com>

  SPDX-License-Identifier: MIT

  Contact KDAB at <info@kdab.com> for commercial licensing options.
*/

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <KDFoundation/platform/win32/win32_platform_event_loop.h>
#include <KDFoundation/object.h>
#include <KDFoundation/file_descriptor_notifier.h>
#include <KDFoundation/postman.h>

#include <KDFoundation/logging.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>

#include <WinSock2.h>
#include <WS2tcpip.h>

using namespace KDFoundation;

static_assert(std::is_destructible<Win32PlatformEventLoop>{});
static_assert(std::is_default_constructible<Win32PlatformEventLoop>{});
static_assert(!std::is_copy_constructible<Win32PlatformEventLoop>{});
static_assert(!std::is_copy_assignable<Win32PlatformEventLoop>{});
static_assert(!std::is_move_constructible<Win32PlatformEventLoop>{});
static_assert(!std::is_move_assignable<Win32PlatformEventLoop>{});

TEST_CASE("Wait for events")
{
    spdlog::set_level(spdlog::level::debug);

    SUBCASE("can poll for events (0ms timeout)")
    {
        Win32PlatformEventLoop loop;
        loop.waitForEvents(0);
    }

    SUBCASE("can wait for events (100 ms timeout)")
    {
        Win32PlatformEventLoop loop;
        loop.waitForEvents(100);
    }

    SUBCASE("can wake up by calling wakeUp from another thread")
    {
        Win32PlatformEventLoop loop;

        // Spawn a thread to wake up the event loop
        std::mutex mutex;
        std::condition_variable cond;
        bool ready = false;
        auto callWakeUp = [&mutex, &cond, &ready, &loop]() {
            spdlog::info("Launched helper thread");
            std::unique_lock lock(mutex);
            cond.wait(lock, [&ready] { return ready == true; });
            spdlog::info("Thread going to sleep before waking up event loop");

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            loop.wakeUp();
        };
        std::thread t1(callWakeUp);

        // Kick the thread off
        {
            spdlog::info("Waking up helper thread");
            std::unique_lock lock(mutex);
            ready = true;
            cond.notify_all();
        }

        const auto startTime = std::chrono::steady_clock::now();
        loop.waitForEvents(10000);
        const auto endTime = std::chrono::steady_clock::now();

        auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        spdlog::info("elapsedTime = {}", elapsedTime);
        REQUIRE(elapsedTime < 10000);

        // Be nice!
        t1.join();
    }

    SUBCASE("can watch a win32 socket")
    {
        const short port = 1337;
        WSAData wsaData;

        Win32PlatformEventLoop loop;
        Postman postman;
        loop.setPostman(&postman);

        const auto ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
        REQUIRE(ret == 0);

        const std::string dataToSend("KDFoundation");
        std::string dataReceived;

        std::mutex mutex;
        std::condition_variable cond;
        bool ready = false;

        auto serverFunction = [&mutex, &cond, &ready, &dataToSend, &port]() {
            SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (serverSocket == INVALID_SOCKET) {
                spdlog::error("Cannot create server socket");
            }
            sockaddr_in ad;
            ad.sin_family = AF_INET;
            ad.sin_port = htons(port);
            ad.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            int ret = 0;
            ret = bind(serverSocket, reinterpret_cast<sockaddr *>(&ad), sizeof(ad));
            if (ret == SOCKET_ERROR) {
                spdlog::error("Bind error");
            }

            ret = listen(serverSocket, SOMAXCONN);
            if (ret == SOCKET_ERROR) {
                spdlog::error("Listen error");
            }

            // Send info to the client that we're ready
            {
                std::lock_guard lk(mutex);
                ready = true;
                cond.notify_all();
            }

            auto clientSocket = accept(serverSocket, NULL, NULL);
            closesocket(serverSocket);

            if (clientSocket != INVALID_SOCKET) {
                ret = send(clientSocket, dataToSend.c_str(), dataToSend.size() + 1, 0);
                closesocket(clientSocket);
            } else {
                spdlog::error("invalid socket from accept");
            }
        };
        // Start the server
        std::thread serverThread(serverFunction);

        // Wait until the server is ready for connection
        {
            std::unique_lock lk(mutex);
            cond.wait(lk, [&]() { return ready == true; });
        }

        SOCKET clientSock = INVALID_SOCKET;
        clientSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        // A notifier for testing deregistration
        int unregisteredCalls = 0;
        FileDescriptorNotifier unregistered(clientSock, FileDescriptorNotifier::NotificationType::Read);
        unregistered.triggered.connect([&unregisteredCalls] {
            unregisteredCalls++;
        });
        loop.registerNotifier(&unregistered);
        loop.unregisterNotifier(&unregistered);

        // Set up read notifier to receive the data
        FileDescriptorNotifier readNotifier(clientSock, FileDescriptorNotifier::NotificationType::Read);
        readNotifier.triggered.connect([&dataReceived](int fd) {
            char buf[128] = {};
            recv(fd, buf, 128, 0);
            dataReceived = std::string(buf);
        });
        loop.registerNotifier(&readNotifier);

        // A notifier for testing Write notification type
        FileDescriptorNotifier writeNotifier(clientSock, FileDescriptorNotifier::NotificationType::Write);
        int writeTriggered = 0;
        writeNotifier.triggered.connect([&writeTriggered]() {
            writeTriggered++;
        });
        loop.registerNotifier(&writeNotifier);

        sockaddr_in add;
        add.sin_family = AF_INET;
        add.sin_port = htons(port);
        add.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        // We don't check for success here because it intentionally returns an error. We're calling
        // connect on an async socket so we expect WOULDBLOCK.
        connect(clientSock, reinterpret_cast<sockaddr *>(&add), sizeof(add));
        REQUIRE(WSAGetLastError() == WSAEWOULDBLOCK);

        loop.waitForEvents(1000); // First we'll get FD_CONNECT on write notifier
        loop.waitForEvents(1000); // Then FD_WRITE, also on the write notifier
        loop.waitForEvents(1000); // And finally, FD_READ when data from the server is sent

        REQUIRE(unregisteredCalls == 0);
        REQUIRE(writeTriggered == 2);
        REQUIRE(dataReceived == dataToSend);

        closesocket(clientSock);
        WSACleanup();
        serverThread.join();
    }
}
