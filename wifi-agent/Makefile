CC	= g++

CFLAGS	= -g -pthread -Wall
OBJ	= wifi_proxy
SRC	= wifi_proxy.cpp
HDR	= proxy.h

all:	$(OBJ)
default:	$(OBJ)

$(OBJ):		$(HDR) $(SRC)
		$(CC) $(CFLAGS) -o $(OBJ) $(SRC)

clean:
		rm $(OBJ) 

