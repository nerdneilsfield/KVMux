#include "support/build_info.hpp"

#include <iostream>

int main() {
    std::cout << kvmux::application_name() << ' ' << kvmux::application_version()
              << " diagnostic shell\n";
    return 0;
}
