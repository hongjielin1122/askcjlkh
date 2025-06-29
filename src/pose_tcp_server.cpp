// pose_tcp_server.cpp
#include <rclcpp/rclcpp.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <csignal>

#define PORT 8080

class PoseTCPServer : public rclcpp::Node {
public:
    PoseTCPServer() : Node("pose_tcp_server") {
        // 订阅位姿话题
        subscription_ = this->create_subscription<geometry_msgs::msg::PoseStamped>(
            "pose_topic", 10, std::bind(&PoseTCPServer::poseCallback, this, std::placeholders::_1));
        
        // 初始化TCP服务器
        initServer();
        
        // 启动服务器线程
        server_thread_ = std::make_unique<std::thread>(&PoseTCPServer::runServer, this);
    }
    
    ~PoseTCPServer() {
        // 停止服务器
        server_running_ = false;
        if (server_thread_ && server_thread_->joinable()) {
            server_thread_->join();
        }
        
        // 关闭所有连接
        close(client_socket_);
        close(server_socket_);
    }

private:
    rclcpp::Subscription<geometry_msgs::msg::PoseStamped>::SharedPtr subscription_;
    int server_socket_ = 0;
    int client_socket_ = 0;
    std::unique_ptr<std::thread> server_thread_;
    std::mutex pose_mutex_;
    geometry_msgs::msg::PoseStamped current_pose_;
    std::atomic<bool> server_running_{true};

    void poseCallback(const geometry_msgs::msg::PoseStamped::SharedPtr msg) {
        // 更新当前位姿
        std::lock_guard<std::mutex> lock(pose_mutex_);
        current_pose_ = *msg;
        RCLCPP_INFO(this->get_logger(), "recivepos");
        // 发送位姿数据到客户端
        sendPoseToClient();
    }
    
    void initServer() {
        // 创建socket
        if ((server_socket_ = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            RCLCPP_ERROR(this->get_logger(), "Socket创建失败");
            exit(EXIT_FAILURE);
        }
        
        // 设置socket选项
        int opt = 1;
        if (setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
            RCLCPP_ERROR(this->get_logger(), "设置socket选项失败");
            exit(EXIT_FAILURE);
        }
        
        // 准备地址结构
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(PORT);
        
        // 绑定socket
        if (bind(server_socket_, (struct sockaddr *)&address, sizeof(address)) < 0) {
            RCLCPP_ERROR(this->get_logger(), "绑定失败");
            exit(EXIT_FAILURE);
        }
        
        // 监听连接
        if (listen(server_socket_, 3) < 0) {
            RCLCPP_ERROR(this->get_logger(), "监听失败");
            exit(EXIT_FAILURE);
        }
        
        RCLCPP_INFO(this->get_logger(), "TCP服务器已启动，监听端口 %d", PORT);
    }
    
    void runServer() {
        struct sockaddr_in client_address;
        int addrlen = sizeof(client_address);
        
        while (server_running_) {
            RCLCPP_INFO(this->get_logger(), "等待客户端连接...");
            
            // 接受连接
            if ((client_socket_ = accept(server_socket_, (struct sockaddr *)&client_address, 
                                        (socklen_t*)&addrlen)) < 0) {
                if (server_running_) {
                    RCLCPP_ERROR(this->get_logger(), "接受连接失败");
                }
                continue;
            }
            
            RCLCPP_INFO(this->get_logger(), "客户端已连接: %s", inet_ntoa(client_address.sin_addr));
            
            // 保持连接，直到客户端断开
            while (server_running_) {
                // 检查连接状态
                char test;
                int valread = recv(client_socket_, &test, 1, MSG_PEEK);
                if (valread <= 0) {
                    RCLCPP_INFO(this->get_logger(), "客户端断开连接");
                    close(client_socket_);
                    client_socket_ = 0;
                    break;
                }
                
                // 短暂休眠避免CPU占用过高
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
    
    void sendPoseToClient() {
        if (client_socket_ <= 0) return;
        
        std::lock_guard<std::mutex> lock(pose_mutex_);
        
        // 构建要发送的消息
        std::stringstream ss;
        ss << "Position: [" 
           << current_pose_.pose.position.x << ", "
           << current_pose_.pose.position.y << ", "
           << current_pose_.pose.position.z << "] ";
        
        ss << "Orientation: [" 
           << current_pose_.pose.orientation.x << ", "
           << current_pose_.pose.orientation.y << ", "
           << current_pose_.pose.orientation.z << ", "
           << current_pose_.pose.orientation.w << "]\n";
        
        std::string message = ss.str();
        
        // 发送消息
        if (send(client_socket_, message.c_str(), message.length(), 0) < 0) {
            RCLCPP_WARN(this->get_logger(), "发送数据失败，可能客户端已断开");
            close(client_socket_);
            client_socket_ = 0;
        }else{
            RCLCPP_INFO(this->get_logger(), "发送数据");
        }
    }
    void shutdown() {
    server_running_ = false;
    
    // 关闭TCP连接
    if (client_socket_ > 0) {
        shutdown(client_socket_, SHUT_RDWR);  // 先优雅关闭读写
        close(client_socket_);
    }
    if (server_socket_ > 0) close(server_socket_);
    
    // 等待TCP线程结束（最多5秒）
    if (server_thread_ && server_thread_->joinable()) {
        auto future = std::async(std::launch::async, [this]() {
            server_thread_->join();
        });
        
        if (future.wait_for(std::chrono::seconds(5)) == std::future_status::timeout) {
            std::cerr << "TCP线程超时未结束，强制终止" << std::endl;
            // 若线程仍不结束，可通过系统调用强制终止
        }
    }
}
};


// 全局标志，用于通知所有线程退出
std::atomic<bool> g_terminate(false);

// 信号处理函数
void signalHandler(int signum) {
    std::cout << "接收到终止信号 " << signum << ", 正在优雅退出..." << std::endl;
    g_terminate = true;
    rclcpp::shutdown();
}
int main(int argc, char * argv[]) {
    // 设置信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // 初始化ROS 2
    rclcpp::init(argc, argv);
    
    // 创建节点
    auto node = std::make_shared<PoseTCPServer>();
    
    // 在单独线程中运行ROS 2循环
    std::thread spin_thread([node]() {
        while (rclcpp::ok() && !g_terminate) {
            rclcpp::spin_some(node);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    
    // 主线程等待终止信号
    while (!g_terminate) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    // 等待ROS 2线程结束
    if (spin_thread.joinable()) {
        spin_thread.join();
    }
    
    // 显式调用节点析构函数（确保资源释放）
    node.reset();
    
    // 关闭ROS 2
    rclcpp::shutdown();
    
    std::cout << "节点已成功关闭" << std::endl;
    return 0;
}