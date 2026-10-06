#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace logtop {

enum class EventType { Unknown, Started, Completed, Failed };

// traceId указывает в строку, из которой разобрана запись.
struct Record {
    std::int64_t timestampMs;
    std::string_view traceId;
    EventType eventType;
};

struct RequestDuration {
    std::string traceId;
    std::int64_t durationMs;
};

// "a < b" означает «a лучше b»: больше durationMs, при равенстве меньше traceId.
// std::priority_queue ставит наверх максимальный элемент, то есть худший из лучших,
// и именно его можно вытеснить.
inline bool operator<(const RequestDuration& a, const RequestDuration& b) {
    if (a.durationMs != b.durationMs) return a.durationMs > b.durationMs;
    return a.traceId < b.traceId;
}

// Разбирает "YYYY-MM-DDTHH:MM:SS.mmmZ" (UTC) в миллисекунды Unix epoch.
// Другие формы (смещения, строчная z, неполные миллисекунды, несуществующие даты)
// дают std::nullopt.
std::optional<std::int64_t> parse_timestamp(std::string_view text);

// Разбирает строку "<TIMESTAMP> <TRACE_ID> <EVENT из двух слов> [MESSAGE...]".
// Строки, которые не являются Started/Completed/Failed, и некорректные строки дают std::nullopt.
// Некорректная строка завершения не закрывает запрос: он остаётся открытым.
std::optional<Record> parse_record(std::string_view line);

// Находит самые долгие завершённые запросы по потоку уже разобранных записей.
// Хранит только открытые запросы и не более topCount результатов.
class RequestTracker {
public:
    explicit RequestTracker(std::size_t topCount);

    void apply(const Record& record);

    // Результат по убыванию длительности; при равенстве — по traceId по возрастанию.
    std::vector<RequestDuration> top() const;

private:
    // Поиск по string_view без создания std::string на каждое завершение.
    struct TransparentHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };

    std::size_t topCount_;
    std::unordered_map<std::string, std::int64_t, TransparentHash, std::equal_to<>> openRequests_;
    std::priority_queue<RequestDuration> top_;
};

}  // namespace logtop
