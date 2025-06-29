cmake_minimum_required(VERSION 3.8)
project(pose_tcp_bridge)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# 查找依赖
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(geometry_msgs REQUIRED)

# 添加可执行文件
add_executable(pose_tcp_server src/pose_tcp_server.cpp)
ament_target_dependencies(pose_tcp_server rclcpp geometry_msgs)

# 安装目标
install(TARGETS
  pose_tcp_server
  DESTINATION lib/${PROJECT_NAME}
)

ament_package()
