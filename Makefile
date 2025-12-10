
all: record_build record_clang

record_build: record_build.c
	gcc -g -o record_build record_build.c

record_clang: record_clang.c
	gcc -g -o record_clang record_clang.c

clean:
	rm record_build record_clang
