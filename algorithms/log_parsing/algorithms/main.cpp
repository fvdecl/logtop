// logtop — находит до 5 traceId с максимальной продолжительностью запроса
// в потоковом текстовом логе произвольного размера.
//
// Формат строки лога:
//   <TIMESTAMP> <TRACE_ID> <EVENT> <MESSAGE...>
// Продолжительность = timestamp(Request completed|Request failed)
//                    - timestamp(Request started)
//
// Режимы работы:
//  - обычный: незавершённые запросы держатся в памяти в RequestTracker, длительность
//    вычисляется при завершении, топ-5 обновляется онлайн. Временных файлов нет.
//  - переполнение: если открытых запросов больше maxOpenRequests, все открытые
//    запросы и все последующие события переносятся во временные бакеты по хешу
//    traceId; бакеты затем сводятся по одному (grace hash partitioning). Это
//    единственный путь, где используется диск, и он нужен, чтобы не нарушать
//    лимит памяти при произвольном числе незавершённых запросов.

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <stdio.h>  // getc_unlocked (POSIX) не объявлен в <cstdio>
#include <unistd.h> // getpid

namespace logtop {

namespace fs = std::filesystem;

// ==========================================================================
// Ошибки
// ==========================================================================

// Фатальная ошибка окружения (диск, права, оборванный stdin). Повреждённая
// строка лога поводом для остановки не является. Бросается только из редких
// путей; построчный разбор исключений не использует.
class LogTopError : public std::runtime_error {
public:
    LogTopError(const std::string& message, int exitCode)
        : std::runtime_error(message), exitCode_(exitCode) {}

    int exit_code() const noexcept { return exitCode_; }

private:
    int exitCode_;
};

// ==========================================================================
// Конфигурация
// ==========================================================================

struct Config {
    size_t topK = 5;
    // Память на один открытый запрос: ключ (до 128 байт в куче) + узел и бакет хеш-таблицы
    // — до ~224 байт. 131072 запросов дают до ~30 МБ, это укладывается в лимит 128 МБ
    // вместе с буфером строки (16 МБ) и буферами записи бакетов (8 МБ).
    size_t maxOpenRequests = 131072;
    size_t bucketCount = 128;                 // фан-аут первого уровня при переполнении
    size_t repartitionFanout = 16;            // фан-аут при дроблении перекошенного бакета
    size_t maxBucketBytes = size_t{4} * 1024 * 1024; // порог размера бакета до дробления
    int maxRepartitionDepth = 6;                     // предохранитель от бесконечной рекурсии

    static constexpr size_t kMaxTraceIdLength = 128;
    static constexpr size_t kMaxLineBytes = size_t{16} * 1024 * 1024;
    static constexpr size_t kBucketIoBufferBytes = size_t{64} * 1024;

