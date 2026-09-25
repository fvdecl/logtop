// logtop — находит до 5 traceId с максимальной продолжительностью запроса
// в потоковом текстовом логе произвольного размера, при жёстком контроле
// памяти: RAM не зависит от размера входа и от числа уникальных traceId.
//
// Формат строки лога:
//   <TIMESTAMP> <TRACE_ID> <EVENT> <MESSAGE...>
// Продолжительность = timestamp(Request completed|Request failed)
//                    - timestamp(Request started)
//
// Архитектура (детали — в комментариях у соответствующих классов):
// один потоковый проход по stdin раскладывает релевантные записи по
// хешу traceId во временные бинарные файлы фиксированного размера
// записи ("бакеты"). Каждый бакет обрабатывается по очереди; если он
// всё ещё велик, он рекурсивно дробится дальше на более мелкие бакеты
// (grace hash partitioning), пока не станет заведомо безопасного
// размера. В памяти в любой момент времени живо состояние только
// ОДНОГО бакета, а не всего входного файла — это и ограничивает пик
// RAM независимо от размера входа.

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <unistd.h>

namespace logtop {

namespace fs = std::filesystem;

// ==========================================================================
// Ошибки
// ==========================================================================

// Фатальная ошибка окружения (диск, права, оборванный stdin) — в отличие
// от повреждённой строки лога, которая не является поводом останавливать
// обработку многогигабайтного файла. Бросается только из редких,
// не-горячих путей: создание/запись временных файлов, чтение stdin.
// Построчный парсинг такие исключения не использует и не перехватывает.
class LogTopError : public std::runtime_error {
public:
    LogTopError(std::string message, int exitCode)
        : std::runtime_error(std::move(message)), exitCode_(exitCode) {}

    int exit_code() const noexcept { return exitCode_; }

private:
    int exitCode_;
};

// ==========================================================================
// Конфигурация
// ==========================================================================

struct Config {
    size_t topK = 5;
    size_t bucketCount = 128;                 // фан-аут первого уровня партиционирования
    size_t repartitionFanout = 16;            // фан-аут при дроблении перекошенного бакета
    size_t maxBucketBytes = 4u * 1024 * 1024; // порог "сырых" байт бакета до дробления
    int maxRepartitionDepth = 6;              // предохранитель от бесконечной рекурсии

    static constexpr size_t kMaxTraceIdLength = 128;
    static constexpr size_t kReadBufferBytes = 1u * 1024 * 1024;
    static constexpr size_t kMaxLineBytes = 16u * 1024 * 1024;
    static constexpr size_t kBucketIoBufferBytes = 64u * 1024;
};

// ==========================================================================
// Разбор timestamp: "YYYY-MM-DDTHH:MM:SS.mmmZ" -> миллисекунды от эпохи
// ==========================================================================

namespace time_parsing {

// days_from_civil (Howard Hinnant) — проверенный алгоритм перевода
// григорианской даты в число дней от 1970-01-01, без обращения к
// системным calendar API и без аллокаций.
constexpr int64_t days_from_civil(int64_t year, unsigned month, unsigned day) noexcept {
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

bool parse_fixed_digits(std::string_view text, size_t offset, int width, int& value) noexcept {
    if (offset + width > text.size()) return false;
    int result = 0;
    for (int i = 0; i < width; ++i) {
        const char c = text[offset + i];
        if (c < '0' || c > '9') return false;
        result = result * 10 + (c - '0');
    }
    value = result;
    return true;
}

} // namespace time_parsing

// Ожидается ровно 24 символа: "YYYY-MM-DDTHH:MM:SS.mmmZ". Любое отклонение
// (другая длина, не те разделители, нецифровые символы, значения вне
// диапазона) -> nullopt. Без исключений и без аллокаций — это часть
// горячего цикла парсинга.
std::optional<int64_t> parse_iso8601_utc_millis(std::string_view text) noexcept {
    using namespace time_parsing;

    if (text.size() != 24) return std::nullopt;
    if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
        text[16] != ':' || text[19] != '.' || text[23] != 'Z') {
        return std::nullopt;
    }

