CC=gcc

BUILDDIR=build

server: builddir server.c
	$(CC) -o $(BUILDDIR)/server server.c

builddir:
	mkdir $(BUILDDIR)

clean:
	rm -r $(BUILDDIR)