    // Верхние границы CLI-параметров: без них пользовательский флаг сам по себе
    // может нарушить лимит памяти.
    static constexpr long long kMaxBucketCountArg = 512;      // 512 x 64 КБ буферов записи
    static constexpr long long kMaxTopKArg = 10000;
    static constexpr long long kMaxOpenRequestsArg = 262144;  // ~59 МБ при худшей длине ключа
};

static_assert(Config::kMaxTraceIdLength <= 255,
              "traceIdLength в BucketRecord — uint8_t; длина traceId не может превышать 255");

// ==========================================================================
// Обработка прерывания (SIGINT/SIGTERM)
// ==========================================================================

// Обработчик сигнала может только записать в volatile sig_atomic_t; проверка
// выполняется в обычном коде, и исключение размотает стек через деструкторы.
volatile std::sig_atomic_t g_receivedSignal = 0;

void handle_termination_signal(int signalNumber) noexcept {
    g_receivedSignal = signalNumber;
}

void throw_if_interrupted() {
    if (g_receivedSignal != 0) {
        throw LogTopError("interrupted by signal " + std::to_string(g_receivedSignal) +
                           ", cleaning up temporary files", 128 + g_receivedSignal);
    }
}

// ==========================================================================
// Разбор timestamp: "YYYY-MM-DDTHH:MM:SS.mmmZ" -> миллисекунды от эпохи
// ==========================================================================

namespace time_parsing {

bool parse_fixed_digits(std::string_view text, size_t offset, size_t width, int& value) noexcept {
    if (offset + width > text.size()) return false;
    int result = 0;
    for (size_t i = 0; i < width; ++i) {
        const char c = text[offset + i];
        if (c < '0' || c > '9') return false;
        result = result * 10 + (c - '0');
    }
    value = result;
    return true;
}

} // namespace time_parsing

// Строго "YYYY-MM-DDTHH:MM:SS.mmmZ" (24 символа, миллисекунды обязательны,
// суффикс Z = UTC). Смещения и строчное "z" не поддерживаются.
// Год — четыре цифры, поэтому результат далеко от предела int64_t.
std::optional<int64_t> parse_iso8601_utc_millis(std::string_view text) noexcept {
    using namespace time_parsing;

    if (text.size() != 24) return std::nullopt;
    if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
        text[16] != ':' || text[19] != '.' || text[23] != 'Z') {
        return std::nullopt;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millis = 0;
    if (!parse_fixed_digits(text, 0, 4, year)) return std::nullopt;
    if (!parse_fixed_digits(text, 5, 2, month)) return std::nullopt;
    if (!parse_fixed_digits(text, 8, 2, day)) return std::nullopt;
    if (!parse_fixed_digits(text, 11, 2, hour)) return std::nullopt;
    if (!parse_fixed_digits(text, 14, 2, minute)) return std::nullopt;
    if (!parse_fixed_digits(text, 17, 2, second)) return std::nullopt;
    if (!parse_fixed_digits(text, 20, 3, millis)) return std::nullopt;

    if (hour > 23 || minute > 59 || second > 59) return std::nullopt;

    // ok() отклоняет месяц вне 1..12, день вне диапазона месяца и 29 февраля
    // не високосного года.
    const std::chrono::year_month_day date{
        std::chrono::year{year},
        std::chrono::month{static_cast<unsigned>(month)},
        std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok()) return std::nullopt;

    const int64_t days = std::chrono::sys_days{date}.time_since_epoch().count();
    const int64_t secondsOfDay = hour * 3600LL + minute * 60LL + second;
    return days * 86'400'000LL + secondsOfDay * 1000LL + millis;
}

// ==========================================================================
// Разбор строки лога: timestamp / traceId / тип события
// ==========================================================================

enum class EventKind { Started, Finished, Irrelevant };

struct LogLine {
    std::string_view timestamp;
    std::string_view traceId;
    EventKind kind;
};

// EVENT в этом формате — два слова ("Request started", "Request completed",
// "Request failed", "DB query"). Граница EVENT — третье и четвёртое слово;
// остальное — MESSAGE, которое результату не нужно и не разбирается.
std::optional<LogLine> split_log_line(std::string_view line) noexcept {
    if (line.empty()) return std::nullopt;

    const size_t afterTimestamp = line.find(' ');
    if (afterTimestamp == std::string_view::npos || afterTimestamp == 0) return std::nullopt;
    const std::string_view timestamp = line.substr(0, afterTimestamp);

    const std::string_view rest = line.substr(afterTimestamp + 1);
    const size_t afterTraceId = rest.find(' ');
    if (afterTraceId == std::string_view::npos || afterTraceId == 0) return std::nullopt;
    const std::string_view traceId = rest.substr(0, afterTraceId);

    const std::string_view afterTrace = rest.substr(afterTraceId + 1);
    if (afterTrace.empty()) return std::nullopt;

    const size_t firstWordEnd = afterTrace.find(' ');
    if (firstWordEnd == std::string_view::npos) {
        return LogLine{timestamp, traceId, EventKind::Irrelevant};
    }

    const size_t secondWordEnd = afterTrace.find(' ', firstWordEnd + 1);
    const size_t eventEnd = (secondWordEnd == std::string_view::npos) ? afterTrace.size() : secondWordEnd;
    const std::string_view event = afterTrace.substr(0, eventEnd);

    EventKind kind = EventKind::Irrelevant;
    if (event == "Request started") kind = EventKind::Started;
    else if (event == "Request completed" || event == "Request failed") kind = EventKind::Finished;

    return LogLine{timestamp, traceId, kind};
}

// ==========================================================================
// Чтение stdin построчно
// ==========================================================================

// std::getline растит строку без предела (строка 200 МБ без '\n' убивала процесс
// под cgroup 128 МБ). Здесь строка длиннее maxBytes отбрасывается: накапливается
// только флаг.
enum class LineStatus { Line, TooLong, EndOfInput };

// Строка без '\n' на конце входа — обычная строка. Пустая строка возвращается
// как Line с пустым содержимым.
LineStatus read_stdin_line(std::string& line, size_t maxBytes) {
    throw_if_interrupted();
    line.clear();
    bool tooLong = false;
    bool sawByte = false;

    for (;;) {
        const int c = ::getc_unlocked(stdin); // POSIX, только глобальное пространство имён
        if (c == EOF) {
            if (std::ferror(stdin) != 0) {
                if (errno == EINTR) {
                    std::clearerr(stdin);
                    throw_if_interrupted();
                    continue;
                }
                const int savedErrno = errno;
                throw LogTopError(std::string("failed to read stdin: ") + std::strerror(savedErrno), 2);
            }
            if (!sawByte) return LineStatus::EndOfInput;
            return tooLong ? LineStatus::TooLong : LineStatus::Line;
        }

        sawByte = true;
        if (c == '\n') return tooLong ? LineStatus::TooLong : LineStatus::Line;
        if (tooLong) continue;
        if (line.size() == maxBytes) {
            tooLong = true;
            line.clear();
            continue;
        }
        line.push_back(static_cast<char>(c));
    }
}

// ==========================================================================
// Событие запроса и статистика
// ==========================================================================

struct RequestEvent {
    std::string_view traceId;     // указывает в буфер строки: копируется при сохранении
    int64_t timestampMillis;
    bool finished;                // true = Request completed/failed, false = Request started
};

struct IngestStats {
    uint64_t linesRead = 0;
    uint64_t linesMalformed = 0;      // повреждённая строка, некорректный timestamp/traceId
    uint64_t linesRelevant = 0;       // корректные Started/Finished
    uint64_t duplicateStarts = 0;     // Started для уже открытого traceId (игнорируется)
    uint64_t unmatchedFinishes = 0;   // Finished без открытого запроса (игнорируется)
    uint64_t negativeDurations = 0;   // Finished раньше Started (запрос отбрасывается)
    bool spilledToDisk = false;
};

// Разбирает строку в событие. Нерелевантные строки молча пропускаются, некорректные
// учитываются в linesMalformed.
std::optional<RequestEvent> parse_request_event(std::string_view line, IngestStats& stats) {
    const auto parsed = split_log_line(line);
    if (!parsed) { ++stats.linesMalformed; return std::nullopt; }
    if (parsed->kind == EventKind::Irrelevant) return std::nullopt;

    const auto timestampMillis = parse_iso8601_utc_millis(parsed->timestamp);
    if (!timestampMillis || parsed->traceId.empty() ||
        parsed->traceId.size() > Config::kMaxTraceIdLength) {
        ++stats.linesMalformed;
        return std::nullopt;
    }

    ++stats.linesRelevant;
    return RequestEvent{parsed->traceId, *timestampMillis, parsed->kind == EventKind::Finished};
}

// ==========================================================================
// Топ-K по длительности (онлайн, без сортировки всех завершённых запросов)
// ==========================================================================

struct DurationEntry {
    std::string traceId;
    int64_t durationMillis;
};

// Порядок результатов: больше длительность — выше; при равенстве — traceId по
// возрастанию. Порядок не зависит от того, в каком режиме шла обработка.
bool ranks_higher(int64_t durationA, std::string_view traceIdA,
                  int64_t durationB, std::string_view traceIdB) noexcept {
    if (durationA != durationB) return durationA > durationB;
    return traceIdA < traceIdB;
}

class TopKDurations {
public:
    explicit TopKDurations(size_t capacity) : capacity_(capacity) { entries_.reserve(capacity_); }

