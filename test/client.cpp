// tcp_client.cpp
#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <atomic>



class TCPClient {
public:
    TCPClient() : running_(false) {}
    
    ~TCPClient() {
        disconnect();
    }
    
    bool connectToServer(char * serverIp,int port) {
        // 创建socket
        if ((socket_ = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            std::cerr << "创建socket失败" << std::endl;
            return false;
        }
        
        // 设置服务器地址
        server_addr_.sin_family = AF_INET;
        server_addr_.sin_port = htons(port);
        
        // 将IPv4地址从点分十进制转换为二进制形式
        if(inet_pton(AF_INET, serverIp, &server_addr_.sin_addr) <= 0) {
            std::cerr << "无效地址/地址不支持" << std::endl;
            return false;
        }
        
        // 连接服务器
        if (connect(socket_, (struct sockaddr *)&server_addr_, sizeof(server_addr_)) < 0) {
            std::cerr << "连接失败，错误: " << strerror(errno) << std::endl;
            return false;
        }
        
        std::cout << "已连接到服务器: " << serverIp << ":" << port << std::endl;
        
        // 启动接收线程
        running_ = true;
        receive_thread_ = std::thread(&TCPClient::receiveData, this);
        
        return true;
    }
    
    void disconnect() {
        if (!running_) return;
        
        running_ = false;
        if (receive_thread_.joinable()) {
            receive_thread_.join();
        }
        
        if (socket_ >= 0) {
            close(socket_);
            socket_ = -1;
        }
        
        std::cout << "已断开与服务器的连接" << std::endl;
    }
    
private:
    int socket_ = -1;
    std::string address;
    int port;

    struct sockaddr_in server_addr_;
    std::thread receive_thread_;
    std::atomic<bool> running_;
    
    void receiveData() {
        char buffer[1024] = {0};
        
        while (running_) {
            memset(buffer, 0, sizeof(buffer));
            
            // 接收数据
            int valread = recv(socket_, buffer, 1024, 0);
            
            if (valread > 0) {
                // 打印接收到的位姿数据
                std::cout << "接收到位姿数据: " << buffer << std::endl;
            } else if (valread == 0) {
                // 连接关闭
                std::cerr << "服务器关闭了连接" << std::endl;
                running_ = false;
                break;
            } else {
                // 接收错误
                std::cerr << "接收数据错误: " << strerror(errno) << std::endl;
                running_ = false;
                break;
            }
        }
    }
};



int main() {

    #define SERVER_IP "127.0.0.1"
    #define PORT 8080

    TCPClient client;
    
    // 连接服务器
    if (!client.connectToServer(SERVER_IP,PORT)) {
        return -1;
    }
    
    // 保持主线程运行，直到用户输入退出命令
    std::cout << "按Enter键退出..." << std::endl;
    std::cin.get();
    
    // 断开连接
    client.disconnect();
    
    return 0;
}