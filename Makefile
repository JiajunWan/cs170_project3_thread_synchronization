all: p3_grader

p3_grader: autograder_main.c thread_lib
	gcc autograder_main.c threads.o -o p3_grader

thread_lib: pthread.c
	gcc -c pthread.c -o threads.o

clean:
	rm -f p3_grader *.o
