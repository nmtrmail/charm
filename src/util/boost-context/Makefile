include ../uFcontext.mk

CDIR=../../
CHARMC=../../bin/charmc

.SUFFIXES : .S .o

all: jump_${TARGET}.o make_${TARGET}.o ontop_${TARGET}.o #libuFcontext_asm.a

.S.o:
	$(CHARMC) -c $<
clean:
	rm *.o *.a