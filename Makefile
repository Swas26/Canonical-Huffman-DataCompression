CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -g
RELEASE_FLAGS = -std=c++17 -O2 -Wall -Wextra
DEPFLAGS = -MMD -MP
INCLUDES = -Isrc

# GoogleTest (brew install googletest). gtest_main supplies main() for every test binary.
GTEST_CFLAGS := $(shell pkg-config --cflags gtest_main)
GTEST_LIBS   := $(shell pkg-config --libs gtest_main)

SRC_DIR   = src
TEST_DIR  = tests
BUILD_DIR = build

SRC_FILES  = $(wildcard $(SRC_DIR)/*.cpp $(SRC_DIR)/*/*.cpp)
TEST_FILES = $(wildcard $(TEST_DIR)/*.cpp)

SRC_OBJS  = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SRC_FILES))
TEST_OBJS = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(TEST_FILES))

MAIN_OBJ = $(BUILD_DIR)/$(SRC_DIR)/main.o
LIB_OBJS = $(filter-out $(MAIN_OBJ),$(SRC_OBJS))

MAIN_TARGET = swas
# One binary per test file -- each links gtest_main, so no test .cpp has a main().
TEST_BINS = $(patsubst $(TEST_DIR)/%.cpp,$(BUILD_DIR)/bin/%,$(TEST_FILES))

all: $(MAIN_TARGET) $(TEST_BINS)

$(MAIN_TARGET): $(SRC_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Only test objects need the gtest headers.
$(TEST_OBJS): INCLUDES += $(GTEST_CFLAGS)

$(BUILD_DIR)/bin/%: $(BUILD_DIR)/$(TEST_DIR)/%.o $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(GTEST_LIBS)

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

# test_main runs the swas binary itself, so it is built first.
test: $(MAIN_TARGET) $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do \
		echo "===== $$t ====="; \
		./$$t || fail=1; \
		echo; \
	done; \
	if [ $$fail -ne 0 ]; then echo "SOME TESTS FAILED"; fi; \
	exit $$fail

# Optimised ./swas only. The objects are rebuilt with -O2; `make clean` goes back to debug.
release: clean
	$(MAKE) $(MAIN_TARGET) CXXFLAGS="$(RELEASE_FLAGS)"

# Encode/decode timings and a round-trip check on the sample corpus.
bench: $(MAIN_TARGET)
	./$(MAIN_TARGET) b tests/data/big.txt

clean:
	rm -rf $(BUILD_DIR) $(MAIN_TARGET)

-include $(SRC_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

.PHONY: all test release bench clean

# Keep object files; without this make treats them as intermediates and deletes them.
.SECONDARY:
