#include "app_mode_keys.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main() {
    constexpr size_t kNvsMaxKeyNameLength = 15;
    const char* key = KeyboardProfileNvsKey();
    const size_t len = std::strlen(key);
    if (len == 0 || len > kNvsMaxKeyNameLength) {
        std::fprintf(stderr,
                     "keyboard profile key length must be 1..15, got %zu for '%s'\n",
                     len,
                     key);
        return 1;
    }
    return 0;
}
