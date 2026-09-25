CXX      := g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -pedantic
TARGET   := logtop

.PHONY: all clean test test-big

all: $(TARGET)

$(TARGET): main.cpp
	$(CXX) $(CXXFLAGS) -o $(TARGET) main.cpp

# Быстрый функциональный набор тестов (кейсы 1-16, 18).
test: $(TARGET)
	bash tests/run_tests.sh

# Тяжёлый тест на большом синтетическом логе (кейс 17): peak RSS,
# время выполнения, точная сверка результата. Аргумент - число строк
# лога (по умолчанию ~20 млн, ~1-1.3 ГБ); например:
#   make test-big LINES=200000000
test-big: $(TARGET)
	bash tests/big_log_test.sh $(LINES)

clean:
	rm -f $(TARGET)
