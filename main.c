#include <gtk/gtk.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <assert.h>
#include <ctype.h>
#include <wctype.h>

typedef pid_t PID;
typedef uint64_t Address;
#define SCNxADDR SCNx64
#define PRIdADDR PRId64
#define PRIxADDR PRIx64

#include "unicode.h"

// a memory map
typedef struct {
	Address lo, size;
} Map;

typedef enum {
	TYPE_U8,
	TYPE_S8,
	TYPE_U16,
	TYPE_S16,
	TYPE_U32,
	TYPE_S32,
	TYPE_U64,
	TYPE_S64,
	TYPE_ASCII,
	TYPE_UTF16,
	TYPE_UTF32,
	TYPE_F32,
	TYPE_F64
} DataType;

static DataType data_type_from_name(char const *name) {
	switch (name[0]) {
	case 'u':
		switch (atoi(&name[1])) {
		case 8:  return TYPE_U8;
		case 16: return TYPE_U16;
		case 32: return TYPE_U32;
		case 64: return TYPE_U64;
		}
		if (strncmp(name, "utf", 3) == 0) {
			switch (atoi(&name[3])) {
			case 16: return TYPE_UTF16;
			case 32: return TYPE_UTF32;
			}
		}
		break;
	case 's':
		switch (atoi(&name[1])) {
		case 8:  return TYPE_S8;
		case 16: return TYPE_S16;
		case 32: return TYPE_S32;
		case 64: return TYPE_S64;
		}
		break;
	case 'f':
		switch (atoi(&name[1])) {
		case 32: return TYPE_F32;
		case 64: return TYPE_F64;
		}
		break;
	case 'a':
		if (strcmp(name, "ascii") == 0)
			return TYPE_ASCII;
		break;
	}
	assert(0);
	return TYPE_U8;
}

static size_t data_type_size(DataType type) {
	switch (type) {
	case TYPE_U8:
	case TYPE_S8:
	case TYPE_ASCII:
		return 1;
	case TYPE_U16:
	case TYPE_S16:
	case TYPE_UTF16:
		return 2;
	case TYPE_U32:
	case TYPE_S32:
	case TYPE_F32:
	case TYPE_UTF32:
		return 4;
	case TYPE_U64:
	case TYPE_S64:
	case TYPE_F64:
		return 8;
	}
	return (size_t)-1;
}

// set str to "a" for 'a', "\\n" for '\n', "\\xff" for (wchar_t)255, etc.
static void char_to_str(uint32_t c, char *str, size_t str_size) {
	if (c <= WINT_MAX && iswgraph((wint_t)c)) {
		snprintf(str, str_size, "%lc", (wint_t)c);
	} else {
		switch (c) {
		case ' ': snprintf(str, str_size, "(space)"); break;
		case '\n': snprintf(str, str_size, "\\n"); break;
		case '\t': snprintf(str, str_size, "\\t"); break;
		case '\r': snprintf(str, str_size, "\\r"); break;
		case '\v': snprintf(str, str_size, "\\v"); break;
		case '\0': snprintf(str, str_size, "\\0"); break;
		default:
			if (c < 256)
				snprintf(str, str_size, "\\x%02x", (unsigned)c);
			else
				snprintf(str, str_size, "\\x%05lx", (unsigned long)c);
		}
	}
}

static bool char_from_str(char const *str, uint32_t *c) {
	if (str[0] == '\0') return false;
	if (str[0] == '\\') {
		switch (str[1]) {
		case 'n': *c = '\n'; return str[2] == '\0';
		case 't': *c = '\t'; return str[2] == '\0';
		case 'r': *c = '\r'; return str[2] == '\0';
		case 'v': *c = '\v'; return str[2] == '\0';
		case '0': *c = '\0'; return str[2] == '\0';
		case 'x': {
			unsigned long v = 0;
			int w = 0;
			if (sscanf(&str[2], "%lx%n", &v, &w) != 1 ||
				(size_t)w != strlen(&str[2]) || v > UINT32_MAX)
				return false;
			*c = (uint32_t)v;
			return true;
		}
		}
	}
	return unicode_utf8_to_utf32(c, str, strlen(str)) == strlen(str);
}

