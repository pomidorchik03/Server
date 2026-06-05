#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#include "../header/TcpServer.h"

int main() {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);   
#endif
    TcpServer server;

    // 1. Настраиваем обработку входящих сообщений
    server.SetMessageCallback([&server](int client_id, const std::vector<uint8_t>& data) {
        // Переводим байты в строку, чтобы прочитать текст
        std::string message(data.begin(), data.end());
        std::cout << "[Server LOG] Игрок [" << server.GetTokenPlayer(client_id) << "] прислал: " << message << "\n";

        // Формируем сообщение для рассылки всем остальным
        std::string broadcast_msg = "Игрок " + std::to_string(client_id) + ": " + message;
        std::vector<uint8_t> broadcast_data(broadcast_msg.begin(), broadcast_msg.end());

        // Отправляем сообщение СРАЗУ ВСЕМ подключенным игрокам
        server.BroadcastData(broadcast_data);
    });

    // 2. Настраиваем обработку отключения игрока
    server.SetDisconnectCallback([&server](int client_id) {
        std::cout << "[Server LOG] Игрок [" << client_id << "] покинул сервер.\n";

        // Уведомляем оставшихся игроков о потере бойца
        std::string alert = "Игрок " + std::to_string(client_id) + " отключился от игры.";
        std::vector<uint8_t> alert_data(alert.begin(), alert.end());
        server.BroadcastData(alert_data);
    });

    // 3. Запускаем сервер
    int port = 8080;
    std::cout << "[Server] Запуск сервера на порту " << port << "...\n";
    if (!server.Start(port)) {
        std::cerr << "[Server CRITICAL] Не удалось запустить сервер! Возможно, порт занят.\n";
        return -1;
    }
    std::cout << "[Server] Сервер успешно запущен и ждет игроков.\n";
    std::cout << "[Server] Введите 'exit' для остановки сервера.\n\n";

    // Исполняем консольные команды сервера
    std::string command;
    while (true) {
        std::getline(std::cin, command);
        if (command == "exit") {
            break;
        }
    }

    std::cout << "[Server] Остановка сервера...\n";
    server.Stop();
    std::cout << "[Server] Сервер остановлен. Выход.\n";

    return 0;
}