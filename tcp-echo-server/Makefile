DIRS = epoll io-uring io-uring-provide-buffer multi-process

.PHONY: all $(DIRS) clean

all: $(DIRS)

$(DIRS):
	make -C $@

clean: 
	@for dir in $(DIRS); do \
		make -C $$dir clean; \
	done
