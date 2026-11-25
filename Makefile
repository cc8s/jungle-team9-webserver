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

WORKER ?= 4
QUEUE ?= 64
MAX ?= 128
BUFFER ?= 32
IMAGE ?= jungle-f2

.PHONY: all test test-f2 asan run clean docker-build docker-test

all: $(BIN)

$(BIN): $(BUILD)/main.o $(LIB_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -c $< -o $@

$(BUILD) $(TBIN):
	mkdir -p $@

$(TBIN)/%: tests/%.c $(LIB_OBJS) | $(TBIN)
	$(CC) $(CFLAGS) -Itests $^ -o $@ $(LDFLAGS)

# 테스트 실행 파일을 하나씩 돌리고, 하나라도 실패하면 make 가 실패한다
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done
	python3 tests/tools/test_upload_fixture.py

asan:
	$(MAKE) test BUILD=build/asan TBIN=tests/bin/asan EXTRA_CFLAGS="$(SAN)" EXTRA_LDFLAGS="$(SAN)"

run: $(BIN)
	./$(BIN) --worker $(WORKER) --queue $(QUEUE) --max $(MAX) --buffer $(BUFFER)

clean:
	rm -rf build tests/bin $(BIN)

-include $(wildcard $(BUILD)/*.d)

# Linux actual-main integration tests (port 8000 must be available).
test-f2: $(BIN)
	python3 tests/tools/test_epoll_server.py

docker-build:
	docker build -t $(IMAGE) .

docker-test: docker-build
	docker run --rm $(IMAGE) sh -c 'make test && make test-f2'
