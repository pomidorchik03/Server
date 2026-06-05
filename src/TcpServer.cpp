#include "../header/TcpServer.h"
#include <iostream>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <sstream>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    using SocketType = SOCKET;
    #define CLOSE_SOCKET(s) closesocket(s)
    #define INVALID_SOCKET_VAL INVALID_SOCKET
    #undef SendMessage
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    using SocketType = int;
    #define CLOSE_SOCKET(s) close(s)
    #define INVALID_SOCKET_VAL -1
#endif

struct ClientSession
{
    SocketType sock;
    std::thread th;
};

class TcpServerImpl
{
public:
    SocketType listen_sock_ = INVALID_SOCKET_VAL;
    std::atomic<bool> is_running_{false};
    std::thread accept_thread;

    std::map<int, ClientSession> clients;
    std::mutex clients_mutex;
    std::map<std::string, int> token_to_id;
    int next_client_id = 1;

    TcpServer::MessageCallback on_message;
    TcpServer::DisconnectCallback on_disconnect;

    TcpServerImpl()
    {
#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    }
    ~TcpServerImpl()
    {
        Stop();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void Stop()
    {
        if (!is_running_)
        {
            return;
        }
        is_running_ = false;
        on_message = nullptr;
        on_disconnect = nullptr;

        if (listen_sock_ != INVALID_SOCKET_VAL)
        {
            CLOSE_SOCKET(listen_sock_);
            listen_sock_ = INVALID_SOCKET_VAL;
        }

        if (accept_thread.joinable())
        {
            accept_thread.join();
        }

        std::vector<SocketType> socket_to_close;
        std::vector<std::thread> threads_to_join;

        {    
            std::lock_guard<std::mutex> lock(clients_mutex);
            for (auto& [id, session] : clients)
            {
                socket_to_close.push_back(session.sock);
                if (session.th.joinable())
                {
                    threads_to_join.push_back(std::move(session.th));
                }
            }
        }

        for (auto sock : socket_to_close)
        {
            CLOSE_SOCKET(sock);
        }

        for (auto& th : threads_to_join)
        {
            th.join();
        }
        clients.clear();
    }
};

TcpServer::TcpServer() : impl_(std::make_unique<TcpServerImpl>()) {}
TcpServer::~TcpServer() 
{
    Stop();
}

void TcpServer::SetMessageCallback(MessageCallback cb)
{
    impl_->on_message = cb;
}
void TcpServer::SetDisconnectCallback(DisconnectCallback cb)
{
    impl_->on_disconnect = cb;
}

void ClientThread(TcpServerImpl* impl, int temp_id, SocketType clientSock)
{
    std::vector<uint8_t> buffer(4096);
    
    int bytes_received = recv(clientSock, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
    if (bytes_received <= 0)
    {
        CLOSE_SOCKET(clientSock);
        std::lock_guard<std::mutex> lock(impl->clients_mutex);
        impl->clients.erase(temp_id);
        return;
    }

    std::string player_token(buffer.begin(), buffer.begin() + bytes_received);
    int final_id = temp_id;

    {
        std::lock_guard<std::mutex> lock(impl->clients_mutex);

        if (impl->token_to_id.count(player_token))
        {
            final_id = impl->token_to_id[player_token];
            std::cout << "[TcpServer::ClientThread] Player " << player_token << " reconnected. Recovering id " << final_id << ".\n";

            auto it = impl->clients.find(final_id);
            if (it != impl->clients.end())
            {
                CLOSE_SOCKET(it->second.sock);
                if (it->second.th.joinable())
                {
                    it->second.th.detach();
                }
            }

            impl->clients[final_id].sock = clientSock;
            impl->clients[final_id].th = std::move(impl->clients[temp_id].th);
            impl->clients.erase(temp_id);
        }
        else
        {
            impl->token_to_id[player_token] = final_id; 
            std::cout << "[TcpServer::ClientThread] New player " << player_token << " with id: " << final_id << ".\n";
        }
    }

    while(impl->is_running_)
    {
        int bytes_received = recv(clientSock, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);

        if (bytes_received > 0)
        {
            std::vector<uint8_t> receiced_data(buffer.begin(), buffer.begin() + bytes_received);
            if (impl->is_running_ && impl->on_message)
            {
                impl->on_message(final_id, receiced_data);
            }
        }
        else
        {
            std::cerr << "[TcpServer::ClientThread] Player " << final_id << " disconnected.\n";
            break;
        }
    }

    CLOSE_SOCKET(clientSock);

    if (impl->on_disconnect)
    {
        impl->on_disconnect(final_id);
    }

    {
        std::lock_guard<std::mutex> lock(impl->clients_mutex);
        auto it = impl->clients.find(final_id);
        if (it != impl->clients.end())
        {
            if (it->second.th.joinable())
            {
                it->second.th.detach();
            }
            impl->clients.erase(it);
        }
    }


}

void AcceptPlayer(TcpServerImpl* impl)
{
    while(impl->is_running_)
    {
        sockaddr_in client_addr{};
#ifdef _WIN32
        int addr_len = sizeof(client_addr);
#else
        socklen_t addr_len = sizeof(client_addr);
#endif
        SocketType client_sock = accept(impl->listen_sock_, (struct sockaddr*)&client_addr, &addr_len);

        if (client_sock == INVALID_SOCKET_VAL)
        {
            if (!impl->is_running_)
            {
                break;
            }
            continue;
        }

#ifdef _WIN32
        DWORD timeout = 50;
        setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 50000;
        setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif

        std::lock_guard<std::mutex> lock(impl->clients_mutex);
        int temp_id = impl->next_client_id++;

        std::cout << "[TcpServer::AcceptPlayer] New player connect with id: " << temp_id << ".\n";

        impl->clients[temp_id].sock = client_sock;
        impl->clients[temp_id].th = std::thread(ClientThread, impl, temp_id, client_sock);
        
    }
}

bool TcpServer::Start(int port)
{
    impl_->listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->listen_sock_ == INVALID_SOCKET_VAL) 
    {
#ifdef _WIN32
        std::cerr << "[TcpServer::Start] Error code: " << WSAGetLastError() << "\n";
#else   
        std::clog << "[TcpServer::Start] errno: " << errno << "\n";
#endif
        return false;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if(bind(impl_->listen_sock_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    {
#ifdef _WIN32
        std::cerr << "[TcpServer::Start] Error code: " << WSAGetLastError() << "\n";
#else   
        std::clog << "[TcpServer::Start] errno: " << errno << "\n";
#endif
        CLOSE_SOCKET(impl_->listen_sock_);
        return false;
    }
    if (listen(impl_->listen_sock_, SOMAXCONN) < 0)
    {
        CLOSE_SOCKET(impl_->listen_sock_);
        return false;
    }

    impl_->is_running_ = true;
    impl_->accept_thread = std::thread(AcceptPlayer, impl_.get());
    return true;
}

bool TcpServer::SendData(int client_id, const std::vector<uint8_t>& data)
{
    SocketType sock = INVALID_SOCKET_VAL;

    {
        std::lock_guard<std::mutex> lock(impl_->clients_mutex);
        auto it = impl_->clients.find(client_id);
        if (it == impl_->clients.end()) 
        {
            return false;
        }
        sock = it->second.sock;
    }

    size_t total_send = 0;
    size_t bytes_left = data.size();
    while (total_send < data.size())
    {
        int n = send(sock, reinterpret_cast<const char*>(data.data()) + total_send, static_cast<int>(bytes_left), 0);
        if (n <= 0)
        {
            return false;
        }
        total_send += n;
        bytes_left -= n;
    }
    return true;
    
}

void TcpServer::BroadcastData(const std::vector<uint8_t>& data)
{
    std::vector<SocketType> sockets_to_send;

    {   
         std::lock_guard<std::mutex> lock(impl_->clients_mutex);
        for (auto& [id, session] : impl_->clients)
        {
            sockets_to_send.push_back(session.sock);
        }
    }

    for (auto sock : sockets_to_send)
    {
#ifdef _WIN32
        int result = send(sock, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0);
#else
        int result = send(sock, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), MSG_NOSIGNAL);
#endif
        if (result < 0)
        {
            std::cout << "[TcpServer::BroadcastData] Socket timed out(50ms).\n";
        }
    }
}

void TcpServer::Stop()
{
    impl_->Stop();
}

std::string TcpServer::GetTokenPlayer(int client_id)
{
    std::lock_guard<std::mutex> lock(impl_->clients_mutex);

    for (const auto& [name, id] : impl_->token_to_id)
    {
        if (id == client_id)
        {
            return name;
        }
    }

    return "Player" + std::to_string(client_id);
}