static void data_to_str(void const *value, DataType type, char *str, size_t str_size) {
	switch (type) {
	case TYPE_U8:  snprintf(str, str_size, "%" PRIu8,  *(uint8_t  *)value); break;
	case TYPE_U16: snprintf(str, str_size, "%" PRIu16, *(uint16_t *)value); break;
	case TYPE_U32: snprintf(str, str_size, "%" PRIu32, *(uint32_t *)value); break;
	case TYPE_U64: snprintf(str, str_size, "%" PRIu64, *(uint64_t *)value); break;
	case TYPE_S8:  snprintf(str, str_size, "%" PRId8,  *(int8_t  *)value);  break;
	case TYPE_S16: snprintf(str, str_size, "%" PRId16, *(int16_t *)value);  break;
	case TYPE_S32: snprintf(str, str_size, "%" PRId32, *(int32_t *)value);  break;
	case TYPE_S64: snprintf(str, str_size, "%" PRId64, *(int64_t *)value);  break;
	case TYPE_F32: snprintf(str, str_size, "%g", *(float  *)value);  break;
	case TYPE_F64: snprintf(str, str_size, "%g", *(double *)value);  break;
	case TYPE_UTF16: char_to_str(*(uint16_t *)value, str, str_size); break;
	case TYPE_UTF32: char_to_str(*(uint32_t *)value, str, str_size); break;
	case TYPE_ASCII:
		char_to_str((uint8_t)*(char *)value, str, str_size);
		break;
	
	}
}

// returns true on success, false if str is not a well-formatted value
static bool data_from_str(char const *str, DataType type, void *value) {
	int len = (int)strlen(str);
	int w = 0;
	uint32_t c = 0;
	switch (type) {
	case TYPE_U8:  return sscanf(str, "%" SCNu8  "%n", (uint8_t  *)value, &w) == 1 && w == len;
	case TYPE_S8:  return sscanf(str, "%" SCNd8  "%n", ( int8_t  *)value, &w) == 1 && w == len;
	case TYPE_U16: return sscanf(str, "%" SCNu16 "%n", (uint16_t *)value, &w) == 1 && w == len;
	case TYPE_S16: return sscanf(str, "%" SCNd16 "%n", ( int16_t *)value, &w) == 1 && w == len;
	case TYPE_U32: return sscanf(str, "%" SCNu32 "%n", (uint32_t *)value, &w) == 1 && w == len;
	case TYPE_S32: return sscanf(str, "%" SCNd32 "%n", ( int32_t *)value, &w) == 1 && w == len;
	case TYPE_U64: return sscanf(str, "%" SCNu64 "%n", (uint64_t *)value, &w) == 1 && w == len;
	case TYPE_S64: return sscanf(str, "%" SCNd64 "%n", ( int64_t *)value, &w) == 1 && w == len;
	case TYPE_F32: return sscanf(str, "%f%n",  (float *)value,  &w) == 1 && w == len;
	case TYPE_F64: return sscanf(str, "%lf%n", (double *)value, &w) == 1 && w == len;
	case TYPE_ASCII:
		if (!char_from_str(str, &c)) return false;
		if (c > 127) return false;
		*(uint8_t *)value = (uint8_t)c;
		return true;
	case TYPE_UTF16:
		if (!char_from_str(str, &c)) return false;
		if (c > 65535) return false;
		*(uint16_t *)value = (uint16_t)c;
		return true;
	case TYPE_UTF32:
		if (!char_from_str(str, &c)) return false;
		*(uint32_t *)value = c;
		return true;
	}
	assert(0);
	return false;
}

typedef struct {
	GtkWindow *window;
	GtkBuilder *builder;
	bool stop_while_accessing_memory;
	long editing_memory; // index of memory value being edited, or -1 if none is
	PID pid;
	Map *maps;
	Address memory_view_address;
	uint32_t memory_view_entries; // # of entries to show
	unsigned nmaps;
	DataType data_type;
} State;

static void display_dialog_box_nofmt(State *state, GtkMessageType type, char const *message) {
	GtkWidget *box = gtk_message_dialog_new(state->window,
		GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
		type, GTK_BUTTONS_OK, "%s", message);
	// make sure dialog box is closed when OK is clicked.
	g_signal_connect_swapped(box, "response", G_CALLBACK(gtk_widget_destroy), box);
	gtk_widget_show_all(box);	
}

