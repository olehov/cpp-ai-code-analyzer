PROJECT := analyzer

CXX := g++
CXXFLAGS := -std=c++23 -Wall -Wextra -pedantic -Icpp/include
LDFLAGS :=

SRC_DIR := cpp/src
OBJ_DIR := build
SOURCES := $(wildcard $(SRC_DIR)/*.cpp)
OBJECTS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(SOURCES))
DEPS := $(OBJECTS:.o=.d)

.PHONY: all clean fclean run re ai

all: $(PROJECT)

$(PROJECT): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

run: $(PROJECT)
	./$(PROJECT) .

ai: $(PROJECT)
	./$(PROJECT) .

clean:
	rm -rf $(OBJ_DIR)

fclean: clean
	rm -f $(PROJECT)

re: fclean all

-include $(DEPS)
