CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -g
DEPFLAGS = -MMD -MP
INCLUDES = -Isrc

SRC_DIR   = src
TEST_DIR  = tests
BUILD_DIR = build

SRC_FILES  = $(wildcard $(SRC_DIR)/*.cpp)
TEST_FILES = $(wildcard $(TEST_DIR)/*.cpp)

SRC_OBJS  = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SRC_FILES))
TEST_OBJS = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(TEST_FILES))

MAIN_OBJ = $(BUILD_DIR)/$(SRC_DIR)/main.o
LIB_OBJS = $(filter-out $(MAIN_OBJ),$(SRC_OBJS))

MAIN_TARGET = dc
# One binary per test file -- each test .cpp brings its own main().
TEST_BINS = $(patsubst $(TEST_DIR)/%.cpp,$(BUILD_DIR)/bin/%,$(TEST_FILES))

all: $(MAIN_TARGET) $(TEST_BINS)

$(MAIN_TARGET): $(SRC_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/bin/%: $(BUILD_DIR)/$(TEST_DIR)/%.o $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

test: $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do \
		echo "===== $$t ====="; \
		./$$t || fail=1; \
		echo; \
	done; \
	if [ $$fail -ne 0 ]; then echo "SOME TESTS FAILED"; fi; \
	exit $$fail

clean:
	rm -rf $(BUILD_DIR) $(MAIN_TARGET)

-include $(SRC_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

.PHONY: all test clean

# Keep object files; without this make treats them as intermediates and deletes them.
.SECONDARY:
