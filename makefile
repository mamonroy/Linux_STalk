makeall:
	gcc -std=c11 -o s-talk instructorList.o stalk.c -Wall -Werror -lpthread

clean:
	rm s-talk
