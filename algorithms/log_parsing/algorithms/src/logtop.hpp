#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace logtop {

struct RequestDuration {
    std::string traceId;
    std::int64_t durationMs;
};

// Разбирает "YYYY-MM-DDTHH:MM:SS.mmmZ" (UTC) в миллисекунды Unix epoch.
// Другие формы (смещения, строчная z, неполные миллисекунды, несуществующие даты)
// дают std::nullopt.
std::optional<std::int64_t> parse_timestamp(std::string_view text);

// Читает лог построчно и возвращает до topCount самых долгих завершённых запросов:
// по убыванию длительности, при равенстве — по traceId по возрастанию.
std::vector<RequestDuration> find_longest_requests(std::istream& in, std::size_t topCount = 5);

}  // namespace logtop
