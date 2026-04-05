#include "PipeProtocol.h"

namespace IcarusMod {

// ============================================================================
// PipeServer
// ============================================================================

PipeServer::~PipeServer() {
    Stop();
}

bool PipeServer::Start(MessageHandler handler) {
    if (m_running.load()) return false;
    m_handler = std::move(handler);
    m_running.store(true);
    m_thread = std::thread(&PipeServer::ServerThread, this);
    return true;
}

void PipeServer::Stop() {
    m_running.store(false);
    if (m_pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(m_pipe);
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_connected.store(false);
}

bool PipeServer::SendMessage(const PipeMessage& msg) {
    if (m_pipe == INVALID_HANDLE_VALUE || !m_connected.load()) return false;
    DWORD written = 0;
    return WriteFile(m_pipe, &msg, sizeof(msg), &written, nullptr) && written == sizeof(msg);
}

void PipeServer::ServerThread() {
    m_pipe = CreateNamedPipeW(
        PIPE_NAME,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,
        sizeof(PipeMessage) * 16,
        sizeof(PipeMessage) * 16,
        0,
        nullptr
    );

    if (m_pipe == INVALID_HANDLE_VALUE) return;

    while (m_running.load()) {
        if (ConnectNamedPipe(m_pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            m_connected.store(true);

            while (m_running.load()) {
                PipeMessage request{};
                DWORD bytesRead = 0;

                if (!ReadFile(m_pipe, &request, sizeof(request), &bytesRead, nullptr) ||
                    bytesRead != sizeof(request)) {
                    break;
                }

                if (m_handler) {
                    PipeMessage response{};
                    m_handler(request, response);
                    DWORD written = 0;
                    WriteFile(m_pipe, &response, sizeof(response), &written, nullptr);
                }
            }

            m_connected.store(false);
            DisconnectNamedPipe(m_pipe);
        }
        else {
            Sleep(100);
        }
    }
}

// ============================================================================
// PipeClient
// ============================================================================

PipeClient::~PipeClient() {
    Disconnect();
}

bool PipeClient::Connect() {
    if (m_connected.load()) return true;

    m_pipe = CreateFileW(
        PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );

    if (m_pipe == INVALID_HANDLE_VALUE) return false;

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(m_pipe, &mode, nullptr, nullptr)) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        return false;
    }

    m_connected.store(true);
    return true;
}

void PipeClient::Disconnect() {
    if (m_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
    m_connected.store(false);
}

bool PipeClient::SendMessage(const PipeMessage& msg) {
    if (!m_connected.load()) return false;
    DWORD written = 0;
    return WriteFile(m_pipe, &msg, sizeof(msg), &written, nullptr) && written == sizeof(msg);
}

bool PipeClient::ReceiveMessage(PipeMessage& msg) {
    if (!m_connected.load()) return false;
    DWORD bytesRead = 0;
    return ReadFile(m_pipe, &msg, sizeof(msg), &bytesRead, nullptr) && bytesRead == sizeof(msg);
}

bool PipeClient::SendAndReceive(const PipeMessage& request, PipeMessage& response) {
    return SendMessage(request) && ReceiveMessage(response);
}

} // namespace IcarusMod
