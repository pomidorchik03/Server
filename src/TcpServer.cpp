#include "../header/TcpServer.h"
#include <iostream>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <sstream>

#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "iphlpapi.lib")
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

    int upnp_mapped_port_ = -1;
    std::string upnp_control_url_ = "";
    std::string upnp_service_type_ = "";

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

    void TryMapPortUPnP(int port)
    {
        int error = 0;

        struct UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
        if (!devlist)
        {
            std::cerr << "[UPnP] Error: " << error << std::endl;
            return;
        }

        struct UPNPUrls urls;
        struct IGDdatas data;
        char lanaddr[64] = {0};
        char wanaddr[64] = {0};

        int status = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr), wanaddr, sizeof(wanaddr));
        freeUPNPDevlist(devlist);

        if (status != 1 && status != 2)
        {
            std::cerr << "[UPnP] No valid router (IGD) found. Status: " << status << std::endl;
            if (status > 0)
            {
                FreeUPNPUrls(&urls);
                return;
            }
        }

        std::string port_str = std::to_string(port);

        int r = UPNP_AddPortMapping(urls.controlURL,
                                    data.first.servicetype,
                                    port_str.c_str(),
                                    port_str.c_str(),
                                    lanaddr,
                                    "Fibbage",
                                    "TCP",
                                    nullptr,
                                    "0");

        if (r != UPNPCOMMAND_SUCCESS)
        {
            std::cerr << "[UPNP] Failed to forward port " << port << ". Error: " << r << std::endl;
            FreeUPNPUrls(&urls);
            return;
        }

        std::cout << "[UPNP] Port " << port << " successfully opened! External IP for connection: " << wanaddr << std::endl;

        upnp_mapped_port_ = port;
        upnp_control_url_ = urls.controlURL;
        upnp_service_type_ = data.first.servicetype;
        
        FreeUPNPUrls(&urls);
    }

    void UnmapPortUPnP()
    {
        if (upnp_mapped_port_ == -1)
        {
            return;
        }

        std::string port_str = std::to_string(upnp_mapped_port_);
        int r = UPNP_DeletePortMapping(upnp_control_url_.c_str(), 
                                       upnp_service_type_.c_str(), 
                                       port_str.c_str(), "TCP", nullptr);

        if (r == UPNPCOMMAND_SUCCESS)
        {
            std::cout << "[UPnP] Правило для порта " << upnp_mapped_port_ << " успешно удалено из роутера.\n";
        }
        else
        {
            std::cerr << "[UPnP] Failed to close the port on the router. Error code: " << r << std::endl;
        }

        upnp_mapped_port_ = -1;
        upnp_control_url_.clear();
        upnp_service_type_.clear();
    }

    void Stop()
    {
        if (!is_running_)
        {
            return;
        }
        is_running_ = false;

        UnmapPortUPnP();

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

        std::cout << "[TcpServer::AcceptPlayer] New player connect with id: " << temp_id << std::endl;

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
        std::cerr << "[TcpServer::Start] Error code: " << WSAGetLastError() << std::endl;
#else   
        std::clog << "[TcpServer::Start] errno: " << errno << std::endl;
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
        std::cerr << "[TcpServer::Start] Error code: " << WSAGetLastError() << std::endl;
#else   
        std::clog << "[TcpServer::Start] errno: " << errno << std::endl;
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

    impl_->TryMapPortUPnP(port);

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