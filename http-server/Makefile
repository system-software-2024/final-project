DIRS = io-uring multi-process epoll

.PHONY: all $(DIRS) clean

all: $(DIRS)

$(DIRS):
	make -C $@

clean: 
	@for dir in $(DIRS); do \
		make -C $$dir clean; \
	done
