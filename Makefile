ALL_CFLAGS=$(CFLAGS) -Wall -Wextra -Wshadow -Wconversion -Wpedantic -pedantic -std=gnu99 \
	-Wno-unused-function -Wno-unused-parameter -Wimplicit-fallthrough -Wno-format-truncation -Wno-unknown-warning-option \
	`pkg-config --libs --cflags gtk+-3.0` -rdynamic -fno-strict-aliasing
DEBUG_CFLAGS=$(ALL_CFLAGS) -DDEBUG -O0 -g
RELEASE_CFLAGS=$(ALL_CFLAGS) -Ofast -g
PROFILE_CFLAGS=$(ALL_CFLAGS) -Ofast -g -DPROFILE=1
NAME=pokemem
$(NAME): *.[ch]
	$(CC) main.c -o $(NAME) $(DEBUG_CFLAGS)
release: *.[ch]
	$(CC) main.c -o $(NAME) $(RELEASE_CFLAGS)
clean:
	rm -f $(NAME)
