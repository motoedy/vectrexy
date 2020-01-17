#pragma once

#include "core/Base.h"
#include "core/Pimpl.h"

class TcpServer {
public:
    TcpServer();
    ~TcpServer();

    void Open(uint16_t port);
    void Close();
    bool TryAccept();
    bool Connected() const;

    // Checks if data is available on the socket to receive. If timeout is 0, performs a quick check, otherwise it checks for timeout milliseconds.
    bool ReceiveDataAvailable(uint32_t timeoutMS = 0) const;

    int Send(const void* data, int len);
    int Receive(void* data, int maxlen);

    template <typename T>
    bool Send(const T& value) {
        auto bytesSent = Send(&value, sizeof(T));
        return bytesSent <= sizeof(T);
    }

    template <typename T>
    bool Receive(T& value) {
        auto size = Receive(&value, sizeof(T));
        return size == sizeof(T);
    }

private:
    pimpl::Pimpl<class TcpServerImpl, 128> m_impl;
};

class TcpClient {
public:
    TcpClient();
    ~TcpClient();

    void Open(const char* ipAddress, uint16_t port);
    void Close();

    int Send(const void* data, int len);
    int Receive(void* data, int maxlen);

    template <typename T>
    bool Send(const T& value) {
        auto bytesSent = Send(&value, sizeof(T));
        return bytesSent <= sizeof(T);
    }

    template <typename T>
    bool Receive(T& value) {
        auto size = Receive(&value, sizeof(T));
        return size == sizeof(T);
    }

private:
    pimpl::Pimpl<class TcpClientImpl, 128> m_impl;
};
