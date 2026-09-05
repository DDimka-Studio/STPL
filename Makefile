CC = gcc
CFLAGS = -std=gnu11 -Wall -Wextra -O2
COMPILER = first-time-stpl

.PHONY: all clean test test-compiler

all: $(COMPILER)

# Компилятор линковки с рантаймом не требует (он сам порождает C и вызывает
# gcc отдельно на каждый .stpl файл) — stpl_rt.c/.h ему нужны только рядом
# с бинарником first-time-stpl (он ищет их через /proc/self/exe).
$(COMPILER): first-time-stpl.c stpl_rt.c stpl_rt.h
	$(CC) $(CFLAGS) -o $(COMPILER) first-time-stpl.c

clean:
	rm -f $(COMPILER) examples/*.gen.c examples/logic examples/term examples/calculator examples/fibonacci examples/hexpack examples/reverse examples/sync_counter examples/strreturn

test: test-compiler

test-compiler: $(COMPILER)
	@echo "--- [compiler] logic.stpl -> binary -> run ---"
	@./$(COMPILER) examples/logic.stpl -o examples/logic
	@./examples/logic; echo "exit code: $$?"
	@echo "--- [compiler] term.stpl -> binary -> run, TERM=$(TERM) ---"
	@./$(COMPILER) examples/term.stpl -o examples/term
	@./examples/term; echo "exit code: $$?"
	@echo "--- [compiler] calculator.stpl -> binary -> run, input: 6, 1 (+), 7 ---"
	@./$(COMPILER) examples/calculator.stpl -o examples/calculator
	@printf "6\n1\n7\n" | ./examples/calculator; echo "exit code: $$?"
	@echo "--- [compiler] fibonacci.stpl -> binary -> run (recursion, fib(10) expected: 55) ---"
	@./$(COMPILER) examples/fibonacci.stpl -o examples/fibonacci
	@./examples/fibonacci; echo "exit code: $$?"
	@echo "--- [compiler] hexpack.stpl -> binary -> run (map + bitwise, expected: 165 / 10 / 5) ---"
	@./$(COMPILER) examples/hexpack.stpl -o examples/hexpack
	@./examples/hexpack; echo "exit code: $$?"
	@echo "--- [compiler] reverse.stpl -> binary -> run (str module, expected: noisrucer) ---"
	@./$(COMPILER) examples/reverse.stpl -o examples/reverse
	@./examples/reverse; echo "exit code: $$?"
	@echo "--- [compiler] sync_counter.stpl -> binary -> run (2 real threads, expected: 400000) ---"
	@./$(COMPILER) examples/sync_counter.stpl -o examples/sync_counter
	@timeout 30 ./examples/sync_counter; echo "exit code: $$?"
	@echo "--- [compiler] strreturn.stpl -> binary -> run (function returning a string, expected: zero/one/unknown) ---"
	@./$(COMPILER) examples/strreturn.stpl -o examples/strreturn
	@./examples/strreturn; echo "exit code: $$?"