    void offer(std::string_view traceId, int64_t durationMillis) {
        if (capacity_ == 0) return;

        if (entries_.size() < capacity_) {
            entries_.push_back({std::string(traceId), durationMillis});
            return;
        }

        auto weakest = std::min_element(entries_.begin(), entries_.end(),
            [](const DurationEntry& a, const DurationEntry& b) {
                return ranks_higher(b.durationMillis, b.traceId, a.durationMillis, a.traceId);
            });
        if (ranks_higher(durationMillis, traceId, weakest->durationMillis, weakest->traceId)) {
            *weakest = {std::string(traceId), durationMillis};
        }
    }

    std::vector<DurationEntry> sorted_descending() const {
        std::vector<DurationEntry> result = entries_;
        std::sort(result.begin(), result.end(),
            [](const DurationEntry& a, const DurationEntry& b) {
                return ranks_higher(a.durationMillis, a.traceId, b.durationMillis, b.traceId);
            });
        return result;
    }

private:
    size_t capacity_;
    std::vector<DurationEntry> entries_;
};

// ==========================================================================
// Открытые запросы
// ==========================================================================

struct TransparentStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept { return std::hash<std::string_view>{}(sv); }
};

struct TransparentStringEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
};

enum class ApplyResult { Applied, Full };

