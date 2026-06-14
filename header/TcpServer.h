#pragma once

#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <cstdint>

class TcpServerImpl;

class TcpServer
{
private:
    std::unique_ptr<TcpServerImpl> impl_;
public:
    using MessageCallback = std::function<void(int client_id, const std::vector<uint8_t>& data)>;
    using DisconnectCallback = std::function<void(int client_id)>;

    TcpServer();
    ~TcpServer();

    bool Start(int port);
    void Stop();

    bool SendData(int client_id, const std::vector<uint8_t>& data);

    void BroadcastData(const std::vector<uint8_t>& data);

    void SetMessageCallback(MessageCallback cb);
    void SetDisconnectCallback(DisconnectCallback cb);

    std::string GetWANIP();
    std::string GetLANIP();

    std::string GetTokenPlayer(int client_id);
};
