#include "Server.h"
#include <iostream>

int main() {
    Server server(8888);
    std::cout << "========================================" << std::endl;
    std::cout << "  MyChatServer 正在启动..." << std::endl;
    std::cout << "========================================" << std::endl;
    server.start();
    return 0;
}