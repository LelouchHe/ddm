
EXE = test
SRC = $(wildcard *.c)
OBJS = $(SRC:%.c=%.o)

CC = gcc

CFLAGS = -g -Wall
INCLUDE =
LDFLAGS =

all: $(EXE)


$(EXE): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

.c.o:
	$(CC) $(CFLAGS) $(INCLUDE) -c $^

clean:
	rm -f $(EXE) $(OBJS)