// this is a macro so we get -Wformat warnings
#define display_dialog_box(state, type, fmt, ...) do { \
	char _buf[1024]; \
	snprintf(_buf, sizeof _buf, fmt, __VA_ARGS__); \
	display_dialog_box_nofmt(state, type, _buf); \
} while (0)
#define display_error(state, fmt, ...) display_dialog_box(state, GTK_MESSAGE_ERROR, fmt, __VA_ARGS__)
#define display_info(state, fmt, ...) display_dialog_box(state, GTK_MESSAGE_INFO, fmt, __VA_ARGS__)
#define display_info_nofmt(state, message) display_dialog_box_nofmt(state, GTK_MESSAGE_INFO, message)



static void close_process(State *state, char const *reason) {
	GtkBuilder *builder = state->builder;
	free(state->maps); state->maps = NULL;
	state->nmaps = 0;
	state->pid = 0;
	{
		GtkListStore *store = GTK_LIST_STORE(gtk_builder_get_object(builder, "memory"));
		gtk_list_store_clear(store);
	}
	if (reason)
		display_info(state, "Can't access process anymore: %s", reason);
	else
		display_info_nofmt(state, "Can't access process anymore.");
}

// don't use this function; use one of the ones below
static int memory_open(State *state, int flags) {
	if (state->pid) {
		if (state->stop_while_accessing_memory) {
			if (kill(state->pid, SIGSTOP) == -1) {
				close_process(state, strerror(errno));
				return 0;
			}
		}
		char name[64];
		sprintf(name, "/proc/%lld/mem", (long long)state->pid);
		int fd = open(name, flags);
		if (fd == -1) {
			close_process(state, strerror(errno));
			return 0;
		}
		return fd;
	}
	return 0;
}

static void memory_close(State *state, int fd) {
	if (state->stop_while_accessing_memory) {
		kill(state->pid, SIGCONT);
	}
	if (fd) close(fd);
}

	
// get a file descriptor for reading memory from the process
// returns 0 on failure
static int memory_reader_open(State *state) {
	return memory_open(state, O_RDONLY);
}

static void memory_reader_close(State *state, int reader) {
	memory_close(state, reader);
}

// like memory_reader_open, but for writing to memory
static int memory_writer_open(State *state) {
	return memory_open(state, O_WRONLY);
}

static void memory_writer_close(State *state, int writer) {
	memory_close(state, writer);
}

static uint8_t memory_read_byte(int reader, Address addr) {
	lseek(reader, (off_t)addr, SEEK_SET);
	uint8_t byte = 0;
	read(reader, &byte, 1);
	return byte;
}

// returns number of bytes successfully read
static Address memory_read_bytes(int reader, Address addr, uint8_t *memory, Address nbytes) {
	lseek(reader, (off_t)addr, SEEK_SET);
	Address idx = 0;
	while (idx < nbytes) {
		ssize_t n = read(reader, &memory[idx], (size_t)(nbytes - idx));
		if (n <= 0) break;
		idx += (Address)n;
	}
	return idx;
}

// returns # of bytes written (so either 0 or 1)
static Address memory_write_byte(int writer, Address addr, uint8_t byte) {
	lseek(writer, (off_t)addr, SEEK_SET);
	if (write(writer, &byte, 1) == 1)
		return 1;
	else
		return 0;
}

// returns # of bytes written
static Address memory_write_bytes(int writer, Address addr, uint8_t const *bytes, Address nbytes) {
	lseek(writer, (off_t)addr, SEEK_SET);
	Address idx = 0;
	while (idx < nbytes) {
		ssize_t n = write(writer, &bytes[idx], (size_t)(nbytes - idx));
		if (n < 0) break;
		idx += (Address)n;
	}
	return idx;
}

// pass config_potentially_changed = false if there definitely hasn't been an update to the target address
// (this is used by auto-refresh so we don't have to clear and re-make the list store each time, which would screw up selection)
static void update_memory_view(State *state, bool config_potentially_changed) {
	if (!state->pid)
		return;
	GtkBuilder *builder = state->builder;
	GtkListStore *store = GTK_LIST_STORE(gtk_builder_get_object(builder, "memory"));
	uint32_t ndisplay = state->memory_view_entries;
	if (config_potentially_changed)
		gtk_list_store_clear(store);
	if (ndisplay == 0)
		return;
	DataType data_type = state->data_type;
	size_t item_size = data_type_size(data_type);
	void *mem = calloc(item_size, ndisplay);
	if (mem) {
		int reader = memory_reader_open(state);
		if (reader) {
			GtkTreeIter iter;
			ndisplay = (uint32_t)memory_read_bytes(reader, state->memory_view_address, mem, ndisplay * item_size);
			gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(store), &iter, "0");
			char *value = mem;
			Address addr = state->memory_view_address;
			for (Address i = 0; i < ndisplay; i += 1, addr += item_size, value += item_size) {
				char index_str[32], addr_str[32], value_str[32];
				sprintf(index_str, "%" PRIdADDR, i);
				sprintf(addr_str, "%" PRIxADDR, addr);
				data_to_str(value, data_type, value_str, sizeof value_str);
				if (config_potentially_changed) {
					gtk_list_store_insert_with_values(store, &iter, -1, 0, index_str, 1, addr_str, 2, value_str, -1);
				} else {
					if (i != (Address)state->editing_memory)
						gtk_list_store_set(store, &iter, 0, index_str, 1, addr_str, 2, value_str, -1);
					if (!gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter))
						config_potentially_changed = true; // could happen if a map grows
				}
			}
			memory_reader_close(state, reader);
		}
		free(mem);
	} else {
		display_error(state, "Out of memory (trying to display %" PRIu32 " bytes of memory).", ndisplay);
	}
}