// Хранит незавершённые запросы: traceId -> время начала. Используется и в обычном
// потоке (с лимитом maxOpen), и при сведении бакетов (без лимита, потому что
// лист ограничен maxBucketBytes).
//
// Правила (фиксируем явно, они совпадают для обоих режимов):
//  - Started для открытого traceId — дубль: учитывается первый, остальные игнорируются.
//  - Started для traceId, который уже завершён, — новый запрос.
//  - Finished без открытого запроса — игнорируется (unmatchedFinishes).
//  - Finished раньше Started — запрос закрывается без результата (negativeDurations).
class RequestTracker {
public:
    explicit RequestTracker(size_t maxOpenRequests) : maxOpen_(maxOpenRequests) {}

    // Full: новый запрос открыть нельзя — событие не учтено, вызывающий код
    // переносит состояние на диск.
    ApplyResult apply(const RequestEvent& event, TopKDurations& topK, IngestStats& stats) {
        const auto it = open_.find(event.traceId);

        if (!event.finished) {
            if (it != open_.end()) {
                ++stats.duplicateStarts;
                return ApplyResult::Applied;
            }
            if (open_.size() >= maxOpen_) return ApplyResult::Full;
            open_.emplace(std::string(event.traceId), event.timestampMillis);
            return ApplyResult::Applied;
        }

        if (it == open_.end()) {
            ++stats.unmatchedFinishes;
            return ApplyResult::Applied;
        }
        const int64_t duration = event.timestampMillis - it->second;
        open_.erase(it);
        if (duration < 0) {
            ++stats.negativeDurations;
            return ApplyResult::Applied;
        }
        topK.offer(event.traceId, duration);
        return ApplyResult::Applied;
    }

    template <class Visitor>
    void for_each_open(Visitor&& visit) const {
        for (const auto& [traceId, startMillis] : open_) {
            visit(std::string_view(traceId), startMillis);
        }
    }

    // Освобождает память таблицы (clear() сохранил бы бакеты).
    void release() { open_ = decltype(open_){}; }

private:
    size_t maxOpen_;
    std::unordered_map<std::string, int64_t, TransparentStringHash, TransparentStringEqual> open_;
};

// ==========================================================================
// Временные бакеты (только при переполнении)
// ==========================================================================

// Запись фиксированного размера: без MESSAGE и без исходного текста timestamp.
#pragma pack(push, 1)
struct BucketRecord {
    int64_t timestampMillis;
    uint8_t traceIdLength;
    uint8_t isFinishEvent; // 0 = Request started, 1 = Request completed/failed
    char traceId[Config::kMaxTraceIdLength];
};
#pragma pack(pop)

static_assert(sizeof(BucketRecord) == 8 + 1 + 1 + Config::kMaxTraceIdLength,
              "BucketRecord не должна иметь скрытого выравнивания");