    int year, month, day, hour, minute, second, millis;
    if (!parse_fixed_digits(text, 0, 4, year)) return std::nullopt;
    if (!parse_fixed_digits(text, 5, 2, month)) return std::nullopt;
    if (!parse_fixed_digits(text, 8, 2, day)) return std::nullopt;
    if (!parse_fixed_digits(text, 11, 2, hour)) return std::nullopt;
    if (!parse_fixed_digits(text, 14, 2, minute)) return std::nullopt;
    if (!parse_fixed_digits(text, 17, 2, second)) return std::nullopt;
    if (!parse_fixed_digits(text, 20, 3, millis)) return std::nullopt;

    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 59) {
        return std::nullopt;
    }

    const int64_t days = time_parsing::days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const int64_t secondsOfDay = hour * 3600LL + minute * 60LL + second;
    return days * 86'400'000LL + secondsOfDay * 1000LL + millis;
}

// ==========================================================================
// Разбор строки лога: timestamp / traceId / event / message
// ==========================================================================

enum class EventKind { Started, Finished, Irrelevant };

struct LogLine {
    std::string_view timestamp;
    std::string_view traceId;
    std::string_view event;
    std::string_view message; // не сохраняется дальше — см. BucketRecord
    EventKind kind;
};

// EVENT в данном формате лога всегда состоит из двух слов ("Request
// started", "Request completed", "Request failed", "DB query" — все
// примеры в спецификации двухсловные; текст задания при этом называет
// EVENT "строкой без пробелов", что противоречит примерам). Разночтение
// снимается так: граница EVENT/MESSAGE — фиксированно третье и четвёртое
// слово строки, всё остальное — MESSAGE. Для нашей задачи это не влияет
// на корректность результата: нерелевантные события (например "DB
// query") в любом случае отбрасываются, независимо от того, как именно
// была бы проведена граница внутри них.
std::optional<LogLine> split_log_line(std::string_view line) noexcept {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
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
        // Одно слово после traceId: для нашего набора событий (все
        // двухсловные) это заведомо нерелевантная строка.
        return LogLine{timestamp, traceId, afterTrace, {}, EventKind::Irrelevant};
    }

    const size_t secondWordEnd = afterTrace.find(' ', firstWordEnd + 1);
    const size_t eventEnd = (secondWordEnd == std::string_view::npos) ? afterTrace.size() : secondWordEnd;
    const std::string_view event = afterTrace.substr(0, eventEnd);
    const std::string_view message =
        (secondWordEnd == std::string_view::npos) ? std::string_view{} : afterTrace.substr(secondWordEnd + 1);

    EventKind kind = EventKind::Irrelevant;
    if (event == "Request started") kind = EventKind::Started;
    else if (event == "Request completed" || event == "Request failed") kind = EventKind::Finished;

    return LogLine{timestamp, traceId, event, message, kind};
}

// ==========================================================================
// Потоковое построчное чтение файлового дескриптора (stdin)
// ==========================================================================

// Хранит только "хвост" непрочитанных байт плюс один буфер чтения
// ограниченного размера — никогда не буферизует входной поток целиком.
// Аномально длинная строка (без '\n' дольше kMaxLineBytes) не растит
// буфер бесконечно, а принудительно "разрезается"; парсер ниже по стеку
// такой фрагмент всё равно отбракует как некорректную строку.
class LineReader {
public:
    LineReader(int inputFd, size_t initialBufferBytes, size_t maxLineBytes)
        : inputFd_(inputFd), maxLineBytes_(maxLineBytes), buffer_(initialBufferBytes) {}

