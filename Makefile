# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -O2
LIB_HTTPD = -lmicrohttpd
LIB_DB = -lsqlite3

# Targets
TARGETS = handlerd addhandle

# Source files
SRCS_HANDLERD = handlerd.c
SRCS_ADDHANDLE = addhandle.c

# Object files
OBJS_HANDLERD = $(SRCS_HANDLERD:.c=.o)
OBJS_ADDHANDLE = $(SRCS_ADDHANDLE:.c=.o)

# Default target
all: $(TARGETS)

# Build handlerd
handlerd: $(OBJS_HANDLERD)
	$(CC) $(CFLAGS) -o $@ $^ $(LIB_HTTPD) $(LIB_DB)

# Build addhandle
addhandle: $(OBJS_ADDHANDLE)
	$(CC) $(CFLAGS) -o $@ $^ $(LIB_DB)

# Compile .c files into .o files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build artifacts
clean:
	rm -f $(OBJS_HANDLERD) $(OBJS_ADDHANDLE) $(TARGETS)

.PHONY: all clean
