#include "logtop.hpp"

#include <exception>
#include <iostream>

int main() {
    std::ios_base::sync_with_stdio(false);
    try {
        for (const auto& request : logtop::find_longest_requests(std::cin)) {
            std::cout << request.traceId << ' ' << request.durationMs << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "logtop: " << error.what() << '\n';
        return 2;
    }
    return 0;
}
