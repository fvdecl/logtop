#!/usr/bin/env bash
# Функциональный набор тестов logtop: кейсы 1-16 и 18 из спецификации.
# Быстрый, детерминированный, не требует много времени/памяти. Кейс 17
# (большой лог для проверки памяти) — отдельно, см. big_log_test.sh.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BINARY="${BINARY:-$PROJECT_DIR/logtop}"

pass=0
fail=0

if [[ ! -x "$BINARY" ]]; then
    echo "Бинарник $BINARY не найден — собираю (make -C $PROJECT_DIR) ..." >&2
    make -C "$PROJECT_DIR" >&2 || { echo "run_tests.sh: сборка не удалась" >&2; exit 1; }
fi

# Все прогоны идут через logtop. LOGTOP_EXTRA_ARGS позволяет прогнать весь набор
# с дополнительными флагами, например --max-open-requests=1 — тогда каждое
# сценарие проверяет путь с переносом на диск.
logtop() { "$BINARY" ${LOGTOP_EXTRA_ARGS:-} "$@"; }

# Прогоняет $BINARY на $input (через stdin) и сравнивает stdout с
# $expected. По умолчанию сравнение точное и позиционное (программа
# обязана сортировать по убыванию длительности). Передайте UNORDERED=1
# непосредственно перед вызовом для сравнения как множества строк — это
# нужно только там, где порядок между равными длительностями не
# специфицирован спецификацией задачи.
run_case() {
    local name="$1" input="$2" expected="$3"
    local actual
    actual="$(printf '%s' "$input" | logtop 2>/dev/null)"

    local ok=1
    if [[ "${UNORDERED:-0}" == "1" ]]; then
        diff <(sort <<<"$expected") <(sort <<<"$actual") >/dev/null || ok=0
    else
        [[ "$actual" == "$expected" ]] || ok=0
    fi

    if [[ $ok -eq 1 ]]; then
        echo "PASS: $name"
        pass=$((pass + 1))
    else
        echo "FAIL: $name"
        echo "  --- expected ---"; sed 's/^/  /' <<<"$expected"
        echo "  --- actual   ---"; sed 's/^/  /' <<<"$actual"
        fail=$((fail + 1))
    fi
    UNORDERED=0
}

# Как run_case, но проверяет только что ожидаемая строка ПРИСУТСТВУЕТ
# где-то в выводе — используется, когда точный состав остального
# вывода не важен для конкретного кейса (например, "затерянный" среди
# шума запрос).
assert_contains_line() {
    local name="$1" input="$2" expectedLine="$3"
    local actual
    actual="$(printf '%s' "$input" | logtop 2>/dev/null)"

    if grep -qxF "$expectedLine" <<<"$actual"; then
        echo "PASS: $name"
        pass=$((pass + 1))
    else
        echo "FAIL: $name (не найдена строка: '$expectedLine')"
        echo "  --- actual ---"; sed 's/^/  /' <<<"$actual"
        fail=$((fail + 1))
    fi
}

# --------------------------------------------------------------------
# 1. Один завершённый запрос
# --------------------------------------------------------------------
run_case "1: один завершённый запрос" \
"2025-01-01T00:00:00.000Z abc123 Request started go
2025-01-01T00:00:05.500Z abc123 Request completed done" \
"abc123 5500"

# --------------------------------------------------------------------
# 2. Два запроса разной длительности
# --------------------------------------------------------------------
run_case "2: два запроса разной длительности" \
"2025-01-01T00:00:00.000Z aaa Request started x
2025-01-01T00:00:01.000Z aaa Request completed x
2025-01-01T00:00:00.000Z bbb Request started x
2025-01-01T00:00:10.000Z bbb Request completed x" \
"bbb 10000
aaa 1000"

# --------------------------------------------------------------------
# 3. Request completed как завершающее событие
# --------------------------------------------------------------------
run_case "3: Request completed" \
"2025-01-01T00:00:00.000Z c1 Request started x
2025-01-01T00:00:02.000Z c1 Request completed ok" \
"c1 2000"

# --------------------------------------------------------------------
# 4. Request failed тоже завершающее событие
# --------------------------------------------------------------------
run_case "4: Request failed" \
"2025-01-01T00:00:00.000Z f1 Request started x
2025-01-01T00:00:03.000Z f1 Request failed error" \
"f1 3000"

