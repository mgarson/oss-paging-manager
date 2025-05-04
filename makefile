CC = g++ -g3
CFLAGS = -g3
TARGET1 = oss
TARGET2 = worker

OBJS1	= oss.o
OBJS2	= worker.o

all:	$(TARGET1) $(TARGET2)

$(TARGET1):	$(OBJS1)
	$(CC) -o $(TARGET1) $(OBJS1)

$(TARGET2):	$(OBJS2)
	$(CC) -o $(TARGET2) $(OBJS2)

oss.o:		oss.cpp
	$(CC) $(CFLAGS) -c oss.cpp

worker.o:	worker.cpp
	$(CC) $(CFLAGS) -c worker.cpp

clean:
	/bin/rm -f *.o $(TARGET1) $(TARGET2)
