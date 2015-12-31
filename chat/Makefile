CROSS = 
export CROSS
export CC  = $(CROSS)gcc 
export CXX = $(CROSS)g++ 
export AR := $(CROSS)ar
export AS := $(CROSS)as
export STRIP := $(CROSS)strip
export CPP = $(CC) 
export OBJCOPY  = $(CROSS)objcopy
export OBJDUMP  = $(CROSS)objdump
export NM       = $(CROSS)nm
export LD       = $(CROSS)ld 

image = chat

CPPFLAGS=  
CFG_INC= -I$(shell pwd)/inc -I$(shell pwd)
LIBPATH= -L$(shell pwd)/lib

LIB = -lbdasr -ljson
#CFLAGS = $(CFG_INC) $(CPPFLAGS) -g -o2
#CFLAGS = $(TARGET_CFLAGS) $(LDFLAGS)

CXX_IMG_OBJS =$(patsubst %.cpp,%.o,$(wildcard *.cpp))
CC_IMG_OBJS =$(patsubst %.c,%.o,$(wildcard *.c))

all: $(image)

$(image): $(CXX_IMG_OBJS) $(CC_IMG_OBJS)
	echo "link $< ..."
	$(CC) -o $@ $^  $(CFG_INC) $(LIBPATH) $(LIB) $(LDFLAGS)

%.o:%.c
	@echo "Compiling $< ..."
	$(CC) -c -Wall $(CFLAGS) -o $@ $<

%.o:%.cpp
	@echo "Compiling $< ..."
	$(CXX) -c -Wall $(CFLAGS) -o $@ $<

clean:
	rm -f *.d *.o $(image)
