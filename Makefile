all:
	gcc -Wall -Wextra -pedantic -std=c99 memdiff.c -o memdiff

debug:
	gcc -g -Wall -Wextra -pedantic -std=c99 memdiff.c -o memdiff

.PHONY: clean cleansnaps cleandiffs
clean:
	rm memdiff

cleansnaps:
	rm pid*_snap*

cleandiffs:
	rm *.memdiff
