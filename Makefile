# Makefile

CFLAGS += -g -W -Wall -pedantic
LDFLAGS += -g

casbas_proba: casbas
	./casbas proba.bas proba.cas
	./casbas proba.cas tmp.bas
	./casbas tmp.bas   tmp.cas
	if cmp proba.cas tmp.cas; then echo OK; else echo Fail; fi
