#!/usr/bin/env bash
# Потоковый генератор синтетического лога для нагрузочных тестов и
# проверки памяти logtop. Пишет строки в stdout по одной через awk —
# не накапливает сгенерированный лог целиком ни в памяти bash, ни в
# памяти awk (нет ни одного массива, растущего с числом строк; цикл в
# BEGIN сам управляет числом итераций, без внешнего входного потока).
#
# Состав потока (фиксированные пропорции, достаточные для нагрузки
# и проверки корректности одновременно):
#   60% — уникальный traceId с одним "Request started", без завершения
#         (создаёт огромную кардинальность незакрытых запросов)
#   10% — пустая строка
#   10% — повреждённая строка (без валидной структуры)
#   20% — пары "Started"/"Completed" с небольшой случайной
#         длительностью (шум, заведомо меньше "подсаженных" ниже)
#
# Дополнительно "подсаживается" ровно --known-top-k пар с заведомо
# большой и заведомо различающейся длительностью, разнесённых по
# разным частям файла (0%, 1/K, 2/K, ... от общего числа строк) —
# это даёт точный, заранее известный ожидаемый top-K даже на
# многогигабайтном логе, без отдельного прохода независимым эталоном.
set -euo pipefail

lines=1000000
knownTopK=5
seed=42
expectedOut=""

usage() {
    cat >&2 <<EOF
Usage: $(basename "$0") --lines N [опции]

  --lines N               количество строк лога (по умолчанию: $lines)
  --known-top-k K         число "подсаженных" пар с заведомо наибольшей
                          длительностью (по умолчанию: $knownTopK; 0 — отключить)
  --seed N                seed для шумовых длительностей (по умолчанию: $seed)
  --expected-output FILE  куда записать точный ожидаемый вывод logtop
                          (traceId duration_ms, по убыванию) для сверки в тестах

Примеры:
  $(basename "$0") --lines 20000000 > big.log
  $(basename "$0") --lines 20000000 --expected-output expected.txt | ./logtop
EOF
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --lines) lines="$2"; shift 2 ;;
        --known-top-k) knownTopK="$2"; shift 2 ;;
        --seed) seed="$2"; shift 2 ;;
        --expected-output) expectedOut="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "generate_log.sh: неизвестный аргумент: $1" >&2; usage ;;
    esac
done

if (( knownTopK > 0 && lines < knownTopK * 20 )); then
    echo "generate_log.sh: --lines слишком мал относительно --known-top-k (нужно хотя бы $((knownTopK * 20)))" >&2
    exit 1
fi

awk -v lines="$lines" -v knownTopK="$knownTopK" -v seed="$seed" -v expectedOut="$expectedOut" '
function fmt_ts(offsetMs,    totalSec, ms, s, m, h) {
    # offsetMs всегда держится в пределах одних суток (< 86 400 000) —
    # см. выбор констант ниже — поэтому дата фиксирована и не требует
    # календарной арифметики с переносом через сутки.
    ms = offsetMs % 1000
    totalSec = int(offsetMs / 1000)
    s = totalSec % 60
    m = int(totalSec / 60) % 60
    h = int(totalSec / 3600) % 24
    return sprintf("2025-01-01T%02d:%02d:%02d.%03dZ", h, m, s, ms)
}

function emit_line(i,    r, pairIndex, id, duration) {
    if (i in startAtLine) {
        id = startAtLine[i]
        print fmt_ts(startOffsetById[id]), id, "Request started", "known payload"
        return
    }
    if (i in endAtLine) {
        id = endAtLine[i]
        print fmt_ts(completionOffsetById[id]), id, "Request completed", "known payload"
        return
    }

    r = i % 10
    if (r == 3) { print ""; return }
    if (r == 7) { print "this line is corrupted and has no valid shape"; return }
    if (r < 7) {
        printf "%s uniq%09d Request started load-generator-payload\n", fmt_ts(i % 3600000), i
        return
    }

    pairIndex = int(i / 10)
    if (r == 8) {
        noiseStartOffset[pairIndex] = (pairIndex * 977) % 3600000
        printf "%s pair%08d Request started go\n", fmt_ts(noiseStartOffset[pairIndex]), pairIndex
    } else {
        duration = 1 + int(rand() * 100000)   # шум: до 100 сек, заведомо меньше подсаженных пар
        printf "%s pair%08d Request completed done\n", fmt_ts(noiseStartOffset[pairIndex] + duration), pairIndex
    }
}

BEGIN {
    srand(seed)

    for (k = 0; k < knownTopK; k++) {
        startLine = int(k * lines / knownTopK)
        endLine   = startLine + int(lines / (2 * knownTopK)) + 1
        if (endLine >= lines) endLine = lines - 1

        id = sprintf("known-pair-%02d", k)
        startOffsetMs = k * 2000000                    # заведомо малое, разное для каждого k
        duration      = 50000000 - k * 1000000          # заведомо больше любого шума (макс. шум ~100000)

        startAtLine[startLine]   = id
        startOffsetById[id]      = startOffsetMs
        endAtLine[endLine]       = id
        completionOffsetById[id] = startOffsetMs + duration
    }

    if (expectedOut != "") {
        for (k = 0; k < knownTopK; k++) {
            id = sprintf("known-pair-%02d", k)
            duration = 50000000 - k * 1000000
            print id, duration >> expectedOut
        }
        close(expectedOut)
    }

    # Цикл управляется явно (а не через NR по входным записям) — у
    # этого awk-скрипта нет входного потока вообще, он сам порождает
    # ровно `lines` строк на stdout, одну за другой.
    for (i = 0; i < lines; i++) {
        emit_line(i)
    }
}
'