    // Возвращает view на следующую строку (без \n и \r), валиден до
    // следующего вызова next(). false — конец потока.
    bool next(std::string_view& outLine) {
        for (;;) {
            if (const auto newlineOffset = find_newline()) {
                outLine = consume_up_to(*newlineOffset, /*skipDelimiter=*/true);
                return true;
            }
            if (unread_bytes() >= maxLineBytes_) {
                outLine = consume_up_to(dataEnd_, /*skipDelimiter=*/false);
                return true;
            }
            if (!refill()) {
                if (unread_bytes() > 0) {
                    outLine = consume_up_to(dataEnd_, /*skipDelimiter=*/false);
                    return true;
                }
                return false;
            }
        }
    }

private:
    size_t unread_bytes() const noexcept { return dataEnd_ - readPosition_; }

    std::optional<size_t> find_newline() const {
        const void* found = std::memchr(buffer_.data() + readPosition_, '\n', unread_bytes());
        if (!found) return std::nullopt;
        return static_cast<size_t>(static_cast<const char*>(found) - buffer_.data());
    }

    std::string_view consume_up_to(size_t position, bool skipDelimiter) {
        std::string_view line(buffer_.data() + readPosition_, position - readPosition_);
        readPosition_ = position + (skipDelimiter ? 1 : 0);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        return line;
    }

    bool refill() {
        compact();
        grow_if_needed();

        ssize_t bytesRead;
        do {
            bytesRead = ::read(inputFd_, buffer_.data() + dataEnd_, buffer_.size() - dataEnd_);
        } while (bytesRead < 0 && errno == EINTR);

        if (bytesRead < 0) {
            throw LogTopError(std::string("failed to read stdin: ") + std::strerror(errno), 2);
        }
        if (bytesRead == 0) return false; // настоящий конец потока, не ошибка
        dataEnd_ += static_cast<size_t>(bytesRead);
        return true;
    }

    void compact() {
        if (readPosition_ == 0) return;
        const size_t remaining = unread_bytes();
        std::memmove(buffer_.data(), buffer_.data() + readPosition_, remaining);
        readPosition_ = 0;
        dataEnd_ = remaining;
    }

    void grow_if_needed() {
        if (dataEnd_ < buffer_.size()) return;
        const size_t grown = std::min(buffer_.size() * 2, maxLineBytes_);
        if (grown > buffer_.size()) buffer_.resize(grown);
        // Если расти уже некуда — next() обработает это веткой
        // unread_bytes() >= maxLineBytes_ на следующей итерации.
    }

    int inputFd_;
    size_t maxLineBytes_;
    std::vector<char> buffer_;
    size_t readPosition_ = 0;
    size_t dataEnd_ = 0;
};

// ==========================================================================
// Компактная бинарная запись во временных бакетах
// ==========================================================================