G_MODULE_EXPORT void update_configuration(GtkWidget *widget, gpointer user_data) {
	State *state = user_data;
	GtkBuilder *builder = state->builder;
	state->stop_while_accessing_memory = gtk_toggle_button_get_active(
		GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, "stop-while-accessing-memory")));
	char const *n_entries_text = gtk_entry_get_text(
		GTK_ENTRY(gtk_builder_get_object(builder, "memory-display-entries")));
	char *endp;
	bool update_memview = false;
	unsigned long n_entries = strtoul(n_entries_text, &endp, 10);
	if (*n_entries_text && !*endp && n_entries != state->memory_view_entries) {
		state->memory_view_entries = (uint32_t)n_entries;
		update_memview = true;
	}
	char const *address_text = gtk_entry_get_text(
		GTK_ENTRY(gtk_builder_get_object(builder, "address")));
	unsigned long address = strtoul(address_text, &endp, 16);
	if (*address_text && !*endp && address != state->memory_view_address) {
		state->memory_view_address = address;
		update_memview = true;
	}
	GtkRadioButton *data_type_u8 = GTK_RADIO_BUTTON(
		gtk_builder_get_object(builder, "type-u8"));
	for (GSList *l = gtk_radio_button_get_group(data_type_u8);
		l; l = l->next) {
		GtkToggleButton *button = l->data;
		if (gtk_toggle_button_get_active(button)) {
			char const *type_name = gtk_widget_get_name(GTK_WIDGET(button));
			DataType type = data_type_from_name(type_name);
			if (state->data_type != type) {
				state->data_type = type;
				update_memview = true;
			}
		}
	}
	
	if (update_memview) {
		update_memory_view(state, true);
	}
}

// update the memory maps for the current process (state->maps)
static void update_maps(State *state) {
	free(state->maps); state->maps = NULL;
	
	char maps_name[64];
	sprintf(maps_name, "/proc/%lld/maps", (long long)state->pid);
	FILE *maps_file = fopen(maps_name, "rb");
	if (maps_file) {
		char line[256];
		size_t capacity = 0;
		while (fgets(line, sizeof line, maps_file))
			++capacity;
		rewind(maps_file);
		Map *maps = state->maps = calloc(capacity, sizeof *maps);
		unsigned nmaps = 0;
		if (maps) {
			while (fgets(line, sizeof line, maps_file)) {
				Address addr_lo, addr_hi;
				char protections[8];
				if (sscanf(line, "%" SCNxADDR "-%" SCNxADDR " %8s", &addr_lo, &addr_hi, protections) == 3 && nmaps < capacity) {
					if (strcmp(protections, "rw-p") == 0) { // @TODO(eventually): make this configurable
						Map *map = &maps[nmaps++];
						map->lo = addr_lo;
						map->size = addr_hi - addr_lo;
					}
				}
			}
			state->nmaps = nmaps;
		} else {
			display_error(state, "Not enough memory to hold map metadata (%zu items)", capacity);
		}
	} else {
		display_error(state, "Couldn't open %s: %s", maps_name, strerror(errno));
	}
}

