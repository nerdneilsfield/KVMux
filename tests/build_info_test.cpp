#include "support/build_info.hpp"

#include <iostream>

int main() {
    if (kvmux::application_name() != "KVMux") {
        std::cerr << "unexpected application name\n";
        return 1;
    }
    if (kvmux::application_version().empty()) {
        std::cerr << "application version must not be empty\n";
        return 1;
    }
    return 0;
}
