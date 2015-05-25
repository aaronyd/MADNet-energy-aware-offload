CC	= g++

CFLAGS	= -g -pthread -lproj -Wall
OBJ	= 3g_proxy
SRC	= 3g_proxy.cpp 
HDR	= proxy.h

all:	$(OBJ)
default:	$(OBJ)

$(OBJ):		$(HDR) $(SRC)
		$(CC) $(CFLAGS) -o $(OBJ) $(SRC)

clean:
		rm $(OBJ) 