// the user entered a PID.
G_MODULE_EXPORT void select_pid(GtkButton *button, gpointer user_data) {
	State *state = user_data;
	GtkBuilder *builder = state->builder;
	GtkEntry *pid = GTK_ENTRY(gtk_builder_get_object(builder, "pid"));
	char const *pid_text = gtk_entry_get_text(pid);
	char *end;
	long long pid_number = strtoll(pid_text, &end, 10);
	if (*pid_text != '\0' && *end == '\0') {
		char dirname[64];
		sprintf(dirname, "/proc/%lld", pid_number);
		int dir = open(dirname, O_DIRECTORY|O_RDONLY);
		if (dir == -1) {
			display_error(state, "Error opening %s: %s", dirname, strerror(errno));
		} else {
			int cmdline = openat(dir, "cmdline", O_RDONLY);
			char process_name[64] = {0};
			if (cmdline != -1) {
				// the contents of cmdline, up to the first null byte is argv[0],
				// which should be the name of the process
				read(cmdline, process_name, sizeof process_name - 1);	
				close(cmdline);
			}
			if (*process_name == '\0') { // handles unable to open/unable to read/empty cmdline
				strcpy(process_name, "Unknown process.");
			}
			GtkLabel *process_name_label = GTK_LABEL(gtk_builder_get_object(builder, "process-name"));
			gtk_label_set_text(process_name_label, process_name);
			state->pid = (PID)pid_number;
			close(dir);
			update_maps(state);
			if (state->nmaps) {
				// display whatever's in the first mapping
				Map *first_map = &state->maps[0];
				state->memory_view_address = first_map->lo;
				update_memory_view(state, true);
			}
		}
	}
}

// a value in memory was edited
G_MODULE_EXPORT void memory_edited(GtkCellRendererText *renderer, char *path, char *new_text, gpointer user_data) {
	State *state = user_data;
	GtkBuilder *builder = state->builder;
	DataType data_type = state->data_type;
	size_t item_size = data_type_size(data_type);
	Address idx = (Address)atol(path);
	Address addr = state->memory_view_address + idx * item_size;
	state->editing_memory = -1;
	uint64_t value = 0;
	if (data_from_str(new_text, data_type, &value)) {
		int writer = memory_writer_open(state);
		if (writer) {
			bool success = memory_write_bytes(writer, addr, (uint8_t const *)&value, item_size) == item_size;
			memory_writer_close(state, writer);
			if (success) {
				GtkListStore *store = GTK_LIST_STORE(gtk_builder_get_object(builder, "memory"));
				GtkTreeIter iter;
				char value_str[32];
				data_to_str(&value, data_type, value_str, sizeof value_str);
				gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(store), &iter, path);
				gtk_list_store_set(store, &iter, 2, value_str, -1);
			}
			
		}
	}
}

G_MODULE_EXPORT void refresh_memory(GtkWidget *widget, gpointer user_data) {
	State *state = user_data;
	update_memory_view(state, true);
}

G_MODULE_EXPORT void memory_editing_started(GtkCellRenderer *renderer, GtkCellEditable *editable, char *path, gpointer user_data) {
	State *state = user_data;
	state->editing_memory = atol(path);
}

G_MODULE_EXPORT void memory_editing_canceled(GtkCellRenderer *renderer, gpointer user_data) {
	State *state = user_data;
	state->editing_memory = -1;
}

// this function is run once per frame
static gboolean frame_callback(gpointer user_data) {
	State *state = user_data;
	GtkBuilder *builder = state->builder;
	GtkToggleButton *auto_refresh = GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, "auto-refresh"));
	
	if (gtk_toggle_button_get_active(auto_refresh)) {
		update_memory_view(state, false);
	}
	return 1;
}

static void on_activate(GtkApplication *app, gpointer user_data) {
	State *state = user_data;
	GError *error = NULL;
	GtkBuilder *builder = gtk_builder_new();
	if (!gtk_builder_add_from_file(builder, "ui.glade", &error)) {
		g_printerr("Error loading UI: %s\n", error->message);
		g_clear_error(&error);
		exit(EXIT_FAILURE);
	}
	state->builder = builder;
	gtk_builder_connect_signals(builder, state);
	
	GtkWindow *window = GTK_WINDOW(gtk_builder_get_object(builder, "window"));
	state->window = window;
	gtk_window_set_application(window, app);
	update_configuration(NULL, state);
	
	g_timeout_add(16, frame_callback, state);
	
	gtk_widget_show_all(GTK_WIDGET(window));
}

int main(int argc, char **argv) {
	GtkApplication *app = gtk_application_new("com.pommicket.pokemem", G_APPLICATION_FLAGS_NONE);
	State state = {0};
	state.editing_memory = -1;
	g_signal_connect(app, "activate", G_CALLBACK(on_activate), &state);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
