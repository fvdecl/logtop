// Юнит-тесты parse_iso8601_utc_millis. Подключают main.cpp как заголовок
// (его main переименован), поэтому проверяется ровно та функция, что в
// бинарнике. Эталон времени — POSIX timegm/gmtime_r, а не собственная формула.

#define main logtop_main_not_used
#include "../main.cpp"
#undef main

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

namespace {

int failures = 0;
long long checks = 0;

void expect(bool condition, const std::string& description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", description.c_str());
    }
}

void expect_parsed(std::string_view text, std::optional<int64_t> expected) {
    const auto actual = logtop::parse_iso8601_utc_millis(text);
    const bool same = (actual.has_value() == expected.has_value()) &&
                      (!expected || *expected == *actual);
    expect(same, "parse(\"" + std::string(text) + "\") = " +
                 (actual ? std::to_string(*actual) : "nullopt") +
                 ", ожидалось " + (expected ? std::to_string(*expected) : "nullopt"));
}

// Эталон: календарно ли валидна дата, и если да — миллисекунды Unix epoch.
// timegm нормализует невозможные даты (30 февраля -> 2 марта), поэтому
// валидность определяем обратным преобразованием gmtime_r и сравнением полей.
std::optional<int64_t> reference_millis(int year, int month, int day, int hour, int minute, int second, int ms) {
    // timegm нормализует struct tm на месте, поэтому запрошенные поля
    // сохраняем отдельно и сравниваем с ними, а не с уже нормализованными.
    const int requestedYear = year, requestedMonth = month, requestedDay = day;

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
    const bool calendarValid = back.tm_year + 1900 == requestedYear &&
                               back.tm_mon + 1 == requestedMonth &&
                               back.tm_mday == requestedDay;
    if (!calendarValid) return std::nullopt;
    return static_cast<int64_t>(seconds) * 1000 + ms;
}

void test_valid_examples() {
    expect_parsed("2025-11-18T12:34:56.789Z", 1763469296789LL);
    expect_parsed("1970-01-01T00:00:00.000Z", 0LL);
    expect_parsed("1970-01-01T00:00:00.001Z", 1LL);
    expect_parsed("2024-02-29T23:59:59.999Z", 1709251199999LL);
    expect_parsed("2000-03-01T00:00:00.001Z", 951868800001LL);
}

void test_calendar_edges() {
    expect_parsed("2024-02-29T00:00:00.000Z", 1709164800000LL);  // високосный
    expect_parsed("2000-02-29T00:00:00.000Z", 951782400000LL);   // 400-летнее правило
    expect_parsed("2025-02-29T00:00:00.000Z", std::nullopt);     // не високосный
    expect_parsed("1900-02-29T00:00:00.000Z", std::nullopt);     // 100-летнее правило
    expect_parsed("2100-02-29T00:00:00.000Z", std::nullopt);
    expect_parsed("2025-02-28T00:00:00.000Z", 1740700800000LL);
    expect_parsed("2025-02-30T00:00:00.000Z", std::nullopt);
    expect_parsed("2025-04-31T00:00:00.000Z", std::nullopt);
    expect_parsed("2025-06-31T00:00:00.000Z", std::nullopt);
    expect_parsed("2025-09-31T00:00:00.000Z", std::nullopt);
    expect_parsed("2025-11-31T00:00:00.000Z", std::nullopt);
    expect_parsed("2025-04-30T00:00:00.000Z", 1745971200000LL);
    expect_parsed("2025-01-00T00:00:00.000Z", std::nullopt);
    expect_parsed("2025-01-32T00:00:00.000Z", std::nullopt);
    expect_parsed("2025-00-10T00:00:00.000Z", std::nullopt);
    expect_parsed("2025-13-01T00:00:00.000Z", std::nullopt);
}

void test_time_of_day_bounds() {
    expect_parsed("2025-01-01T23:59:59.999Z", 1735775999999LL);
    expect_parsed("2025-01-01T24:00:00.000Z", std::nullopt);
    expect_parsed("2025-01-01T00:60:00.000Z", std::nullopt);
    expect_parsed("2025-01-01T00:00:60.000Z", std::nullopt);  // leap second не поддерживаем
    expect_parsed("2025-01-01T99:00:00.000Z", std::nullopt);
}

