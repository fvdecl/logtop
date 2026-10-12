#!/usr/bin/env bash
# Сквозные проверки: stdin -> executable -> stdout. Путь к бинарнику передаётся в LOGTOP.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOGTOP="${LOGTOP:?LOGTOP must point to the logtop binary}"
failures=0

report() {
    local name="$1" ok="$2"
    if [[ "$ok" == "1" ]]; then
        echo "PASS: $name"
    else
        echo "FAIL: $name"
        failures=$((failures + 1))
    fi
}

# Заранее известный набор: результат записан в known_dataset.expected.
actual="$("$LOGTOP" < "$SCRIPT_DIR/fixtures/known_dataset.log")"
report "known dataset" "$([[ "$actual" == "$(cat "$SCRIPT_DIR/fixtures/known_dataset.expected")" ]] && echo 1 || echo 0)"

# Последняя строка без перевода строки обрабатывается как обычная.
actual="$(printf '%s' '2025-01-01T00:00:00.000Z n Request started x
2025-01-01T00:00:02.000Z n Request completed x' | "$LOGTOP")"
report "последняя строка без перевода строки" "$([[ "$actual" == "n 2000" ]] && echo 1 || echo 0)"

# Пустой вход: пустой вывод и код 0.
empty_output="$("$LOGTOP" < /dev/null)"
empty_code=$?
report "пустой вход" "$([[ $empty_code -eq 0 && -z "$empty_output" ]] && echo 1 || echo 0)"

# Ошибка чтения: закрытый stdin и каталог на входе дают код 2 и сообщение в stderr.
error_output="$("$LOGTOP" <&- 2>&1 >/dev/null)"
closed_code=$?
report "закрытый stdin: код 2" "$([[ $closed_code -eq 2 && "$error_output" == logtop:* ]] && echo 1 || echo 0)"

error_output="$("$LOGTOP" < / 2>&1 >/dev/null)"
dir_code=$?
report "каталог на входе: код 2" "$([[ $dir_code -eq 2 && "$error_output" == logtop:* ]] && echo 1 || echo 0)"

exit $failures
