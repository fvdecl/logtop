#!/usr/bin/env bash
# Измеряет пик RSS, время выполнения и (опционально) точную корректность
# результата для одного прогона logtop поверх заданного файла. Использует
# GNU time (/usr/bin/time -v) в раздельные файлы: отчёт time, stdout
# программы и stderr программы не перемешиваются.
set -uo pipefail

binary="./logtop"
input=""
expected=""
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

usage() {
    cat >&2 <<EOF
Usage: $(basename "$0") --input FILE [--binary PATH] [--expected FILE]

  --input FILE     входной лог (обязательно)
  --binary PATH    путь к бинарнику logtop (по умолчанию: $binary)
  --expected FILE  ожидаемый вывод (traceId duration_ms, по убыванию) для
                   точной сверки; если не задан — только структурная проверка
                   (не более 5 строк, длительности строго убывают)
EOF
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --input) input="$2"; shift 2 ;;
        --binary) binary="$2"; shift 2 ;;
        --expected) expected="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "measure.sh: неизвестный аргумент: $1" >&2; usage ;;
    esac
done
[[ -n "$input" ]] || usage

if ! command -v /usr/bin/time >/dev/null 2>&1; then
    echo "measure.sh: /usr/bin/time не найден (Debian/Ubuntu: apt-get install time; Alpine: apk add time)" >&2
    exit 2
fi

stdoutFile="$workdir/stdout.txt"
stderrFile="$workdir/stderr.txt"
timeReportFile="$workdir/time-report.txt"

/usr/bin/time -v -o "$timeReportFile" "$binary" < "$input" > "$stdoutFile" 2> "$stderrFile"
exitCode=$?

peakRssKb=$(awk -F': ' '/Maximum resident set size/{print $2}' "$timeReportFile")
elapsed=$(awk -F': ' '/Elapsed \(wall clock\)/{print $2}' "$timeReportFile")

echo "exit code:     $exitCode"
echo "peak RSS:      ${peakRssKb:-?} KB"
echo "elapsed time:  ${elapsed:-?}"
echo "program stderr:"
sed 's/^/  /' "$stderrFile"

correctness="not checked (no --expected given)"
mismatch=0
if [[ -n "$expected" ]]; then
    if diff -u <(sort "$expected") <(sort "$stdoutFile") > "$workdir/diff.txt"; then
        correctness="OK (exact match against $expected)"
    else
        correctness="MISMATCH"
        mismatch=1
    fi
else
    if awk '
        { if ($2 !~ /^[0-9]+$/) bad = 1
          if (NR > 1 && $2 >= prev) bad = 1
          prev = $2 }
        END { if (NR > 5) bad = 1; exit bad ? 1 : 0 }
    ' "$stdoutFile"; then
        correctness="OK (structural: <=5 lines, strictly descending durations)"
    else
        correctness="STRUCTURAL CHECK FAILED"
        mismatch=1
    fi
fi
echo "correctness:   $correctness"

if [[ $mismatch -eq 1 || $exitCode -ne 0 ]]; then
    echo "--- actual stdout ---"
    cat "$stdoutFile"
    [[ -f "$workdir/diff.txt" ]] && { echo "--- diff (expected vs actual) ---"; cat "$workdir/diff.txt"; }
    exit 1
fi
