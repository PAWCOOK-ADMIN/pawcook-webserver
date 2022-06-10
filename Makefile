a.out: ./src/*.cpp
	g++ ./src/*.cpp -g -o a.out  -pthread -I ./include

.PHONY:clean

clean:
	rm a.out