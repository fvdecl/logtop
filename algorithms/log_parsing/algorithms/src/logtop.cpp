#include "logtop.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace logtop {
namespace {

struct TransparentHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

// Порядок результатов: больше длительность — лучше; при равенстве меньше traceId — лучше.
bool ranks_before(const RequestDuration& a, const RequestDuration& b) {
    if (a.durationMs != b.durationMs) return a.durationMs > b.durationMs;
    return a.traceId < b.traceId;
}

bool candidate_beats(std::int64_t durationMs, std::string_view traceId, const RequestDuration& other) {
    if (durationMs != other.durationMs) return durationMs > other.durationMs;
    return traceId < other.traceId;
}

struct Fields {
    std::string_view timestamp;
    std::string_view traceId;
    std::string_view event;  // два слова: "Request started", "Request completed", ...
};

// <TIMESTAMP> <TRACE_ID> <EVENT из двух слов> [MESSAGE...]. MESSAGE не разбирается.
std::optional<Fields> split_fields(std::string_view line) {
    const std::size_t firstSpace = line.find(' ');
    if (firstSpace == std::string_view::npos) return std::nullopt;
    const std::size_t secondSpace = line.find(' ', firstSpace + 1);
    if (secondSpace == std::string_view::npos) return std::nullopt;

    const std::size_t eventStart = secondSpace + 1;
    std::size_t eventEnd = line.size();
    const std::size_t wordEnd = line.find(' ', eventStart);
    if (wordEnd != std::string_view::npos) {
        const std::size_t messageStart = line.find(' ', wordEnd + 1);
        if (messageStart != std::string_view::npos) eventEnd = messageStart;
    }

    return Fields{line.substr(0, firstSpace),
                  line.substr(firstSpace + 1, secondSpace - firstSpace - 1),
                  line.substr(eventStart, eventEnd - eventStart)};
}

}  // namespace

std::optional<std::int64_t> parse_timestamp(std::string_view text) {
    std::istringstream in{std::string(text)};
    std::chrono::sys_time<std::chrono::milliseconds> time;
    in >> std::chrono::parse("%FT%T%Z", time);
    if (in.fail()) return std::nullopt;

    // parse нормализует и принимает не-канонические записи: смещение "+01:00" игнорируется,
    // минуты 60 переносятся на следующий час, "78" превращается в 780 мс. Поэтому результат
    // форматируется обратно и должен совпасть с исходной строкой побайтно.
    if (std::format("{:%FT%T}Z", time) != text) return std::nullopt;
    return time.time_since_epoch().count();
}

std::vector<RequestDuration> find_longest_requests(std::istream& in, std::size_t topCount) {
    // traceId -> время начала открытого запроса.
    std::unordered_map<std::string, std::int64_t, TransparentHash, std::equal_to<>> openRequests;

    // Верхний элемент — худший из лучших topCount, его вытесняет более длительный запрос.
    auto worstOnTop = [](const RequestDuration& a, const RequestDuration& b) { return ranks_before(a, b); };
    std::priority_queue<RequestDuration, std::vector<RequestDuration>, decltype(worstOnTop)> top(worstOnTop);

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        const auto fields = split_fields(line);
        if (!fields || fields->traceId.empty()) continue;

        const bool started = fields->event == "Request started";
        const bool finished = fields->event == "Request completed" || fields->event == "Request failed";
        if (!started && !finished) continue;

        const auto timestampMs = parse_timestamp(fields->timestamp);
        if (!timestampMs) continue;

        if (started) {
            // Повторное начало открытого запроса — дубль; первый побеждает.
            if (openRequests.find(fields->traceId) == openRequests.end()) {
                openRequests.emplace(std::string(fields->traceId), *timestampMs);
            }
            continue;
        }

        // Завершение без открытого начала игнорируется; запрос удаляется в любом случае.
        const auto it = openRequests.find(fields->traceId);
        if (it == openRequests.end()) continue;
        const std::int64_t durationMs = *timestampMs - it->second;
        openRequests.erase(it);
        if (durationMs < 0 || topCount == 0) continue;

        if (top.size() < topCount) {
            top.push({std::string(fields->traceId), durationMs});
        } else if (candidate_beats(durationMs, fields->traceId, top.top())) {
            top.pop();
            top.push({std::string(fields->traceId), durationMs});
        }
    }
    if (in.bad()) throw std::runtime_error("failed to read input");

    std::vector<RequestDuration> result;
    result.reserve(top.size());
    while (!top.empty()) {
        result.push_back(top.top());
        top.pop();
    }
    std::sort(result.begin(), result.end(),
              [](const RequestDuration& a, const RequestDuration& b) { return ranks_before(a, b); });
    return result;
}

}  // namespace logtop
