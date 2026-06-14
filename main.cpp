#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#include "../header/TcpServer.h"
#include "header/TcpServer.h"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);   
#endif
    TcpServer server;
    int test_port = 54321; // Можешь указать любой порт, который планируешь использовать в игре

    std::cout << "[Main] Попытка запуска сервера на порту " << test_port << "...\n";

    if (server.Start(test_port)) {
        std::cout << "[Main] Сервер запущен и слушает локальную сеть.\n";
        std::cout << "[Main] Оставляем сервер работать на 30 секунд для проверки UPnP...\n";
        
        // Даем времени серверу побыть запущенным, чтобы ты успел проверить логи
        std::this_thread::sleep_for(std::chrono::seconds(30));

        std::cout << "[Main] Останавливаем сервер...\n";
        server.Stop();
        std::cout << "[Main] Программа завершена.\n";
    } else {
        std::cerr << "[Main] Критическая ошибка: не удалось запустить сервер (возможно, порт занят).\n";
    }

    return 0;
}