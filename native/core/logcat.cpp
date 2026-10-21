#include "logcat.h"

#include <android/log.h>
#include <unistd.h>

#include <string>
#include <thread>

namespace forza::core {

void StartLogcat(const char* tag) {
    int fds[2];
    if (pipe(fds) != 0) return;
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    std::thread([fd = fds[0], name = std::string(tag)] {
        char buf[1024];
        std::string line;
        for (ssize_t n; (n = read(fd, buf, sizeof(buf))) > 0;) {
            line.append(buf, size_t(n));
            for (size_t nl; (nl = line.find('\n')) != std::string::npos; line.erase(0, nl + 1))
                __android_log_write(ANDROID_LOG_INFO, name.c_str(), line.substr(0, nl).c_str());
        }
    }).detach();
}

}  // namespace forza::core
