CC = gcc
CFLAGS = 
LIBS = -DENABLE_DEBUG -lpigpio -lpthread 

TARGET_MASTER = master
TARGET_SLAVE = slave
LOG_FILE = sync_log.txt

.PHONY: all master slave clean

all:
	@echo "Specify 'make master' or 'make slave'"

master: $(TARGET_MASTER)

slave: $(TARGET_SLAVE)

$(TARGET_MASTER): master_sync.c
	$(CC) $(CFLAGS) -o $@ 

$(TARGET_SLAVE): slave_sync.c
	@rm -f $(LOG_FILE)
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f $(TARGET_MASTER) $(TARGET_SLAVE)  $(LOG_FILE)
