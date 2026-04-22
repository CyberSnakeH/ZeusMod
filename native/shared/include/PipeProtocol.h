#pragma once
#include "SharedTypes.h"
#include <functional>
#include <atomic>
#include <thread>

namespace IcarusMod {

class PipeServer {
public:
    using MessageHandler = std::function<void(const PipeMessage&, PipeMessage&)>;

    PipeServer() = default;
    ~PipeServer();

    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    bool Start(MessageHandler handler);
    void Stop();
    bool SendMessage(const PipeMessage& msg);
    bool IsConnected() const { return m_connected.load(); }

private:
    void ServerThread();

    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_connected{ false };
    std::thread m_thread;
    MessageHandler m_handler;
};

class PipeClient {
public:
    PipeClient() = default;
    ~PipeClient();

    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    bool Connect();
    void Disconnect();
    bool SendMessage(const PipeMessage& msg);
    bool ReceiveMessage(PipeMessage& msg);
    bool IsConnected() const { return m_connected.load(); }

    bool SendAndReceive(const PipeMessage& request, PipeMessage& response);

private:
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    std::atomic<bool> m_connected{ false };
};

} // namespace IcarusMod
