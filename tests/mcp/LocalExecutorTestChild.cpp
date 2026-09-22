/**
 * @file LocalExecutorTestChild.cpp
 * @brief LocalSessionCommandExecutor 的纯控制台结果夹具。
 */
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    const std::string command{argv[1]};
    if (command == "system.identity") {
        std::fwrite("fixture-ok\n", 1, 11, stdout);
        return 0;
    }
    if (command == "system.uptime") {
        std::fwrite("fixture-failed\n", 1, 15, stderr);
        return 42;
    }
    if (command == "memory.summary") {
        const std::string output(4096, 'x');
        std::fwrite(output.data(), 1, output.size(), stdout);
        return 0;
    }
    if (command == "filesystem.usage") {
        std::this_thread::sleep_for(std::chrono::seconds{1});
        std::fwrite("late\n", 1, 5, stdout);
        return 0;
    }
    return 2;
}
