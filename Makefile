
all: record_build 

record_build: record_build.c
	gcc -g -o record_build record_build.c

clean:
	rm record_build