void test_format_violations() {
    expect_parsed("2025-11-18T12:34:56.78Z", std::nullopt);       // 2 цифры мс, длина 23
    expect_parsed("2025-11-18T12:34:56.7890Z", std::nullopt);     // 4 цифры мс, длина 25
    expect_parsed("2025-11-18T12:34:56Z", std::nullopt);          // без миллисекунд
    expect_parsed("2025-11-18T12:34:56.789", std::nullopt);       // без Z
    expect_parsed("2025-11-18T12:34:56.789z", std::nullopt);      // строчная z
    expect_parsed("2025-11-18T12:34:56.789+00:00", std::nullopt); // смещения не поддерживаем
    expect_parsed("2025-11-18T12:34:56.789-01:00", std::nullopt);
    expect_parsed("2025-11-18 12:34:56.789Z", std::nullopt);      // пробел вместо T
    expect_parsed("2025/11/18T12:34:56.789Z", std::nullopt);      // другой разделитель даты
    expect_parsed("2025-11-18T12-34-56.789Z", std::nullopt);      // другой разделитель времени
    expect_parsed("2025-11-18T12:34:56,789Z", std::nullopt);      // запятая вместо точки
    expect_parsed("2025-11-18T12:34:56.789Z ", std::nullopt);     // хвост
    expect_parsed(" 2025-11-18T12:34:56.789Z", std::nullopt);     // ведущий пробел
    expect_parsed("", std::nullopt);
    expect_parsed("Z", std::nullopt);
    expect_parsed("not-a-timestamp-at-all", std::nullopt);
}

void test_non_digit_characters() {
    expect_parsed("2025-1a-18T12:34:56.789Z", std::nullopt);
    expect_parsed("2025-11-18T12:3x:56.789Z", std::nullopt);
    expect_parsed("2025-11-18T12:34:56.78$Z", std::nullopt);
    expect_parsed("20+5-11-18T12:34:56.789Z", std::nullopt);
    expect_parsed("2025-11-18T12:34:56.-89Z", std::nullopt);
}

void test_full_calendar_against_reference() {
    // Исчерпывающе: все (год, месяц, день) с 0000 по 9999 — включая все
    // невозможные комбинации (31 февраля, 31 апреля и т.п.).
    constexpr int kMs = 123;
    char buffer[32];
    for (int year = 0; year <= 9999; ++year) {
        for (int month = 1; month <= 12; ++month) {
            for (int day = 1; day <= 31; ++day) {
                std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT00:00:00.%03dZ", year, month, day, kMs);
                const auto actual = logtop::parse_iso8601_utc_millis(std::string_view(buffer, 24));
                const auto expected = reference_millis(year, month, day, 0, 0, 0, kMs);
                const bool same = (actual.has_value() == expected.has_value()) &&
                                  (!expected || *expected == *actual);
                if (!same) {
                    expect(false, std::string("календарь расходится с эталоном: ") + buffer);
                    return;
                }
                ++checks;
            }
        }
    }
}

void test_time_fields_against_reference() {
    char buffer[32];
    for (int hour = 0; hour <= 25; ++hour) {
        for (int minute = 0; minute <= 61; ++minute) {
            for (int second = 0; second <= 61; ++second) {
                std::snprintf(buffer, sizeof buffer, "2025-03-15T%02d:%02d:%02d.999Z", hour, minute, second);
                const auto actual = logtop::parse_iso8601_utc_millis(std::string_view(buffer, 24));
                const auto expected = reference_millis(2025, 3, 15, hour, minute, second, 999);
                const bool hasRealTime = hour <= 23 && minute <= 59 && second <= 59;
                const bool same = (actual.has_value() == hasRealTime) &&
                                  (!hasRealTime || *actual == *expected);
                if (!same) {
                    expect(false, std::string("время расходится с эталоном: ") + buffer);
                    return;
                }
                ++checks;
            }
        }
    }
}

void test_epoch_range_boundaries() {
    // Минимальный и максимальный моменты, которые может выразить поле года из 4 цифр.
    expect_parsed("0000-01-01T00:00:00.000Z", reference_millis(0, 1, 1, 0, 0, 0, 0));
    expect_parsed("9999-12-31T23:59:59.999Z", reference_millis(9999, 12, 31, 23, 59, 59, 999));
    expect(logtop::parse_iso8601_utc_millis("0000-01-01T00:00:00.000Z").value() == -62167219200000LL,
           "нижняя граница 0000-01-01 должна быть -62167219200000 мс");
    expect(logtop::parse_iso8601_utc_millis("9999-12-31T23:59:59.999Z").value() == 253402300799999LL,
           "верхняя граница 9999-12-31 должна быть 253402300799999 мс");
}

} // namespace

int main() {
    test_valid_examples();
    test_calendar_edges();
    test_time_of_day_bounds();
    test_format_violations();
    test_non_digit_characters();
    test_full_calendar_against_reference();
    test_time_fields_against_reference();
    test_epoch_range_boundaries();

    std::printf("timestamp unit tests: %lld проверок, провалов: %d\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
