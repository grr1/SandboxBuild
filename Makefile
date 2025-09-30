
all: record_build 

record_build: record_build.c
	gcc -g -o record_build record_build.c

clean:
	rm record_build

test: record_build hello_world/hello.c
	cd hello_world
	make clean
	pwd
	./record_build make
