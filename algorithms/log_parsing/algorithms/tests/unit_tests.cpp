#include "logtop.hpp"

#include <cstdio>
#include <ctime>
#include <sstream>
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

std::string analyze(const std::string& log) {
    std::istringstream in(log);
    std::string out;
    for (const auto& request : logtop::find_longest_requests(in)) {
        out += request.traceId + " " + std::to_string(request.durationMs) + "\n";
    }
    return out;
}

void expect_analysis(const std::string& name, const std::string& log, const std::string& expected) {
    const std::string actual = analyze(log);
    check(actual == expected, name + ": ожидалось [" + expected + "], получено [" + actual + "]");
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

void test_timestamp_examples() {
    check(logtop::parse_timestamp("2025-11-18T12:34:56.789Z") == 1763469296789LL, "пример из условия");
    check(logtop::parse_timestamp("1970-01-01T00:00:00.000Z") == 0LL, "граница эпохи");
    check(logtop::parse_timestamp("2024-02-29T00:00:00.000Z") == 1709164800000LL, "29 февраля високосного года");
    check(logtop::parse_timestamp("9999-12-31T23:59:59.999Z") == 253402300799999LL, "верхняя граница года");
}

void test_timestamp_rejected() {
    const char* rejected[] = {
        "2025-11-18T12:34:56.78Z",       // 2 цифры миллисекунд
        "2025-11-18T12:34:56.7890Z",     // 4 цифры миллисекунд
        "2025-11-18T12:34:56Z",          // без миллисекунд
        "2025-11-18T12:34:56.789",       // без Z
        "2025-11-18T12:34:56.789z",      // строчная z
        "2025-11-18T12:34:56.789+01:00", // смещение (chrono молча его игнорирует)
        "2025-11-18 12:34:56.789Z",      // пробел вместо T
        "2025/11/18T12:34:56.789Z",      // другой разделитель даты
        "2025-02-30T00:00:00.000Z",      // 30 февраля
        "2025-02-29T00:00:00.000Z",      // 29 февраля невисокосного года
        "2025-04-31T00:00:00.000Z",      // 31 апреля
        "2025-13-01T00:00:00.000Z",      // месяц 13
        "2025-11-18T24:00:00.000Z",      // час 24
        "2025-11-18T12:60:00.000Z",      // минуты 60 (chrono переносит на час)
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

void test_single_completed() {
    expect_analysis("один завершённый",
        "2025-01-01T00:00:00.000Z a Request started GET /\n"
        "2025-01-01T00:00:02.500Z a Request completed 200 OK\n",
        "a 2500\n");
}

void test_failed_counts_as_finish() {
    expect_analysis("Request failed завершает запрос",
        "2025-01-01T00:00:00.000Z f Request started x\n"
        "2025-01-01T00:00:03.000Z f Request failed 500\n",
        "f 3000\n");
}

void test_several_sorted_descending() {
    expect_analysis("несколько запросов по убыванию",
        "2025-01-01T00:00:00.000Z a Request started x\n"
        "2025-01-01T00:00:01.000Z a Request completed x\n"
        "2025-01-01T00:00:00.000Z b Request started x\n"
        "2025-01-01T00:00:10.000Z b Request completed x\n"
        "2025-01-01T00:00:00.000Z c Request started x\n"
        "2025-01-01T00:00:03.000Z c Request completed x\n",
        "b 10000\nc 3000\na 1000\n");
}

void test_top5_of_seven() {
    std::string log;
    for (int i = 1; i <= 7; ++i) {
        const std::string id = "r" + std::to_string(i);
        log += "2025-01-01T00:00:00.000Z " + id + " Request started x\n";
        log += "2025-01-01T00:00:0" + std::to_string(i) + ".000Z " + id + " Request completed x\n";
    }
    expect_analysis("top-5 из семи", log, "r7 7000\nr6 6000\nr5 5000\nr4 4000\nr3 3000\n");
}

void test_fewer_than_five() {
    expect_analysis("меньше пяти завершённых",
        "2025-01-01T00:00:00.000Z a Request started x\n"
        "2025-01-01T00:00:01.000Z a Request completed x\n",
        "a 1000\n");
}

void test_ties_at_boundary() {
    // Шесть запросов, два с длительностью 5000 на границе top-5: проходит меньший traceId.
    expect_analysis("равные длительности на границе",
        "2025-01-01T00:00:00.000Z bb Request started x\n2025-01-01T00:00:05.000Z bb Request completed x\n"
        "2025-01-01T00:00:00.000Z aa Request started x\n2025-01-01T00:00:05.000Z aa Request completed x\n"
        "2025-01-01T00:00:00.000Z t1 Request started x\n2025-01-01T00:00:09.000Z t1 Request completed x\n"
        "2025-01-01T00:00:00.000Z t2 Request started x\n2025-01-01T00:00:08.000Z t2 Request completed x\n"
        "2025-01-01T00:00:00.000Z t3 Request started x\n2025-01-01T00:00:07.000Z t3 Request completed x\n"
        "2025-01-01T00:00:00.000Z t4 Request started x\n2025-01-01T00:00:06.000Z t4 Request completed x\n",
        "t1 9000\nt2 8000\nt3 7000\nt4 6000\naa 5000\n");
}

void test_unfinished_excluded() {
    expect_analysis("незавершённый запрос не попадает в результат",
        "2025-01-01T00:00:00.000Z open Request started x\n"
        "2025-01-01T00:00:00.000Z done Request started x\n"
        "2025-01-01T00:00:01.000Z done Request completed x\n",
        "done 1000\n");
}

void test_wrong_sequences() {
    expect_analysis("завершение без начала игнорируется",
        "2025-01-01T00:00:01.000Z x Request completed x\n",
        "");
    expect_analysis("завершение раньше начала отбрасывается",
        "2025-01-01T00:00:05.000Z q Request started x\n"
        "2025-01-01T00:00:01.000Z q Request completed x\n",
        "");
    expect_analysis("повторное начало: побеждает первое",
        "2025-01-01T00:00:00.000Z d Request started first\n"
        "2025-01-01T00:00:04.000Z d Request started second\n"
        "2025-01-01T00:00:10.000Z d Request completed x\n",
        "d 10000\n");
    expect_analysis("повторное завершение не даёт второго результата",
        "2025-01-01T00:00:00.000Z e Request started x\n"
        "2025-01-01T00:00:02.000Z e Request completed x\n"
        "2025-01-01T00:00:09.000Z e Request completed x\n",
        "e 2000\n");
    expect_analysis("повторный запрос после завершения — новый",
        "2025-01-01T00:00:00.000Z x Request started a\n"
        "2025-01-01T00:00:01.000Z x Request completed a\n"
        "2025-01-01T00:00:02.000Z x Request started b\n"
        "2025-01-01T00:00:07.000Z x Request completed b\n",
        "x 5000\nx 1000\n");
}

void test_invalid_lines_ignored() {
    expect_analysis("некорректный timestamp завершения не закрывает запрос",
        "2025-01-01T00:00:00.000Z y Request started a\n"
        "2025-01-01T00:00:05.00Z y Request completed a\n",
        "");
    expect_analysis("некорректный timestamp начала — запрос не открывается",
        "2025-01-01T00:00:00.00Z y Request started a\n"
        "2025-01-01T00:00:05.000Z y Request completed a\n",
        "");
    expect_analysis("повреждённые и посторонние строки пропускаются",
        "garbage\n\n2025-01-01T00:00:00.000Z\n"
        "2025-01-01T00:00:00.000Z z DB query SELECT 1\n"
        "2025-01-01T00:00:00.000Z ok Request started x\n"
        "2025-01-01T00:00:03.000Z ok Request completed x\n",
        "ok 3000\n");
}

void test_empty_and_unterminated_input() {
    expect_analysis("пустой вход", "", "");
    expect_analysis("последняя строка без перевода строки",
        "2025-01-01T00:00:00.000Z n Request started x\n"
        "2025-01-01T00:00:02.000Z n Request completed x",
        "n 2000\n");
    expect_analysis("CRLF",
        "2025-01-01T00:00:00.000Z c Request started x\r\n"
        "2025-01-01T00:00:01.000Z c Request completed x\r\n",
        "c 1000\n");
}

}  // namespace

int main() {
    test_timestamp_examples();
    test_timestamp_rejected();
    test_timestamp_against_reference();
    test_single_completed();
    test_failed_counts_as_finish();
    test_several_sorted_descending();
    test_top5_of_seven();
    test_fewer_than_five();
    test_ties_at_boundary();
    test_unfinished_excluded();
    test_wrong_sequences();
    test_invalid_lines_ignored();
    test_empty_and_unterminated_input();

    std::printf("logtop unit tests: %d проверок, провалов: %d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
