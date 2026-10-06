#include "logtop.hpp"

#include <cstdio>
#include <iostream>
#include <string>

int main() {
    constexpr std::size_t kTopCount = 5;
    logtop::RequestTracker tracker(kTopCount);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (const auto record = logtop::parse_record(line)) tracker.apply(*record);
    }
    if (std::ferror(stdin) != 0) {
        std::cerr << "logtop: failed to read input\n";
        return 2;
    }

    for (const auto& request : tracker.top()) {
        std::cout << request.traceId << ' ' << request.durationMs << '\n';
    }
    return 0;
}