// Хранит только то, что нужно для вычисления длительности: ни MESSAGE, ни
// исходный текст timestamp. Фиксированный размер записи делает проверку
// "поместится ли бакет в бюджет памяти" точной (по размеру файла в
// байтах), а не эвристической.
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
    for (unsigned char byte : data) {
        hash ^= byte;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// Разные seed на разных уровнях рекурсии дробят случайный перекос хеша
// между уровнями. Это не защита от целенаправленно подобранных
// коллизий — см. пояснение в сопроводительном разборе после кода.
uint64_t hash_seed_for_depth(int depth) noexcept {
    return 0x9E3779B97F4A7C15ULL * static_cast<uint64_t>(depth + 1);
}

// ==========================================================================
// Временная рабочая директория (RAII)
// ==========================================================================

// Создаётся в стандартном системном временном каталоге (учитывает TMPDIR,
// как это принято на GNU/Linux — std::filesystem::temp_directory_path).
// Удаляется целиком в деструкторе БЕЗ ПОЛАГАНИЯ на то, что вся обработка
// уже подчистила за собой каждый файл: это защищает от утечки временных
// файлов на любом аварийном пути (исключение размотает стек и вызовет
// этот деструктор — в отличие от std::exit(), которым этот код
// сознательно не пользуется).
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

    // Путь для нового временного файла с гарантированно уникальным в
    // пределах этого workspace именем. Счётчик — состояние объекта, не
    // глобальная переменная.
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

// ==========================================================================
// Набор буферизованных файлов-бакетов для записи
// ==========================================================================

class BucketFileSet {
public:
    BucketFileSet(TempWorkspace& workspace, size_t bucketCount, std::string_view label, size_t ioBufferBytes)
        : bucketCount_(bucketCount) {
        files_.reserve(bucketCount_);
        paths_.reserve(bucketCount_);
        for (size_t i = 0; i < bucketCount_; ++i) {
            std::string path = workspace.make_unique_path(label);
            FILE* file = std::fopen(path.c_str(), "wb");
            if (!file) {
                throw LogTopError("cannot create temp file " + path + ": " + std::strerror(errno), 2);
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
        if (std::fwrite(&record, sizeof(record), 1, files_[bucketIndex]) != 1) {
            throw LogTopError("write failed for temp file " + paths_[bucketIndex] +
                               " (disk full?): " + std::strerror(errno), 2);
        }
    }

    // Закрывает все файлы и возвращает их пути. Безопасно вызывать
    // повторно (в т.ч. из деструктора) — уже закрытые файлы пропускаются.
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
    size_t bucketCount_;
    std::vector<FILE*> files_;
    std::vector<std::string> paths_;
};

// ==========================================================================
// Индекс "ещё не завершённых" запросов внутри ОДНОГО бакета
// ==========================================================================

struct TransparentStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept { return std::hash<std::string_view>{}(sv); }
};

struct TransparentStringEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
};

// Размер этой структуры ограничен размером ОДНОГО бакета (см.
// BucketReducer), а не общим числом traceId во входном файле — именно
// это и делает пиковую память независимой от размера входа.
class StartedRequestIndex {
public:
    // Запоминает время старта, если это первое "Started" для traceId.
    // Повторные "Started" для того же traceId (дубль/аномалия входных
    // данных) игнорируются — побеждает самый ранний, и лишняя строка не
    // аллоцируется на дублях благодаря гетерогенному поиску по
    // string_view перед вставкой.
    void record_started(std::string_view traceId, int64_t startMillis) {
        if (startTimes_.find(traceId) == startTimes_.end()) {
            startTimes_.emplace(std::string(traceId), startMillis);
        }
    }

    // Если для traceId был зарегистрирован старт — возвращает
    // длительность и удаляет запись (защита от повторных
    // Completed/Failed на тот же traceId). Если старта не было —
    // завершение без начала считается аномалией и игнорируется.
    // Отрицательная длительность (часы "назад") тоже считается
    // аномалией.
    std::optional<int64_t> record_finished(std::string_view traceId, int64_t finishMillis) {
        auto it = startTimes_.find(traceId); // поиск по string_view, без аллокации
        if (it == startTimes_.end()) return std::nullopt;

        const int64_t duration = finishMillis - it->second;
        startTimes_.erase(it);
        if (duration < 0) return std::nullopt;
        return duration;
    }

private:
    std::unordered_map<std::string, int64_t, TransparentStringHash, TransparentStringEqual> startTimes_;
};

// ==========================================================================
// Топ-K по длительности без сортировки всего множества запросов
// ==========================================================================

struct DurationEntry {
    std::string traceId;
    int64_t durationMillis;
};

// Фиксированного размера (K <= 5 по заданию): линейный поиск минимума
// среди K элементов на каждой вставке дешевле и проще кучи при таком K.
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
            [](const DurationEntry& a, const DurationEntry& b) { return a.durationMillis < b.durationMillis; });
        if (durationMillis > weakest->durationMillis) {
            *weakest = {std::string(traceId), durationMillis};
        }
    }

    // Сортируется только сам результат (<= K элементов), не весь набор
    // завершённых запросов.
    std::vector<DurationEntry> sorted_descending() const {
        std::vector<DurationEntry> result = entries_;
        std::sort(result.begin(), result.end(),
            [](const DurationEntry& a, const DurationEntry& b) { return a.durationMillis > b.durationMillis; });
        return result;
    }

