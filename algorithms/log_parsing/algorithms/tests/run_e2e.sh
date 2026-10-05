#!/usr/bin/env bash
# Сквозные проверки бинарника logtop. Путь к бинарнику передаётся в LOGTOP (CTest выставляет его сам).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOGTOP="${LOGTOP:?LOGTOP must point to the logtop binary}"
failures=0

# Заранее известный набор из fixtures: результат записан в known_dataset.expected.
actual="$("$LOGTOP" < "$SCRIPT_DIR/fixtures/known_dataset.log" 2>/dev/null)"
if [[ "$actual" == "$(cat "$SCRIPT_DIR/fixtures/known_dataset.expected")" ]]; then
    echo "PASS: known dataset"
else
    echo "FAIL: known dataset"; failures=$((failures + 1))
fi

# Пустой вход: пустой вывод и код 0.
empty_output="$("$LOGTOP" < /dev/null)"
if [[ $? -eq 0 && -z "$empty_output" ]]; then
    echo "PASS: empty input"
else
    echo "FAIL: empty input"; failures=$((failures + 1))
fi

# Ошибка чтения stdin (закрытый дескриптор): код 2 и сообщение в stderr.
error_output="$("$LOGTOP" <&- 2>&1 >/dev/null)"
code=$?
if [[ $code -eq 2 && "$error_output" == logtop:* ]]; then
    echo "PASS: closed stdin reports error"
else
    echo "FAIL: closed stdin (exit $code)"; failures=$((failures + 1))
fi

exit $failures
