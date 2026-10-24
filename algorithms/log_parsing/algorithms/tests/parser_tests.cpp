// Часть A: разбор одной строки лога и формат timestamp. Трекер и поток здесь не участвуют.
#include "logtop.hpp"

#include <cstdio>
#include <ctime>
#include <string>

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string& description) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", description.c_str());
    }
}

// Эталон: миллисекунды для дат, которые POSIX timegm принимает как валидные.
std::optional<std::int64_t> reference_millis(int year, int month, int day, int hour, int minute, int second, int ms) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    const std::time_t seconds = timegm(&tm);
    std::tm back{};
    gmtime_r(&seconds, &back);
    const bool valid = back.tm_year + 1900 == year && back.tm_mon + 1 == month && back.tm_mday == day;
    if (!valid) return std::nullopt;
    return static_cast<std::int64_t>(seconds) * 1000 + ms;
}

// ---- формат timestamp -----------------------------------------------------

void test_timestamp_format() {
    check(logtop::parse_timestamp("2025-11-18T12:34:56.789Z") == 1763469296789LL, "пример из условия");
    check(logtop::parse_timestamp("1970-01-01T00:00:00.000Z") == 0LL, "граница эпохи");
    check(logtop::parse_timestamp("2024-02-29T00:00:00.000Z") == 1709164800000LL, "29 февраля високосного года");
    check(logtop::parse_timestamp("9999-12-31T23:59:59.999Z") == 253402300799999LL, "верхняя граница года");

    const char* rejected[] = {
        "2025-11-18T12:34:56.78Z",       // 2 цифры миллисекунд
        "2025-11-18T12:34:56.7890Z",     // 4 цифры миллисекунд
        "2025-11-18T12:34:56Z",          // без миллисекунд
        "2025-11-18T12:34:56.789",       // без Z
        "2025-11-18T12:34:56.789z",      // строчная z
        "2025-11-18T12:34:56.789+01:00", // смещение
        "2025-11-18 12:34:56.789Z",      // пробел вместо T
        "2025/11/18T12:34:56.789Z",      // другой разделитель даты
        "2025-02-30T00:00:00.000Z",      // 30 февраля
        "2025-02-29T00:00:00.000Z",      // 29 февраля невисокосного года
        "2025-04-31T00:00:00.000Z",      // 31 апреля
        "2025-13-01T00:00:00.000Z",      // месяц 13
        "2025-11-18T24:00:00.000Z",      // час 24
        "2025-11-18T12:60:00.000Z",      // минуты 60
        "2025-11-18T12:34:60.000Z",      // leap second
        "2025-11-18T12:2a:56.789Z",      // не цифра
        "",
    };
    for (const char* text : rejected) {
        check(!logtop::parse_timestamp(text).has_value(), std::string("должно отклоняться: ") + text);
    }
}

// Все даты 0000-01-01 .. 9999-12-31 (включая календарно невозможные) сверяются с эталоном.
void test_timestamp_against_reference() {
    char buffer[32];
    for (int year = 0; year <= 9999; ++year) {
        for (int month = 1; month <= 12; ++month) {
            for (int day = 1; day <= 31; ++day) {
                std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT13:37:42.123Z", year, month, day);
                const auto actual = logtop::parse_timestamp(std::string_view(buffer, 24));
                const auto expected = reference_millis(year, month, day, 13, 37, 42, 123);
                if (actual != expected) {
                    check(false, std::string("расхождение с эталоном: ") + buffer);
                    return;
                }
                ++checks;
            }
        }
    }
}

// ---- разбор строки ---------------------------------------------------------

void check_record(const std::string& name, std::string_view line, logtop::EventType type,
                  std::string_view traceId, std::int64_t timestampMs) {
    const auto record = logtop::parse_record(line);
    check(record.has_value() && record->eventType == type && record->traceId == traceId &&
              record->timestampMs == timestampMs,
          name);
}

void check_rejected(const std::string& name, std::string_view line) {
    check(!logtop::parse_record(line).has_value(), name);
}

void test_record_event_types() {
    check_record("Request started", "2025-01-01T00:00:00.000Z a Request started GET /",
                 logtop::EventType::Started, "a", 1735689600000LL);
    check_record("Request completed", "2025-01-01T00:00:02.500Z a Request completed 200 OK",
                 logtop::EventType::Completed, "a", 1735689602500LL);
    check_record("Request failed", "2025-01-01T00:00:03.000Z a Request failed 500",
                 logtop::EventType::Failed, "a", 1735689603000LL);
}

void test_record_without_and_with_message() {
    check_record("без message", "2025-01-01T00:00:00.000Z a Request started",
                 logtop::EventType::Started, "a", 1735689600000LL);
    check_record("CRLF без message", "2025-01-01T00:00:00.000Z a Request completed\r",
                 logtop::EventType::Completed, "a", 1735689600000LL);
    check_record("с message, содержащим пробелы", "2025-01-01T00:00:00.000Z a Request started GET /api/items HTTP/1.1",
                 logtop::EventType::Started, "a", 1735689600000LL);
}

void test_record_rejected() {
    check_rejected("неизвестное событие", "2025-01-01T00:00:00.000Z a DB query SELECT 1");
    check_rejected("событие в сообщении не считается",
                   "2025-01-01T00:00:00.000Z a DB query Request started x");
    check_rejected("неполное событие", "2025-01-01T00:00:00.000Z a Request");
    check_rejected("некорректный timestamp", "2025-01-01T00:00:00.00Z a Request started x");
    check_rejected("пустой traceId (двойной пробел)", "2025-01-01T00:00:00.000Z  Request started x");
    check_rejected("повреждённая строка без структуры", "garbage");
    check_rejected("только timestamp", "2025-01-01T00:00:00.000Z");
    check_rejected("timestamp и traceId без события", "2025-01-01T00:00:00.000Z a");
    check_rejected("пустая строка", "");
}

}  // namespace

int main() {
    test_timestamp_format();
    test_timestamp_against_reference();
    test_record_event_types();
    test_record_without_and_with_message();
    test_record_rejected();

    std::printf("parser unit tests: %d проверок, провалов: %d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