uint64_t fnv1a_hash(std::string_view data, uint64_t seed) noexcept {
    uint64_t hash = seed ^ 0xcbf29ce484222325ULL;
    for (const char c : data) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// Разный seed на каждом уровне рекурсии дробит случайный перекос хеша. Это не
// защита от целенаправленно подобранных коллизий.
uint64_t hash_seed_for_depth(int depth) noexcept {
    return 0x9E3779B97F4A7C15ULL * (static_cast<uint64_t>(depth) + 1);
}

// Временный каталог создаётся только при переполнении. Удаляется целиком в
// деструкторе, поэтому исключение (в т.ч. прерывание) не оставляет файлов.
class TempWorkspace {
public:
    TempWorkspace() : directory_(create_unique_directory()) {}

    ~TempWorkspace() {
        std::error_code error;
        fs::remove_all(directory_, error);
        if (error) {
            std::fprintf(stderr, "logtop: warning: failed to remove temp directory %s: %s\n",
                         directory_.string().c_str(), error.message().c_str());
        }
    }

    TempWorkspace(const TempWorkspace&) = delete;
    TempWorkspace& operator=(const TempWorkspace&) = delete;

    std::string make_unique_path(std::string_view label) {
        return (directory_ / (std::string(label) + std::to_string(nextFileId_++) + ".bin")).string();
    }

private:
    static fs::path create_unique_directory() {
        std::error_code error;
        const fs::path base = fs::temp_directory_path(error);
        if (error) {
            throw LogTopError("cannot determine system temp directory: " + error.message(), 2);
        }

        std::random_device randomDevice;
        for (int attempt = 0; attempt < 64; ++attempt) {
            fs::path candidate = base / ("logtop-" + std::to_string(::getpid()) + "-" +
                                          std::to_string(randomDevice()));
            if (fs::create_directory(candidate, error)) {
                return candidate;
            }
        }
        throw LogTopError("cannot create a unique temp directory under " + base.string(), 2);
    }

    fs::path directory_;
    uint64_t nextFileId_ = 0;
};

class BucketFileSet {
public:
    BucketFileSet(TempWorkspace& workspace, size_t bucketCount, std::string_view label, size_t ioBufferBytes) {
        files_.reserve(bucketCount);
        paths_.reserve(bucketCount);
        for (size_t i = 0; i < bucketCount; ++i) {
            std::string path = workspace.make_unique_path(label);
            FILE* file = std::fopen(path.c_str(), "wb");
            if (!file) {
                const int savedErrno = errno;
                throw LogTopError("cannot create temp file " + path + ": " + std::strerror(savedErrno), 2);
            }
            std::setvbuf(file, nullptr, _IOFBF, ioBufferBytes);
            files_.push_back(file);
            paths_.push_back(std::move(path));
        }
    }

    ~BucketFileSet() { close_all(); }

    BucketFileSet(const BucketFileSet&) = delete;
    BucketFileSet& operator=(const BucketFileSet&) = delete;

    void write(size_t bucketIndex, const BucketRecord& record) {
        if (std::fwrite(&record, sizeof(record), 1, files_.at(bucketIndex)) != 1) {
            const int savedErrno = errno;
            throw LogTopError("write failed for temp file " + paths_.at(bucketIndex) +
                               " (disk full?): " + std::strerror(savedErrno), 2);
        }
    }

    // Безопасно вызывать повторно, в т.ч. из деструктора.
    std::vector<std::string> close_all() {
        for (auto& file : files_) {
            if (file) {
                std::fclose(file);
                file = nullptr;
            }
        }
        return paths_;
    }

private:
    std::vector<FILE*> files_;
    std::vector<std::string> paths_;
};

// ==========================================================================
// Сведение бакетов (grace hash partitioning)
// ==========================================================================

class BucketReducer {
public:
    BucketReducer(TempWorkspace& workspace, const Config& config, TopKDurations& topK, IngestStats& stats)
        : workspace_(workspace), config_(config), topK_(topK), stats_(stats) {}

    // Обрабатывает один файл-бакет, при необходимости рекурсивно дробит его.
    // Файл удаляется по окончании обработки.
    void reduce(const std::string& bucketPath, int depth) {
        throw_if_interrupted();

        std::error_code sizeError;
        const uintmax_t bucketBytes = fs::file_size(bucketPath, sizeError);
        if (sizeError) return;

        if (bucketBytes > config_.maxBucketBytes) {
            reduce_oversized_bucket(bucketPath, bucketBytes, depth);
        } else {
            reduce_leaf_bucket(bucketPath);
        }

        remove_best_effort(bucketPath);
    }

private:
    void reduce_oversized_bucket(const std::string& bucketPath, uintmax_t bucketBytes, int depth) {
        if (depth >= config_.maxRepartitionDepth) {
            throw LogTopError(
                "bucket " + bucketPath + " (" + std::to_string(bucketBytes) +
                " bytes) still exceeds " + std::to_string(config_.maxBucketBytes) +
                " bytes at max recursion depth " + std::to_string(depth), 3);
        }

        for (const auto& subPath : repartition(bucketPath, depth)) {
            reduce(subPath, depth + 1);
        }
    }

    std::vector<std::string> repartition(const std::string& bucketPath, int depth) {
        BucketFileSet subBuckets(workspace_, config_.repartitionFanout,
                                  "l" + std::to_string(depth + 1) + "-", Config::kBucketIoBufferBytes);

        FILE* input = std::fopen(bucketPath.c_str(), "rb");
        if (!input) {
            const int savedErrno = errno;
            throw LogTopError("cannot reopen " + bucketPath + " for repartitioning: " + std::strerror(savedErrno), 2);
        }

        const uint64_t seed = hash_seed_for_depth(depth + 1);
        BucketRecord record;
        while (std::fread(&record, sizeof(record), 1, input) == 1) {
            const std::string_view traceId(record.traceId, record.traceIdLength);
            subBuckets.write(fnv1a_hash(traceId, seed) % config_.repartitionFanout, record);
        }
        std::fclose(input);

        return subBuckets.close_all();
    }

    // Лист ограничен maxBucketBytes, поэтому и трекер открытых запросов здесь
    // ограничен размером листа.
    void reduce_leaf_bucket(const std::string& bucketPath) {
        FILE* input = std::fopen(bucketPath.c_str(), "rb");
        if (!input) {
            const int savedErrno = errno;
            throw LogTopError("cannot reopen " + bucketPath + ": " + std::strerror(savedErrno), 2);
        }

        RequestTracker tracker(SIZE_MAX);
        BucketRecord record;
        while (std::fread(&record, sizeof(record), 1, input) == 1) {
            const RequestEvent event{std::string_view(record.traceId, record.traceIdLength),
                                     record.timestampMillis, record.isFinishEvent != 0};
            tracker.apply(event, topK_, stats_);
        }
        std::fclose(input);
    }

    static void remove_best_effort(const std::string& path) {
        std::error_code error;
        fs::remove(path, error);
        if (error) {
            std::fprintf(stderr, "logtop: warning: failed to remove temp file %s: %s\n",
                         path.c_str(), error.message().c_str());
        }
    }

    TempWorkspace& workspace_;
    const Config& config_;
    TopKDurations& topK_;
    IngestStats& stats_;
};

// ==========================================================================
// Основной поток: потоковый разбор и переход к бакетам при переполнении
// ==========================================================================

class LogAnalyzer {
public:
    explicit LogAnalyzer(const Config& config)
        : config_(config), topK_(config.topK), tracker_(config.maxOpenRequests) {}

    void consume_stdin() {
        std::string line;
        for (;;) {
            const LineStatus status = read_stdin_line(line, Config::kMaxLineBytes);
            if (status == LineStatus::EndOfInput) break;

            ++stats_.linesRead;
            if (status == LineStatus::TooLong) {
                ++stats_.linesMalformed;
                continue;
            }
            if (const auto event = parse_request_event(line, stats_)) {
                handle_event(*event);
            }
        }

        if (spilled_) finish_spilled_buckets();
    }

    std::vector<DurationEntry> top() const { return topK_.sorted_descending(); }
    const IngestStats& stats() const { return stats_; }

private:
    void handle_event(const RequestEvent& event) {
        if (!spilled_) {
            if (tracker_.apply(event, topK_, stats_) == ApplyResult::Applied) return;
            spill_open_requests();
        }
        write_to_bucket(event);
    }

    // Переносит открытые запросы на диск, освобождает таблицу. Дальнейшие
    // события тоже идут в бакеты, потому что продолжение их пары находится там.
    void spill_open_requests() {
        workspace_.emplace();
        buckets_.emplace(*workspace_, config_.bucketCount, "l0-", Config::kBucketIoBufferBytes);

        tracker_.for_each_open([this](std::string_view traceId, int64_t startMillis) {
            write_to_bucket(RequestEvent{traceId, startMillis, /*finished=*/false});
        });
        tracker_.release();

        spilled_ = true;
        stats_.spilledToDisk = true;
    }

    // Вызывается только при spilled_ == true, когда buckets_ уже создан: горячий
    // путь, поэтому доступ через -> без проверки optional.
    void write_to_bucket(const RequestEvent& event) {
        BucketRecord record{};
        record.timestampMillis = event.timestampMillis;
        record.isFinishEvent = event.finished ? 1 : 0;
        record.traceIdLength = static_cast<uint8_t>(event.traceId.size());
        std::memcpy(record.traceId, event.traceId.data(), event.traceId.size());

        buckets_->write(fnv1a_hash(event.traceId, hash_seed_for_depth(0)) % config_.bucketCount, record);
    }

    void finish_spilled_buckets() {
        const std::vector<std::string> levelZero = buckets_.value().close_all();
        buckets_.reset();

        BucketReducer reducer(workspace_.value(), config_, topK_, stats_);
        for (const auto& bucketPath : levelZero) {
            reducer.reduce(bucketPath, /*depth=*/0);
        }
    }

    const Config& config_;
    TopKDurations topK_;
    RequestTracker tracker_;
    IngestStats stats_;
    bool spilled_ = false;
    // Порядок важен: buckets_ уничтожается раньше workspace_ (сначала закрыть файлы).
    std::optional<TempWorkspace> workspace_;
    std::optional<BucketFileSet> buckets_;
};

// ==========================================================================
// Аргументы командной строки
// ==========================================================================

void apply_command_line_arguments(int argc, char** argv, Config& config) {
    auto readNumber = [](std::string_view arg, std::string_view flag) -> std::optional<long long> {
        if (arg.size() <= flag.size() || arg.substr(0, flag.size()) != flag) return std::nullopt;
        const std::string_view digits = arg.substr(flag.size());
        long long value = 0;
        const auto parseResult = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (parseResult.ec != std::errc{}) return std::nullopt;
        return value;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (const auto buckets = readNumber(arg, "--buckets=")) {
            config.bucketCount = static_cast<size_t>(std::clamp<long long>(*buckets, 1, Config::kMaxBucketCountArg));
        } else if (const auto top = readNumber(arg, "--top=")) {
            config.topK = static_cast<size_t>(std::clamp<long long>(*top, 0, Config::kMaxTopKArg));
        } else if (const auto open = readNumber(arg, "--max-open-requests=")) {
            config.maxOpenRequests = static_cast<size_t>(std::clamp<long long>(*open, 1, Config::kMaxOpenRequestsArg));
        }
    }
}

// ==========================================================================
// Оркестрация
// ==========================================================================

int run(int argc, char** argv) {
    Config config;
    apply_command_line_arguments(argc, argv, config);

    LogAnalyzer analyzer(config);
    analyzer.consume_stdin();

    for (const auto& entry : analyzer.top()) {
        std::printf("%s %lld\n", entry.traceId.c_str(), static_cast<long long>(entry.durationMillis));
    }

    const IngestStats& stats = analyzer.stats();
    std::fprintf(stderr,
                 "logtop: lines read=%llu relevant=%llu malformed=%llu duplicate_starts=%llu "
                 "unmatched_finishes=%llu negative_durations=%llu spilled=%d\n",
                 static_cast<unsigned long long>(stats.linesRead),
                 static_cast<unsigned long long>(stats.linesRelevant),
                 static_cast<unsigned long long>(stats.linesMalformed),
                 static_cast<unsigned long long>(stats.duplicateStarts),
                 static_cast<unsigned long long>(stats.unmatchedFinishes),
                 static_cast<unsigned long long>(stats.negativeDurations),
                 stats.spilledToDisk ? 1 : 0);
    return 0;
}

} // namespace logtop

int main(int argc, char** argv) {
    std::signal(SIGINT, logtop::handle_termination_signal);
    std::signal(SIGTERM, logtop::handle_termination_signal);

    try {
        return logtop::run(argc, argv);
    } catch (const logtop::LogTopError& error) {
        std::fprintf(stderr, "logtop: %s\n", error.what());
        return error.exit_code();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "logtop: unexpected error: %s\n", error.what());
        return 1;
    }
}