# --------------------------------------------------------------------
# 5. Меньше 5 завершённых запросов — выводятся только имеющиеся
# --------------------------------------------------------------------
run_case "5: меньше 5 завершённых" \
"2025-01-01T00:00:00.000Z a Request started x
2025-01-01T00:00:01.000Z a Request completed x
2025-01-01T00:00:00.000Z b Request started x
2025-01-01T00:00:02.000Z b Request completed x
2025-01-01T00:00:00.000Z c Request started x
2025-01-01T00:00:03.000Z c Request completed x" \
"c 3000
b 2000
a 1000"

# --------------------------------------------------------------------
# 6. Ровно 5 завершённых запросов
# --------------------------------------------------------------------
run_case "6: ровно 5 завершённых" \
"2025-01-01T00:00:00.000Z a Request started x
2025-01-01T00:00:01.000Z a Request completed x
2025-01-01T00:00:00.000Z b Request started x
2025-01-01T00:00:02.000Z b Request completed x
2025-01-01T00:00:00.000Z c Request started x
2025-01-01T00:00:03.000Z c Request completed x
2025-01-01T00:00:00.000Z d Request started x
2025-01-01T00:00:04.000Z d Request completed x
2025-01-01T00:00:00.000Z e Request started x
2025-01-01T00:00:05.000Z e Request completed x" \
"e 5000
d 4000
c 3000
b 2000
a 1000"

# --------------------------------------------------------------------
# 7. Больше 5 завершённых запросов — лишние отсекаются
# --------------------------------------------------------------------
run_case "7: больше 5 завершённых" \
"2025-01-01T00:00:00.000Z a Request started x
2025-01-01T00:00:01.000Z a Request completed x
2025-01-01T00:00:00.000Z b Request started x
2025-01-01T00:00:02.000Z b Request completed x
2025-01-01T00:00:00.000Z c Request started x
2025-01-01T00:00:03.000Z c Request completed x
2025-01-01T00:00:00.000Z d Request started x
2025-01-01T00:00:04.000Z d Request completed x
2025-01-01T00:00:00.000Z e Request started x
2025-01-01T00:00:05.000Z e Request completed x
2025-01-01T00:00:00.000Z f Request started x
2025-01-01T00:00:06.000Z f Request completed x
2025-01-01T00:00:00.000Z g Request started x
2025-01-01T00:00:07.000Z g Request completed x" \
"g 7000
f 6000
e 5000
d 4000
c 3000"

# --------------------------------------------------------------------
# 8. Одинаковая длительность у нескольких запросов (граница top-5:
# три запроса претендуют на одинаковую максимальную длительность,
# плюс два с меньшей — итого ровно 5, порядок среди равных не
# специфицирован, поэтому сравниваем как множество строк)
# --------------------------------------------------------------------
UNORDERED=1 run_case "8: одинаковая длительность у нескольких запросов" \
"2025-01-01T00:00:00.000Z tie1 Request started x
2025-01-01T00:00:05.000Z tie1 Request completed x
2025-01-01T00:00:00.000Z tie2 Request started x
2025-01-01T00:00:05.000Z tie2 Request completed x
2025-01-01T00:00:00.000Z tie3 Request started x
2025-01-01T00:00:05.000Z tie3 Request completed x
2025-01-01T00:00:00.000Z low1 Request started x
2025-01-01T00:00:01.000Z low1 Request completed x
2025-01-01T00:00:00.000Z low2 Request started x
2025-01-01T00:00:02.000Z low2 Request completed x" \
"tie1 5000
tie2 5000
tie3 5000
low2 2000
low1 1000"

# --------------------------------------------------------------------
# 9. Запрос без завершения — не попадает в результат
# --------------------------------------------------------------------
run_case "9: запрос без завершения" \
"2025-01-01T00:00:00.000Z pending Request started x" \
""

# --------------------------------------------------------------------
# 10. Несколько запросов с одним traceId: побеждает первый Started;
# повторный Completed после уже сматченного - игнорируется
# --------------------------------------------------------------------
run_case "10: несколько запросов с одним traceId" \
"2025-01-01T00:00:00.000Z dup Request started first
2025-01-01T00:00:01.000Z dup Request started second-should-be-ignored
2025-01-01T00:00:10.000Z dup Request completed done
2025-01-01T00:00:00.000Z dup2 Request started x
2025-01-01T00:00:05.000Z dup2 Request completed first
2025-01-01T00:00:99.000Z dup2 Request completed second-should-be-ignored" \
"dup 10000
dup2 5000"

