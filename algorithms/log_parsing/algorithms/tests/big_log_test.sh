#!/usr/bin/env bash
# Кейс 17: большой синтетический лог для проверки памяти. Генерирует
# лог заданного размера (в строках) через generate_log.sh (потоково, без
# накопления в RAM) и прогоняет logtop через measure.sh — с точной
# сверкой top-5 (по "подсаженным" в генераторе известным парам) и
# измерением пика RSS/времени через /usr/bin/time -v.
#
# По умолчанию ~20 млн строк (~1-1.3 ГБ в зависимости от профиля
# сборки). Для полноценной проверки на 10 ГБ:
#   tests/big_log_test.sh 200000000
# (см. также README-заметку в конце файла про --memory=128m).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BINARY="${BINARY:-$PROJECT_DIR/logtop}"
lines="${1:-20000000}"

if [[ ! -x "$BINARY" ]]; then
    echo "Бинарник $BINARY не найден — собираю (make -C $PROJECT_DIR) ..." >&2
    make -C "$PROJECT_DIR" >&2 || { echo "big_log_test.sh: сборка не удалась" >&2; exit 1; }
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
#   docker run --rm -v "$PWD:/work" -w /work debian:12-slim bash -c '
#       apt-get update -qq && apt-get install -y -qq time g++ make >/dev/null
#       make
#   '
#   docker run --rm --memory=128m --memory-swap=128m \
#       -v "$PWD:/work" -w /work debian:12-slim bash -c '
#           apt-get update -qq && apt-get install -y -qq time >/dev/null
#           tests/generate_log.sh --lines 20000000 --expected-output /tmp/expected.txt > /tmp/big.log
#           tests/measure.sh --input /tmp/big.log --expected /tmp/expected.txt
#       '
#
# или через systemd-run на хосте (бинарник уже собран, без Docker):
#   systemd-run --scope -p MemoryMax=128M --user \
#       tests/measure.sh --input big.log --expected expected.txt
