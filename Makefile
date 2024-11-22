# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -O2 -pedantic
LIBS = -lmicrohttpd -lsqlite3

# Targets
TARGETS = handlerd

# Source files
SRCS = main.c

# Object files
OBJS = $(SRCS:.c=.o)

# Default target
all: $(TARGETS)

# Build handlerd
handlerd: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

# Compile .c files into .o files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build artifacts
clean:
	rm -f $(OBJS) $(TARGETS)

.PHONY: all clean