CC=gcc

BUILDDIR=build

server: builddir server.c
	$(CC) -o $(BUILDDIR)/server server.c

builddir:
	mkdir -p $(BUILDDIR)

clean:
	rm -r $(BUILDDIR)
