# Compiler and flags
# CHANGE TO -O2 later on
CC = gcc
CFLAGS = -Wall -Wextra -O0 -pedantic
LIBS = -lmicrohttpd -lsqlite3 -lcurl 

# Check if static linking is enabled (use make STATIC=1 to make it static)
# DO NOT USE YET, BROKEN!
ifeq ($(STATIC), 1)
    LDFLAGS = -static
else
    LDFLAGS = 
endif

# Targets
TARGETS = handlerd

# Development mode (handlerd-dev with address sanitizer and port 8507)
# NEW DEV ADDITIONS
MODE ?= 0
PORT_DEV = 8507
PORT_PROD = 8123
EXE_DEV = handlerd-dev
EXE_PROD = handlerd

# Source files
SRCS = main.c

# Object files
OBJS = $(SRCS:.c=.o)

# Default target
all: $(TARGETS)

# Build handlerd or handlerd-dev (based on DEV mode)
# NEW DEV ADDITIONS
$(TARGETS): $(OBJS)
ifeq ($(MODE), 1)
	$(CC) $(CFLAGS) -DPORT=$(PORT_DEV) -o $(EXE_DEV) $^ $(LDFLAGS) $(LIBS) -fsanitize=address
else
	$(CC) $(CFLAGS) -DPORT=$(PORT_PROD) -o $(EXE_PROD) $^ $(LDFLAGS) $(LIBS)
endif

# Build handlerd
# handlerd: $(OBJS)
#	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

# Compile .c files into .o files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build artifacts
clean:
	rm -f $(OBJS) $(TARGETS)

.PHONY: all clean
