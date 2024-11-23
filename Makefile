# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -O2 -pedantic
LIBS = -lmicrohttpd -lsqlite3

# Check if static linking is enabled (use make STATIC=1 to make it static)
# DO NOT USE YET, BROKEN!
ifeq ($(STATIC), 1)
    LDFLAGS = -static
else
    LDFLAGS = 
endif

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
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

# Compile .c files into .o files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build artifacts
clean:
	rm -f $(OBJS) $(TARGETS)

.PHONY: all clean
