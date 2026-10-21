#pragma once

namespace forza::core {

// Sends stderr to logcat on a helper thread. The SDK's logx writes every line
// to stderr as well as its file, so this makes the runtime log visible in
// `adb logcat` without touching the SDK.
void StartLogcat(const char* tag);

}  // namespace forza::core
