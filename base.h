// types and stuff needed everywhere

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
#include <math.h>

typedef pid_t PID;
typedef uint64_t Address;
#define SCNxADDR SCNx64
#define PRIdADDR PRId64
#define PRIxADDR PRIx64

#define MASK64(i) ((uint64_t)1 << (i))

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

typedef enum {
	SEARCH_ENTER_VALUE,
	SEARCH_SAME_DIFFERENT
} SearchType;

// a memory map
typedef struct {
	Address lo, size;
} Map;

typedef struct {
	GtkWindow *window;
	GtkBuilder *builder;
	bool stop_while_accessing_memory;
	long editing_memory; // index of memory value being edited, or -1 if none is
	PID pid;
	Address total_memory; // total amount of memory used by process, in bytes
	Map *maps;
	Address memory_view_address;
	unsigned memory_view_n_items; // # of entries to show
	unsigned nmaps;
	DataType data_type;
	SearchType search_type;
	uint64_t *search_candidates; // this is a bit array, where the ith bit corresponds to whether byte #i in the processes memory is a search candidate.
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
#define display_error_nofmt(state, message) display_dialog_box_nofmt(state, GTK_MESSAGE_ERROR, message)
#define display_info(state, fmt, ...) display_dialog_box(state, GTK_MESSAGE_INFO, fmt, __VA_ARGS__)
#define display_info_nofmt(state, message) display_dialog_box_nofmt(state, GTK_MESSAGE_INFO, message)