private:
    size_t capacity_;
    std::vector<DurationEntry> entries_;
};

// ==========================================================================
// Фаза 1: потоковое партиционирование stdin по хешу traceId
// ==========================================================================

struct PartitionStats {
    uint64_t linesRead = 0;
    uint64_t linesMalformed = 0;
    uint64_t linesRelevant = 0;
};

class PartitionPass {
public:
    PartitionPass(TempWorkspace& workspace, const Config& config)
        : workspace_(workspace), config_(config) {}

    // Один проход по входному дескриптору. Возвращает пути к бакетам
    // первого уровня. Сама фаза не удерживает ничего, кроме буферов
    // записи и буфера чтения — оба фиксированного размера, независимого
    // от объёма входа.
    std::vector<std::string> run(int inputFd, PartitionStats& stats) {
        BucketFileSet buckets(workspace_, config_.bucketCount, "l0-", Config::kBucketIoBufferBytes);
        LineReader reader(inputFd, Config::kReadBufferBytes, Config::kMaxLineBytes);

        std::string_view line;
        while (reader.next(line)) {
            ++stats.linesRead;
            process_line(line, buckets, stats);
        }

        return buckets.close_all();
    }

private:
    void process_line(std::string_view line, BucketFileSet& buckets, PartitionStats& stats) {
        const auto parsed = split_log_line(line);
        if (!parsed) { ++stats.linesMalformed; return; }
        if (parsed->kind == EventKind::Irrelevant) return;

        const auto timestampMillis = parse_iso8601_utc_millis(parsed->timestamp);
        if (!timestampMillis) { ++stats.linesMalformed; return; }

        if (parsed->traceId.empty() || parsed->traceId.size() > Config::kMaxTraceIdLength) {
            ++stats.linesMalformed;
            return;
        }

        BucketRecord record{};
        record.timestampMillis = *timestampMillis;
        record.isFinishEvent = (parsed->kind == EventKind::Finished) ? 1 : 0;
        record.traceIdLength = static_cast<uint8_t>(parsed->traceId.size());
        std::memcpy(record.traceId, parsed->traceId.data(), parsed->traceId.size());

        const uint64_t hash = fnv1a_hash(parsed->traceId, hash_seed_for_depth(0));
        buckets.write(hash % config_.bucketCount, record);
        ++stats.linesRelevant;
    }

    TempWorkspace& workspace_;
    const Config& config_;
};

// ==========================================================================
// Фаза 2: сведение бакетов (grace hash partitioning)
// ==========================================================================

class BucketReducer {
public:
    BucketReducer(TempWorkspace& workspace, const Config& config, TopKDurations& topK)
        : workspace_(workspace), config_(config), topK_(topK) {}

    // Обрабатывает один файл-бакет; при необходимости рекурсивно дробит
    // его дальше. Файл всегда удаляется по завершении обработки — и в
    // случае листа, и в случае дальнейшего дробления.
    void reduce(const std::string& bucketPath, int depth) {
        std::error_code sizeError;
        const uintmax_t bucketBytes = fs::file_size(bucketPath, sizeError);
        if (sizeError) return; // бакет пуст и не был создан на диске — нечего сводить

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
            // Структурно недостижимо для входов 1-10 ГБ при разумном
            // распределении хешей: на практике хватает 2-3 уровней (см.
            // сопроводительный разбор). Если предел всё же достигнут —
            // это сигнал аномалии (например, целенаправленно подобранные
            // коллизии), и лучше упасть явно, чем молча нарушить лимит
            // памяти загрузкой всего бакета целиком.
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
            throw LogTopError("cannot reopen " + bucketPath + " for repartitioning: " + std::strerror(errno), 2);
        }

