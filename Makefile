CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -g -O2 -pthread -D_GNU_SOURCE -Isrc $(EXTRA_CFLAGS)
LDFLAGS := -pthread $(EXTRA_LDFLAGS)

# 메모리 오류(AddressSanitizer)·누수(LeakSanitizer)·정의되지 않은 동작(UBSan)을 잡는다
SAN := -fsanitize=address,leak,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

BUILD ?= build
TBIN  ?= tests/bin

BIN      := webserver
SRCS     := $(wildcard src/*.c)
LIB_SRCS := $(filter-out src/main.c, $(SRCS))
LIB_OBJS := $(patsubst src/%.c, $(BUILD)/%.o, $(LIB_SRCS))
TESTS    := $(patsubst tests/%.c, $(TBIN)/%, $(wildcard tests/test_*.c))

PORT ?= 8080
ROOT ?= www

.PHONY: all test asan functional run clean

all: $(BIN)

$(BIN): $(BUILD)/main.o $(LIB_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -c $< -o $@

$(BUILD) $(TBIN):
	mkdir -p $@

$(TBIN)/%: tests/%.c $(LIB_OBJS) | $(TBIN)
	$(CC) $(CFLAGS) -Itests $^ -o $@ $(LDFLAGS) $(TEST_LDFLAGS)

# test_response 만: write 를 테스트 안의 가짜(__wrap_write)로 바꿔 끼워 부분 쓰기·EINTR 를 재현한다
$(TBIN)/test_response: TEST_LDFLAGS := -Wl,--wrap=write

# 테스트 실행 파일을 하나씩 돌리고, 하나라도 실패하면 make 가 실패한다
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

asan:
	$(MAKE) test BUILD=build/asan TBIN=tests/bin/asan EXTRA_CFLAGS="$(SAN)" EXTRA_LDFLAGS="$(SAN)"

# 실제 서버를 띄워 curl·nc 로 확인하는 기능 테스트 (curl, nc, python3 필요)
functional: $(BIN)
	bash tests/functional.sh ./$(BIN)

run: $(BIN)
	./$(BIN) -p $(PORT) -r $(ROOT)

clean:
	rm -rf build tests/bin $(BIN)

-include $(wildcard $(BUILD)/*.d)
