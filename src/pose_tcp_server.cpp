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
        server_running_ = true;
        server_thread_ = std::make_unique<std::thread>(&PoseTCPServer::runServer, this);
    }
    
    ~PoseTCPServer() {
        // 主动调用关闭方法
        shutdown();
    }

    // 显式关闭方法
    void shutdown() {
    if (!server_running_) return;
    
    RCLCPP_INFO(this->get_logger(), "开始关闭TCP服务器");
    server_running_ = false;
    
    // 先关闭客户端连接
    if (client_socket_ > 0) {
        close(client_socket_);
        client_socket_ = 0;
    }
    
    // 关键改进：通过向服务器套接字发送假数据来中断accept()
    if (server_socket_ > 0) {
        // 创建临时套接字连接到自身，触发accept返回
        int temp_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (temp_socket >= 0) {
            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(PORT);
            
            // 尝试连接（非阻塞）
            connect(temp_socket, (struct sockaddr*)&addr, sizeof(addr));
            close(temp_socket);
        }
        
        // 关闭服务器套接字
        close(server_socket_);
        server_socket_ = 0;
    }
    
    // 等待TCP线程结束
    if (server_thread_ && server_thread_->joinable()) {
        auto future = std::async(std::launch::async, [this]() {
            server_thread_->join();
        });
        
        if (future.wait_for(std::chrono::seconds(5)) == std::future_status::timeout) {
            RCLCPP_ERROR(this->get_logger(), "TCP线程超时未结束！");
        } else {
            RCLCPP_INFO(this->get_logger(), "TCP线程已成功结束");
        }
    }
    
    RCLCPP_INFO(this->get_logger(), "TCP服务器已关闭");
}

private:
    rclcpp::Subscription<geometry_msgs::msg::PoseStamped>::SharedPtr subscription_;
    int server_socket_ = 0;
    int client_socket_ = 0;
    std::unique_ptr<std::thread> server_thread_;
    std::mutex pose_mutex_;
    geometry_msgs::msg::PoseStamped current_pose_;
    std::atomic<bool> server_running_{false};

    void poseCallback(const geometry_msgs::msg::PoseStamped::SharedPtr msg) {
        // 更新当前位姿
        std::lock_guard<std::mutex> lock(pose_mutex_);
        current_pose_ = *msg;
        RCLCPP_INFO(this->get_logger(), "接收到位姿数据");
        
        // 发送位姿数据到客户端（添加客户端连接检查）
        if (client_socket_ > 0) {
            sendPoseToClient();
        }
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
            
            // 重置客户端套接字
            client_socket_ = 0;
            
            // 接受连接（添加中断处理）
            int accept_ret = accept(server_socket_, (struct sockaddr *)&client_address, 
                                  (socklen_t*)&addrlen);
                                  
            if (accept_ret < 0) {
                if (server_running_) {
                    RCLCPP_ERROR(this->get_logger(), "接受连接失败: %d", errno);
                }
                continue;
            }
            
            client_socket_ = accept_ret;
            RCLCPP_INFO(this->get_logger(), "客户端已连接: %s", inet_ntoa(client_address.sin_addr));
            
            // 处理客户端连接（带终止检查）
            handleClient();
        }
    }
    
    void handleClient() {
        while (server_running_ && client_socket_ > 0) {
            // 检查客户端连接状态
            char buffer[1];
            int valread = recv(client_socket_, buffer, 1, MSG_PEEK);
            
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
        
        // 发送消息（增强错误处理）
        int send_ret = send(client_socket_, message.c_str(), message.length(), 0);
        if (send_ret < 0) {
            RCLCPP_WARN(this->get_logger(), "发送数据失败，错误码: %d", errno);
            close(client_socket_);
            client_socket_ = 0;
        } else {
            RCLCPP_INFO(this->get_logger(), "成功发送位姿数据，字节数: %d", send_ret);
        }
    }
};


// 全局标志，用于通知所有线程退出
std::atomic<bool> g_terminate(false);

// 信号处理函数（增强版）
void signalHandler(int signum) {
    std::cout<<"接收到终止信号 "<<signum<<"启动优雅关闭流程"<<std::endl;
    g_terminate = true;
    
    // 触发ROS 2关闭
    rclcpp::shutdown();
    
    // 记录当前时间，用于超时控制
    auto start_time = std::chrono::system_clock::now();
    
    // 等待ROS节点完成关闭（带超时）
    while (rclcpp::ok() && 
           std::chrono::system_clock::now() - start_time < std::chrono::seconds(10)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
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
    
    std::cout<<"主线程接收到终止信号，等待资源释放"<<std::endl;
    
    // 等待ROS 2线程结束
    if (spin_thread.joinable()) {
        spin_thread.join();
    }
    
    // 显式释放节点资源
    node.reset();
    
    std::cout<< "节点已成功关闭"<<std::endl;
    return 0;
}