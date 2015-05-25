CC	= g++

CFLAGS	= -g -pthread -lcurl -Wall -pedantic `pkg-config --cflags --libs hildon-1 hildon-fm-2 gtk+-2.0 gdk-2.0 gstreamer-0.10 liblocation` 
OBJ	= music_streamer
SRC	= music_streamer.cpp wifi_util.cpp
HDR	= music_streamer.h

all:	$(OBJ)
default:	$(OBJ)

$(OBJ):		$(HDR) $(SRC)
		$(CC) $(CFLAGS) -o $(OBJ) $(SRC)

clean:
		rm $(OBJ) 

