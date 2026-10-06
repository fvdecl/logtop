#!/usr/bin/env bash
# Большой синтетический лог для проверки памяти и времени. Генерирует лог заданного
# размера (в строках) через generate_log.sh и прогоняет logtop через measure.sh:
# сверка top-5 с известными генератору парами и измерение пика RSS/времени
# через /usr/bin/time -v.
#
# По умолчанию ~20 млн строк (~1,2 ГБ). Для 10 ГБ: tests/big_log_test.sh 200000000
# Сначала соберите проект: cmake -S . -B build && cmake --build build
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BINARY="${BINARY:-$PROJECT_DIR/build/logtop}"
lines="${1:-20000000}"

if [[ ! -x "$BINARY" ]]; then
    echo "Бинарник $BINARY не найден. Соберите: cmake -S $PROJECT_DIR -B $PROJECT_DIR/build && cmake --build $PROJECT_DIR/build" >&2
    exit 1
fi

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

echo "Генерация $lines строк лога в $workdir/big.log ..."
"$SCRIPT_DIR/generate_log.sh" --lines "$lines" --expected-output "$workdir/expected.txt" > "$workdir/big.log"
ls -lh "$workdir/big.log"

echo
echo "Измерение (peak RSS / время / корректность) ..."
"$SCRIPT_DIR/measure.sh" --binary "$BINARY" --input "$workdir/big.log" --expected "$workdir/expected.txt"

# Чтобы проверить RAM < 128 МБ как ЖЁСТКИЙ предел (ядро реально убьёт
# процесс при превышении), а не просто понаблюдать за пиком, запустите
# ЗАПУСК (не сборку!) под настоящим cgroup-лимитом. Лимит должен
# ограничивать только сам logtop — сборка компилятором g++ сама по
# себе требует существенно больше 128 МБ (это ограничение относится к
# рантайму программы, не к её компиляции), поэтому собирать нужно ДО
# входа в ограниченный контейнер/scope, например через Docker:
#
#   docker run --rm -v "$PWD:/work" -w /work debian:trixie-slim bash -c '
#       apt-get update -qq && apt-get install -y -qq g++ cmake make >/dev/null
#       cmake -S . -B build && cmake --build build -j
#   '
#   docker run --rm --memory=128m --memory-swap=128m \
#       -v "$PWD:/work" -w /work debian:trixie-slim bash -c '
#           apt-get update -qq && apt-get install -y -qq time gawk >/dev/null
#           tests/generate_log.sh --lines 20000000 --expected-output /tmp/expected.txt > /tmp/big.log
#           tests/measure.sh --input /tmp/big.log --expected /tmp/expected.txt
#       '
#
# или через systemd-run на хосте (бинарник уже собран, без Docker):
#   systemd-run --scope -p MemoryMax=128M --user \
#       tests/measure.sh --input big.log --expected expected.txt