# --------------------------------------------------------------------
# 11. Строки одного traceId разбросаны по всему файлу
# --------------------------------------------------------------------
scattered_input="2025-01-01T00:00:00.000Z scattered Request started x"
for i in $(seq 1 20); do
    ms="$(printf '%03d' "$i")"
    scattered_input="$scattered_input
2025-01-01T00:00:00.${ms}Z filler$i Request started x
2025-01-01T00:00:00.${ms}Z filler$i Request completed x"
done
scattered_input="$scattered_input
2025-01-01T00:05:00.000Z scattered Request completed done"
assert_contains_line "11: traceId разбросаны по всему файлу" "$scattered_input" "scattered 300000"

# --------------------------------------------------------------------
# 12. Неверный timestamp — строка отбрасывается, соседние не страдают
# --------------------------------------------------------------------
run_case "12: неверный timestamp" \
"2025-11-18T12:36:00.000ZBROKEN badts Request started x
not-a-timestamp-at-all badts2 Request started x
2025-13-01T00:00:00.000Z badmonth Request started x
2025-01-01T00:00:00.000Z ok1 Request started x
2025-01-01T00:00:01.000Z ok1 Request completed x" \
"ok1 1000"

# Длительность через 29 февраля високосного года (2 суток без 1 мс с обеих сторон).
run_case "12b: длительность через 29 февраля високосного года" \
"2024-02-28T23:59:59.999Z leap Request started x
2024-03-01T00:00:00.001Z leap Request completed x" \
"leap 86400002"

# Невозможная дата (30 февраля) отбрасывает строку: запрос без старта не попадает в вывод.
run_case "12c: 30 февраля не принимается за валидную дату" \
"2025-02-30T00:00:00.000Z impossible Request started x
2025-03-02T00:00:00.000Z impossible Request completed x" \
""

# --------------------------------------------------------------------
# 13. Пустая строка не ломает разбор соседних строк
# --------------------------------------------------------------------
run_case "13: пустая строка" \
"2025-01-01T00:00:00.000Z e1 Request started x

2025-01-01T00:00:01.000Z e1 Request completed x" \
"e1 1000"

# --------------------------------------------------------------------
# 14. Повреждённая строка (разные виды) не приводит к падению
# --------------------------------------------------------------------
run_case "14: повреждённая строка" \
"totally garbage no structure here
2025-01-01T00:00:00.000Z
2025-01-01T00:00:00.000Z c1
2025-01-01T00:00:00.000Z c1 Request started x
2025-01-01T00:00:02.000Z c1 Request completed x" \
"c1 2000"

# --------------------------------------------------------------------
# 15. Очень длинный MESSAGE: (a) большой, но в пределах лимита строки —
# должен корректно обработаться; (b) превышающий жёсткий предел длины
# строки — не должен уронить программу и не должен помешать соседнему
# валидному запросу.
# --------------------------------------------------------------------
longMessage="$(head -c 2000000 /dev/zero | tr '\0' 'x')"
run_case "15a: очень длинный MESSAGE (в пределах лимита)" \
"2025-01-01T00:00:00.000Z longmsg Request started ${longMessage}
2025-01-01T00:00:03.000Z longmsg Request completed ${longMessage}" \
"longmsg 3000"

oversizedMessage="$(head -c 18000000 /dev/zero | tr '\0' 'y')"
oversized_input="2025-01-01T00:00:00.000Z oversized Request started ${oversizedMessage}
2025-01-01T00:00:00.000Z ok2 Request started x
2025-01-01T00:00:04.000Z ok2 Request completed x"
run_case "15b: MESSAGE длиннее жёсткого предела строки — не роняет программу" \
"$oversized_input" \
"ok2 4000"

# Слишком длинная строка отбрасывается целиком и учитывается как malformed.
stats_line="$(printf '%s' "$oversized_input" | logtop 2>&1 >/dev/null | grep -o 'malformed=[0-9]*')"
if [[ "$stats_line" == "malformed=1" ]]; then
    echo "PASS: 15c: слишком длинная строка учитывается как malformed"
    pass=$((pass + 1))
else
    echo "FAIL: 15c: ожидалось malformed=1, получено '$stats_line'"
    fail=$((fail + 1))
