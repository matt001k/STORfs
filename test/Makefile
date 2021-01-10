CC = gcc

CFLAGS = -g -Wall -Wno-format

# C Sources
C_SOURCES = \
main.c \
../src/storfs.c

# C Includes
C_INCLUDES = \
-I../test \
-I../include 

# Build Path
BUILD_DIR = build

# Target
TARGET = test

# C Objects
C_OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))

LDFLAGS = -Xlinker -Map=$(BUILD_DIR)/$(TARGET).map

# Build the executable
all: $(BUILD_DIR)/$(TARGET)

$(BUILD_DIR)/%.o: %.c  | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(C_INCLUDES) -c $< -o $@

$(BUILD_DIR)/$(TARGET): $(C_OBJECTS)
	$(CC) $(CFLAGS) $(C_INCLUDES) -o $(BUILD_DIR)/$(TARGET) $(C_OBJECTS)
	
$(BUILD_DIR):
	mkdir $@

clean:
	-rm -fr $(BUILD_DIR)

### EOF ###