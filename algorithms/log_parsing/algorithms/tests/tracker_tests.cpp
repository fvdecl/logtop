// Часть B: алгоритм top-N по уже разобранным записям. Строки и потоки здесь не используются.
#include "logtop.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using logtop::EventType;
using logtop::Record;
using logtop::RequestDuration;
using logtop::RequestTracker;

int failures = 0;
int checks = 0;

void check(bool ok, const std::string& description) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", description.c_str());
    }
}

Record started(std::int64_t ms, std::string_view traceId) { return {ms, traceId, EventType::Started}; }
Record completed(std::int64_t ms, std::string_view traceId) { return {ms, traceId, EventType::Completed}; }
Record failed(std::int64_t ms, std::string_view traceId) { return {ms, traceId, EventType::Failed}; }

// Результат в виде "id ms;" для сравнения.
std::string format(const std::vector<RequestDuration>& requests) {
    std::string out;
    for (const auto& request : requests) {
        out += request.traceId + " " + std::to_string(request.durationMs) + ";";
    }
    return out;
}

void expect_top(const std::string& name, const std::vector<Record>& records, std::size_t topCount,
                const std::string& expected) {
    RequestTracker tracker(topCount);
    for (const auto& record : records) tracker.apply(record);
    const std::string actual = format(tracker.top());
    check(actual == expected, name + ": ожидалось [" + expected + "], получено [" + actual + "]");
}

// ---- порядок -------------------------------------------------------------

void test_ordering() {
    const RequestDuration fast{"a", 1000};
    const RequestDuration slow{"b", 5000};
    const RequestDuration tieSmallId{"aa", 5000};
    const RequestDuration tieBigId{"bb", 5000};

    check(slow < fast && !(fast < slow), "больший durationMs лучше (a < b значит «a лучше»)");
    check(tieSmallId < tieBigId, "при равенстве меньший traceId лучше");

    std::priority_queue<RequestDuration> worstOnTop;
    worstOnTop.push(slow);
    worstOnTop.push(fast);
    worstOnTop.push(tieBigId);
    check(worstOnTop.top().traceId == fast.traceId, "наверху priority_queue худший (fast)");
}

// ---- одиночные и базовые сценарии ----------------------------------------

void test_basic_requests() {
    expect_top("один завершённый", {started(0, "a"), completed(2500, "a")}, 5, "a 2500;");
    expect_top("Request failed завершает запрос", {started(0, "f"), failed(3000, "f")}, 5, "f 3000;");
    expect_top("несколько запросов по убыванию",
               {started(0, "a"), completed(1000, "a"), started(0, "b"), completed(10000, "b"),
                started(0, "c"), completed(3000, "c")},
               5, "b 10000;c 3000;a 1000;");
    expect_top("пустой поток", {}, 5, "");
}

// ---- top-N по размеру ----------------------------------------------------

// Запрос i (1..7) с длительностью i*1000 мс; идентификаторы — литералы, чтобы string_view не висел.
std::vector<Record> sequence_of_requests(int count) {
    static const std::string_view kIds[] = {"r1", "r2", "r3", "r4", "r5", "r6", "r7"};
    std::vector<Record> records;
    for (int i = 1; i <= count; ++i) {
        records.push_back(started(0, kIds[i - 1]));
        records.push_back(completed(i * 1000, kIds[i - 1]));
    }
    return records;
}

void test_top_n_sizes() {
    expect_top("top-5 из семи", sequence_of_requests(7), 5, "r7 7000;r6 6000;r5 5000;r4 4000;r3 3000;");
    expect_top("меньше topCount", sequence_of_requests(3), 5, "r3 3000;r2 2000;r1 1000;");
    expect_top("ровно topCount", sequence_of_requests(5), 5, "r5 5000;r4 4000;r3 3000;r2 2000;r1 1000;");
    expect_top("topCount == 0 даёт пустой результат", sequence_of_requests(3), 0, "");
    expect_top("topCount == 1 оставляет самый длинный", sequence_of_requests(3), 1, "r3 3000;");
}

// ---- ничьи ---------------------------------------------------------------

void test_ties() {
    expect_top("одинаковые duration, разные traceId: сортировка по traceId",
               {started(0, "zz"), completed(1000, "zz"), started(0, "aa"), completed(1000, "aa"),
                started(0, "mm"), completed(1000, "mm")},
               5, "aa 1000;mm 1000;zz 1000;");
    expect_top("ничья на границе top-N: проходит меньший traceId",
               {started(0, "bb"), completed(5000, "bb"), started(0, "aa"), completed(5000, "aa"),
                started(0, "t1"), completed(9000, "t1"), started(0, "t2"), completed(8000, "t2"),
                started(0, "t3"), completed(7000, "t3"), started(0, "t4"), completed(6000, "t4")},
               5, "t1 9000;t2 8000;t3 7000;t4 6000;aa 5000;");
    expect_top("одинаковые traceId и duration — два результата",
               {started(0, "x"), completed(1000, "x"), started(2000, "x"), completed(3000, "x")},
               5, "x 1000;x 1000;");
}

// ---- жизненный цикл запроса ----------------------------------------------

void test_zero_duration_and_failed() {
    expect_top("нулевая длительность попадает в результат",
               {started(1000, "z"), completed(1000, "z")}, 5, "z 0;");
    expect_top("Failed без start игнорируется",
               {failed(1000, "f")}, 5, "");
    expect_top("Failed закрывает открытый запрос",
               {started(0, "f"), failed(2000, "f")}, 5, "f 2000;");
}

void test_lifecycle() {
    expect_top("незавершённый запрос не попадает в результат",
               {started(0, "open"), started(0, "done"), completed(1000, "done")},
               5, "done 1000;");
    expect_top("повторный start: побеждает первый",
               {started(0, "d"), started(4000, "d"), completed(10000, "d")},
               5, "d 10000;");
    expect_top("completed без start игнорируется",
               {completed(1000, "x")},
               5, "");
    expect_top("completed раньше start отбрасывается",
               {started(5000, "q"), completed(1000, "q")},
               5, "");
    expect_top("повторное завершение не даёт второго результата",
               {started(0, "e"), completed(2000, "e"), completed(9000, "e")},
               5, "e 2000;");
    expect_top("новый start после завершения — новый запрос",
               {started(0, "x"), completed(1000, "x"), started(2000, "x"), completed(7000, "x")},
               5, "x 5000;x 1000;");
    expect_top("completed раньше start, затем новый start — учитывается новый",
               {started(5000, "q"), completed(1000, "q"), started(2000, "q"), completed(2500, "q")},
               5, "q 500;");
}

}  // namespace

int main() {
    test_ordering();
    test_basic_requests();
    test_top_n_sizes();
    test_ties();
    test_zero_duration_and_failed();
    test_lifecycle();

    std::printf("request algorithm unit tests: %d проверок, провалов: %d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
