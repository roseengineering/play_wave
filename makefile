CFLAGS?=-O2 -g -Wall
LDLIBS+= -lmirsdrapi-rsp
CC?=gcc
PROGNAME=play_wave

all: $(PROGNAME)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

$(PROGNAME): $(PROGNAME).o
	$(CC) -g -o $@ $^ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f *.o $(PROGNAME)
