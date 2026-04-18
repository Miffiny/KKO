CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Iinclude -pthread 
SRC_DIR := src
OBJ_DIR := obj
BIN := lz_codec

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(SRCS))

.PHONY: all clean doc zip

all: test

# --- PREKLAD ---

$(BIN): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) $(BIN)


# --- TESTY ---

DATA := $(wildcard data/*.raw)
TMP := tmp

test: $(BIN)
	@mkdir -p $(TMP)
	@for mode in 0 1 2 3; do \
		case $$mode in \
			0) OPTS="" ;; \
			1) OPTS="-m" ;; \
			2) OPTS="-a" ;; \
			3) OPTS="-a -m" ;; \
		esac; \
		echo "Režim $$mode: $$OPTS"; \
		for f in $(DATA); do \
			echo "  Soubor $$f"; \
			orig_size=$$(wc -c < $$f); \
			rm -f $(TMP)/out*; \
			./$(BIN) -c -i $$f -o $(TMP)/out.lz -w 256 $$OPTS || exit 1; \
			comp_size=$$(wc -c < $(TMP)/out.lz); \
			ratio=$$(echo "scale=2; $$comp_size / $$orig_size" | bc); \
			echo "    Komprese: $$orig_size B → $$comp_size B  (poměr $$ratio×)"; \
			./$(BIN) -d -i $(TMP)/out.lz -o $(TMP)/out.raw || exit 1; \
			cmp -s $$f $(TMP)/out.raw || { \
				echo "CHYBA: Výstup se liší od originálu u souboru $$f"; \
				exit 1; \
			}; \
		done; \
	done
	@echo "=== Všechny testy prošly ==="


