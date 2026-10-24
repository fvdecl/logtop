#include "logtop.hpp"

#include <chrono>
#include <format>
#include <sstream>

namespace logtop {
namespace {

struct Fields {
    std::string_view timestamp;
    std::string_view traceId;
    std::string_view event;
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

EventType event_type(std::string_view event) {
    if (event == "Request started") return EventType::Started;
    if (event == "Request completed") return EventType::Completed;
    if (event == "Request failed") return EventType::Failed;
    return EventType::Unknown;
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

std::optional<Record> parse_record(std::string_view line) {
    // Файлы с окончаниями CRLF: \r относится к концу строки, а не к событию. Без этого
    // завершение без MESSAGE ("Request completed\r") молча теряется.
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    const auto fields = split_fields(line);
    if (!fields || fields->traceId.empty()) return std::nullopt;  // traceId — непустой токен формата

    const EventType type = event_type(fields->event);
    if (type == EventType::Unknown) return std::nullopt;

    const auto timestampMs = parse_timestamp(fields->timestamp);
    if (!timestampMs) return std::nullopt;
    return Record{*timestampMs, fields->traceId, type};
}

RequestTracker::RequestTracker(std::size_t topCount) : topCount_(topCount) {}

void RequestTracker::apply(const Record& record) {
    switch (record.eventType) {
        case EventType::Started:
            // Повторное начало открытого запроса — дубль; первый побеждает.
            if (openRequests_.find(record.traceId) == openRequests_.end()) {
                openRequests_.emplace(std::string(record.traceId), record.timestampMs);
            }
            break;
        case EventType::Completed:
        case EventType::Failed: {
            // Завершение без открытого начала игнорируется; запрос удаляется в любом случае.
            const auto it = openRequests_.find(record.traceId);
            if (it == openRequests_.end()) break;
            const std::int64_t durationMs = record.timestampMs - it->second;
            openRequests_.erase(it);
            if (durationMs < 0) break;

            // При topCount == 0 элемент сразу вытесняется: top_ остаётся пустым.
            top_.push({std::string(record.traceId), durationMs});
            if (top_.size() > topCount_) top_.pop();
            break;
        }
        case EventType::Unknown:
            break;
    }
}

std::vector<RequestDuration> RequestTracker::top() const {
    // pop() отдаёт худший элемент первым; заполняем с конца, и лучший оказывается первым.
    auto rest = top_;
    std::vector<RequestDuration> result(rest.size());
    for (std::size_t i = result.size(); i-- > 0;) {
        result[i] = rest.top();
        rest.pop();
    }
    return result;
}

}  // namespace logtop