fi

# --------------------------------------------------------------------
# Чтение входа: пустой файл, последняя строка без '\n' и с ним, пустые
# строки в середине, ошибки ввода-вывода.
# --------------------------------------------------------------------
actual="$(logtop < /dev/null 2>/dev/null)"
code=$?
if [[ -z "$actual" && $code -eq 0 ]]; then
    echo "PASS: R1: пустой вход — пустой результат, код 0"
    pass=$((pass + 1))
else
    echo "FAIL: R1: пустой вход (код $code, вывод '$actual')"
    fail=$((fail + 1))
fi

run_case "R2: последняя строка с переводом строки" \
"$(printf '2025-01-01T00:00:00.000Z nl Request started x\n2025-01-01T00:00:02.000Z nl Request completed x\n')" \
"nl 2000"

run_case "R3: последняя строка без переводов строки" \
"$(printf '2025-01-01T00:00:00.000Z nn Request started x\n2025-01-01T00:00:02.000Z nn Request completed x')" \
"nn 2000"

run_case "R4: пустые строки между записями" \
"$(printf '\n\n2025-01-01T00:00:00.000Z e Request started x\n\n2025-01-01T00:00:02.000Z e Request completed x\n\n')" \
"e 2000"

expect_error_exit() {
    local name="$1" expectedCode="$2" stderrPattern="$3"
    shift 3
    local stderrText code
    stderrText="$("$@" 2>&1 >/dev/null)"
    code=$?
    if [[ $code -eq $expectedCode ]] && grep -q "$stderrPattern" <<<"$stderrText"; then
        echo "PASS: $name"
        pass=$((pass + 1))
    else
        echo "FAIL: $name (код $code, stderr: '$stderrText')"
        fail=$((fail + 1))
    fi
}

expect_error_exit "R5: закрытый stdin — ошибка чтения, код 2" 2 "failed to read stdin" \
    bash -c "exec 0<&-; '$BINARY'"
expect_error_exit "R6: stdin — каталог, ошибка чтения, код 2" 2 "failed to read stdin" \
    bash -c "'$BINARY' < /"

# --------------------------------------------------------------------
# Жизненный цикл открытых запросов: повторное начало, завершение без начала,
# некорректный timestamp, завершение раньше начала, равные длительности.
# --------------------------------------------------------------------
run_case "L1: повторный запрос с тем же traceId после завершения — новый запрос" \
"2025-01-01T00:00:00.000Z x Request started a
2025-01-01T00:00:01.000Z x Request completed a
2025-01-01T00:00:02.000Z x Request started b
2025-01-01T00:00:07.000Z x Request completed b" \
"x 5000
x 1000"

run_case "L2: нулевая длительность (старт и завершение в одну миллисекунду)" \
"2025-01-01T00:00:00.123Z z Request started a
2025-01-01T00:00:00.123Z z Request completed a" \
"z 0"

run_case "L3: завершение с некорректным timestamp не закрывает запрос" \
"2025-01-01T00:00:00.000Z y Request started a
2025-01-01T00:00:05.00Z y Request completed a" \
""

run_case "L4: некорректный timestamp старта — запрос не открывается" \
"2025-01-01T00:00:00.00Z y Request started a
2025-01-01T00:00:05.000Z y Request completed a" \
""

run_case "L5: завершение раньше начала — запрос закрывается без результата" \
"2025-01-01T00:00:05.000Z q Request started a
2025-01-01T00:00:01.000Z q Request completed a
2025-01-01T00:00:09.000Z q Request completed b" \
""

run_case "L6: равные длительности на границе топ-5 — выигрывает меньший traceId" \
"2025-01-01T00:00:00.000Z bb Request started x
2025-01-01T00:00:05.000Z bb Request completed x
2025-01-01T00:00:00.000Z aa Request started x
2025-01-01T00:00:05.000Z aa Request completed x
2025-01-01T00:00:00.000Z t1 Request started x
2025-01-01T00:00:09.000Z t1 Request completed x
2025-01-01T00:00:00.000Z t2 Request started x
2025-01-01T00:00:08.000Z t2 Request completed x
2025-01-01T00:00:00.000Z t3 Request started x
2025-01-01T00:00:07.000Z t3 Request completed x
2025-01-01T00:00:00.000Z t4 Request started x
2025-01-01T00:00:06.000Z t4 Request completed x" \
"t1 9000
t2 8000
t3 7000
t4 6000
aa 5000"