        const uint64_t seed = hash_seed_for_depth(depth + 1);
        BucketRecord record;
        while (std::fread(&record, sizeof(record), 1, input) == 1) {
            const std::string_view traceId(record.traceId, record.traceIdLength);
            const uint64_t hash = fnv1a_hash(traceId, seed);
            subBuckets.write(hash % config_.repartitionFanout, record);
        }
        std::fclose(input);

        return subBuckets.close_all();
    }

    // Бакет заведомо мал (ограничен maxBucketBytes) -> размер индекса в
    // памяти ограничен ЭТИМ бакетом, а не всем входным файлом.
    void reduce_leaf_bucket(const std::string& bucketPath) {
        FILE* input = std::fopen(bucketPath.c_str(), "rb");
        if (!input) {
            throw LogTopError("cannot reopen " + bucketPath + ": " + std::strerror(errno), 2);
        }

        StartedRequestIndex startedRequests;
        BucketRecord record;
        while (std::fread(&record, sizeof(record), 1, input) == 1) {
            const std::string_view traceId(record.traceId, record.traceIdLength);
            if (record.isFinishEvent == 0) {
                startedRequests.record_started(traceId, record.timestampMillis);
            } else if (const auto duration = startedRequests.record_finished(traceId, record.timestampMillis)) {
                topK_.offer(traceId, *duration);
            }
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
};

// ==========================================================================
// Аргументы командной строки
// ==========================================================================

void apply_command_line_arguments(int argc, char** argv, Config& config) {
    auto readSizeArgument = [](std::string_view arg, std::string_view flag) -> std::optional<long long> {
        if (arg.size() <= flag.size() || arg.substr(0, flag.size()) != flag) return std::nullopt;
        const std::string_view digits = arg.substr(flag.size());
        long long value = 0;
        const auto parseResult = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (parseResult.ec != std::errc{}) return std::nullopt;
        return value;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (const auto v = readSizeArgument(arg, "--buckets=")) {
            config.bucketCount = static_cast<size_t>(std::max<long long>(1, *v));
        } else if (const auto v = readSizeArgument(arg, "--top=")) {
            config.topK = static_cast<size_t>(std::max<long long>(0, *v));
        }
    }
}

// ==========================================================================
// Оркестрация
// ==========================================================================

int run(int argc, char** argv) {
    Config config;
    apply_command_line_arguments(argc, argv, config);

    TempWorkspace workspace;
    PartitionStats stats;
    TopKDurations topDurations(config.topK);

    std::vector<std::string> firstLevelBuckets;
    {
        PartitionPass partitionPass(workspace, config);
        firstLevelBuckets = partitionPass.run(STDIN_FILENO, stats);
    }

    {
        BucketReducer reducer(workspace, config, topDurations);
        for (const auto& bucketPath : firstLevelBuckets) {
            reducer.reduce(bucketPath, /*depth=*/0);
        }
    }

    for (const auto& entry : topDurations.sorted_descending()) {
        std::printf("%s %lld\n", entry.traceId.c_str(), static_cast<long long>(entry.durationMillis));
    }

    std::fprintf(stderr, "logtop: lines read=%llu relevant=%llu malformed=%llu\n",
                 static_cast<unsigned long long>(stats.linesRead),
                 static_cast<unsigned long long>(stats.linesRelevant),
                 static_cast<unsigned long long>(stats.linesMalformed));

    return 0;
    // TempWorkspace выходит из области видимости здесь и в любом месте,
    // куда исключение размотает стек из этой функции — деструктор
    // гарантированно подчищает временный каталог в обоих случаях.
}

} // namespace logtop

int main(int argc, char** argv) {
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
