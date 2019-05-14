all: p3_grader

autograder_main.o: autograder_main.c
	gcc -c -o autograder_main.o autograder_main.c

p3_grader: autograder_main.o threads.o
	gcc -o p3_grader autograder_main.o threads.o

clean:
	rm -f p3_grader *.o
