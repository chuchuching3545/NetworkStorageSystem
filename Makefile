CC = g++
OPENCV =  `pkg-config --cflags --libs opencv`
PTHREAD = -pthread

CLIENT = client.cpp
SERVER = server.cpp
CLI = client
SER = server

all: server client
  
server: $(SERVER)
	sudo g++ -I ./ffmpeg-lib/inc server.cpp -L ./ffmpeg-lib/lib -lavdevice -lavfilter -lavformat -lavcodec -lswresample -lswscale -lavutil -o server -pthread `pkg-config --cflags --libs opencv`
 
client: $(CLIENT)
	sudo g++ -I ./ffmpeg-lib/inc client.cpp -L ./ffmpeg-lib/lib -lavdevice -lavfilter -lavformat -lavcodec -lswresample -lswscale -lavutil -o client -pthread `pkg-config --cflags --libs opencv`

.PHONY: clean

clean:
	rm $(CLI) $(SER)