CXX      := g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -pedantic
TARGET   := logtop

.PHONY: all clean test test-unit test-big

all: $(TARGET)

$(TARGET): main.cpp
	$(CXX) $(CXXFLAGS) -o $(TARGET) main.cpp

# Юнит-тесты разбора timestamp (сверка с эталоном timegm/gmtime_r).
test-unit: tests/timestamp_unit_test.cpp main.cpp
	$(CXX) $(CXXFLAGS) -o timestamp_unit_test tests/timestamp_unit_test.cpp
	./timestamp_unit_test

# Функциональный набор тестов дважды: в обычном режиме и с принудительным
# переносом открытых запросов на диск (--max-open-requests=1).
test: $(TARGET) test-unit
	bash tests/run_tests.sh
	LOGTOP_EXTRA_ARGS=--max-open-requests=1 bash tests/run_tests.sh

# Тяжёлый тест на большом синтетическом логе (кейс 17): peak RSS,
# время выполнения, точная сверка результата. Аргумент - число строк
# лога (по умолчанию ~20 млн, ~1-1.3 ГБ); например:
#   make test-big LINES=200000000
test-big: $(TARGET)
	bash tests/big_log_test.sh $(LINES)

clean:
	rm -f $(TARGET) timestamp_unit_test