# Счётчик незавершённых/лишних событий в служебной строке stderr.
stats_check="$(printf '%s' "2025-01-01T00:00:00.000Z s Request completed x" | logtop 2>&1 >/dev/null)"
if grep -q "unmatched_finishes=1" <<<"$stats_check"; then
    echo "PASS: L7: завершение без начала учитывается в статистике"
    pass=$((pass + 1))
else
    echo "FAIL: L7: ожидалось unmatched_finishes=1, stderr: '$stats_check'"
    fail=$((fail + 1))
fi

# Минимальное значение --max-open-requests зажимается до 1 и не ломает результат.
actual="$(printf '%s' "2025-01-01T00:00:00.000Z abc Request started x
2025-01-01T00:00:05.500Z abc Request completed x" | "$BINARY" --max-open-requests=0 2>/dev/null)"
if [[ "$actual" == "abc 5500" ]]; then
    echo "PASS: L8: --max-open-requests=0 зажимается до 1"
    pass=$((pass + 1))
else
    echo "FAIL: L8: --max-open-requests=0 (вывод '$actual')"
    fail=$((fail + 1))
fi

# Временные файлы: после прогона каталог TMPDIR должен быть пуст. Проверяем оба
# режима независимо от LOGTOP_EXTRA_ARGS: без переполнения временных файлов нет,
# с переполнением они удаляются.
temp_check() {
    local name="$1"; shift
    local dir
    dir="$(mktemp -d)"
    (
        export TMPDIR="$dir"
        printf '%s' "2025-01-01T00:00:00.000Z t Request started x
2025-01-01T00:00:01.000Z t Request completed x
2025-01-01T00:00:00.000Z u Request started x" | "$BINARY" "$@" >/dev/null 2>&1
    )
    if [[ -z "$(ls -A "$dir")" ]]; then
        echo "PASS: $name"
        pass=$((pass + 1))
    else
        echo "FAIL: $name (остались файлы: $(ls -A "$dir"))"
        fail=$((fail + 1))
    fi
    rm -rf "$dir"
}
temp_check "T1: без переполнения временных файлов не создаётся"
temp_check "T2: при переполнении временные файлы удаляются" --max-open-requests=1

# --------------------------------------------------------------------
# 16. Большое количество traceId (несколько миллионов уникальных,
# подавляющее большинство никогда не завершается) — программа не
# должна падать и обязана корректно найти top-5 среди шума.
# --------------------------------------------------------------------
echo "16: большое количество traceId (генерация ~2 млн строк) ..."
bigCardinalityLog="$(mktemp)"
bigCardinalityExpected="$(mktemp)"
"$SCRIPT_DIR/generate_log.sh" --lines 2000000 --expected-output "$bigCardinalityExpected" > "$bigCardinalityLog"

actual="$(logtop < "$bigCardinalityLog" 2>/dev/null)"
if diff <(sort "$bigCardinalityExpected") <(sort <<<"$actual") >/dev/null; then
    echo "PASS: 16: большое количество traceId"
    pass=$((pass + 1))
else
    echo "FAIL: 16: большое количество traceId"
    echo "  --- expected ---"; sed 's/^/  /' "$bigCardinalityExpected"
    echo "  --- actual   ---"; sed 's/^/  /' <<<"$actual"
    fail=$((fail + 1))
fi
rm -f "$bigCardinalityLog" "$bigCardinalityExpected"

# --------------------------------------------------------------------
# 18. Результат на заранее известном наборе данных (фикстура)
# --------------------------------------------------------------------
actual="$(logtop < "$SCRIPT_DIR/fixtures/known_dataset.log" 2>/dev/null)"
expected="$(cat "$SCRIPT_DIR/fixtures/known_dataset.expected")"
if [[ "$actual" == "$expected" ]]; then
    echo "PASS: 18: результат на заранее известном наборе данных"
    pass=$((pass + 1))
else
    echo "FAIL: 18: результат на заранее известном наборе данных"
    echo "  --- expected ---"; sed 's/^/  /' <<<"$expected"
    echo "  --- actual   ---"; sed 's/^/  /' <<<"$actual"
    fail=$((fail + 1))
fi

echo
echo "Итого: $pass пройдено, $fail провалено"
[[ $fail -eq 0 ]]
