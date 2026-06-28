#include "ctcpclient.h"
#include "logger.h"
#include "thread_pool.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "exit") {
      break;
    }
    hps::Logger::_info("成功启动并获得父进程传入的信息: " + line);
    std::istringstream iss(line);
    std::string path;
    std::string ip;
    int file_size = 0;
    unsigned short port = 0;
    if (!(iss >> file_size >> path >> ip >> port)) {
      hps::Logger::_error("输入格式错误，正确格式为: <path> <file_size> <ip> <port>");
      continue;
    }
    std::string msg = "准备发送文件: ";
    msg += path;
    msg += " 到 ";
    msg += ip;
    msg += ":";
    msg += std::to_string(port);
    hps::Logger::_info(msg);

    std::vector<hps::CTcpClient> clients;
    clients.reserve(10);
    for (int i = 0; i < 10; ++i) {
      clients.emplace_back(ip, port);
    }
    for (auto& client : clients) {
      if (!client.connectToServer()) {
        continue;
      }
      hps::Logger::_info("成功连接服务器: " + ip + ":" + std::to_string(port));
    }
    hps::ThreadPool thread_pool(clients.size());
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      hps::Logger::_error("无法打开文件: " + path);
      continue;
    }
  }
  return 0;
}
