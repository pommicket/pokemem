ALL_CFLAGS=$(CFLAGS) -Wall -Wextra -Wshadow -Wconversion -Wpedantic -pedantic -std=gnu99 \
	-Wno-unused-function -Wno-unused-parameter -Wimplicit-fallthrough -Wno-format-truncation -Wno-unknown-warning-option \
	`pkg-config --libs --cflags gtk+-3.0` -rdynamic -fno-strict-aliasing
DEBUG_CFLAGS=$(ALL_CFLAGS) -DDEBUG -O0 -g
RELEASE_CFLAGS=$(ALL_CFLAGS) -Ofast -g
PROFILE_CFLAGS=$(ALL_CFLAGS) -Ofast -g -DPROFILE=1
NAME=pokemem
INSTALL_BIN_DIR=/usr/bin
GLOBAL_DATA_DIR=/usr/share/pokemem
$(NAME): *.[ch]
	$(CC) main.c -o $(NAME) $(DEBUG_CFLAGS)
release: *.[ch]
	$(CC) main.c -o $(NAME) $(RELEASE_CFLAGS)
clean:
	rm -f $(NAME)
pokemem.deb: release
	rm -rf /tmp/pokemem
	mkdir -p /tmp/pokemem/DEBIAN
	mkdir -p /tmp/pokemem$(INSTALL_BIN_DIR)
	mkdir -p /tmp/pokemem$(GLOBAL_DATA_DIR)
	mkdir -p /tmp/pokemem/usr/share/applications
	cp pokemem.desktop /tmp/pokemem/usr/share/applications
	cp pokemem /tmp/pokemem$(INSTALL_BIN_DIR)/
	cp -r ui.glade /tmp/pokemem$(GLOBAL_DATA_DIR)/
	cp control /tmp/pokemem/DEBIAN
	dpkg-deb --build /tmp/pokemem
	mv /tmp/pokemem.deb ./
