/*** CREW TEXTEDITOR ***/
/*** VT100 ref - https://www.csie.ntu.edu.tw/~r92094/c++/VT100.html ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <fcntl.h>

#define CTRL_KEY(k) ((k) & 0x1f)
#define ABUF_INIT {NULL,0}
#define CREW_VERSION "0.0.1"
#define TAB_STOP 8
#define QUIT_TIMES 2

/*** structs ***/

typedef struct erow {
	int size;
	int rsize;
	char *chars;
	char *render;
}erow;

typedef struct editor_config {
	int cx, cy;
	int ry;
	int row_off;
	int col_off;
	int scr_rows;
	int scr_cols;
	int num_rows;
	int dirty;
	erow *row;
	char *file_name;
	char status_msg[80];
	time_t status_msg_time;
	struct termios orginal;
}editor_config;

editor_config E;

typedef struct abuf {
	char *b;
	int len;
}abuf;

/*** utils ***/
void clr_scr() {
	// VT100 escape sequences
	write(STDOUT_FILENO, "\x1b[2J", 4);
	// reposition the cursor
	write(STDOUT_FILENO, "\x1b[H", 3);
}

void die(char* s) {
	clr_scr();
	perror(s);
	exit(1);
}

enum EDITOR_KEY {
	BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	PAGE_UP,
	PAGE_DOWN,
	HOME_KEY,
	END_KEY,
	DEL_KEY
};

int editor_read_key() {
	int nread = 0;
	char c;
	while ((nread = read(STDIN_FILENO,&c,1)) != 1) {
		if (nread == -1 && errno == EAGAIN) die("read");
	}
	if (c == '\x1b') {
		char seq[3];
		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    	if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
    	if (seq[0] == '[') {
    		if (seq[1] >= '0' && seq[1] <= '9') {
    			if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
    			if (seq[2] == '~') {
    				switch (seq[1]) {
	    				case '1': return HOME_KEY;
			            case '3': return DEL_KEY;
			            case '4': return END_KEY;
			            case '5': return PAGE_UP;
			            case '6': return PAGE_DOWN;
			            case '7': return HOME_KEY;
			            case '8': return END_KEY;
	    			}
    			}
    		} else {
    			switch (seq[1]) {
	    			case 'A' : return ARROW_UP;
	    			case 'B' : return ARROW_DOWN;
	    			case 'C' : return ARROW_RIGHT;
	    			case 'D' : return ARROW_LEFT;          
	    			case 'H': return HOME_KEY;
          			case 'F': return END_KEY;
	    		}
    		}
    	} else if (seq[0] == 'O') {
		    switch (seq[1]) {
			    case 'H': return HOME_KEY;
		    	case 'F': return END_KEY;
		    }
		}
    	return '\x1b';
	}
	return c;
}

int get_cur_pos(int* rows, int* cols) {
	if (write(STDOUT_FILENO, "\x1b[6n",4) != 4) return -1;
	char buf[32];
	unsigned int i = 0;
	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO,&buf[i],1) != 1) return -1;
		if (buf[i] == 'R') break;
		i++;
	}
	buf[i] = '\0';
	if (buf[0] != '\x1b' || buf[1] != '[') return -1;
	if (sscanf(&buf[2],"%d;%d",rows,cols) == -1) return -1;
	return 0;
}

int get_win_size(int* rows, int* cols) {
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return get_cur_pos(rows, cols);
  	} else {
    	*cols = ws.ws_col;
    	*rows = ws.ws_row;
    	return 0;
  	}
}

void ab_append(abuf* ab, const char* s, int len) {
	char *new = realloc(ab->b, ab->len + len);
	if (new == NULL) return;
	memcpy(&new[ab->len],s,len);
	ab->b = new;
	ab->len += len;
}

void ab_free(abuf* ab) {
	free(ab->b);
}

void editor_set_status(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.status_msg, sizeof(E.status_msg), fmt, ap);
	va_end(ap);
	E.status_msg_time = time(NULL);
}

void editor_update_row(erow* row) {
	int i = 0, idx = 0, tabs = 0;
	for (;i<row->size;i++) if (row->chars[i] == '\t') tabs++;
	row->render = (char*) malloc(row->size + ((TAB_STOP - 1)*tabs) + 1);
	for (i=0;i<row->size;i++) {
		if (row->chars[i] == '\t') {
			int k = 0;
			for (;k<TAB_STOP;k++) row->render[idx++] = ' ';
		} else row->render[idx++] = row->chars[i];
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void editor_insert_row(int at, char* s, size_t len) {
	if (at < 0 || at > E.num_rows) return;	
	E.row = realloc(E.row, sizeof(erow) * (E.num_rows + 1));
	memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.num_rows - at));
	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';
	E.row[at].rsize = 0;
	E.row[at].render = "";
	editor_update_row(&E.row[at]);
	E.num_rows++;
}

void editor_row_del_char(erow* row, int at) {
	if (at < 0 || at >= row->size) return;
	memmove(&row->chars[at], &row->chars[at+1], row->size - at);
	row->size--;
	editor_update_row(row);
	E.dirty++;
}

void editor_free_row(erow* row) {
	free(row->render);
	free(row->chars);
}

void editor_del_row(int at) {
	if (at < 0 || at >= E.num_rows) return;
	editor_free_row(&E.row[at]);
	memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.num_rows - at - 1));
	E.num_rows--;
	E.dirty++;
}

void editor_row_append_string(erow* row, char* s, size_t len) {
	row->chars = realloc(row->chars, row->size + len + 1);
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	editor_update_row(row);
	E.dirty++;
}

void editor_del_char() {
	if (E.cx == E.num_rows) return;
	if (E.cx == 0 && E.cy == 0) return;
	erow *row = &E.row[E.cx];
	if (E.cy > 0) {
		editor_row_del_char(row,E.cy - 1);
		E.cy--;
	} else {
		E.cy = E.row[E.cx - 1].size;
		editor_row_append_string(&E.row[E.cx - 1], row->chars, row->size);
		editor_del_row(E.cx);
		E.cx--;
	}
}

char* editor_rows_to_string(int* buf_len) {
	int tot_len = 0, i = 0;
	for (;i<E.num_rows;i++) tot_len += E.row[i].size + 1;
	*buf_len = tot_len;
	char *buf = (char*) malloc(sizeof(char) * *buf_len), *p = buf;
	for (i = 0; i < E.num_rows; i++) {
		memcpy(p, E.row[i].chars, E.row[i].size);
		p += E.row[i].size;
		*p = '\n';
		p++;
	}
	return buf;
}

/*** raw_mode ***/
void disable_raw_mode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orginal) == -1) die("tcsetattr");
}

// IXON -> turn off ctrl+s,ctrl+q
// ICRNL -> stop turning carriage return to newline
// ECHO -> keypress invisible
// ICANON -> turn off canonical mode => one byte at a time is read
// ISIG -> to turn off ctrl+c,ctrl+d
// IEXTEN -> ctrl+v
// OPOST -> stop turning '\n' to '\r\n' by terminal
void enable_raw_mode() {
	if (tcgetattr(STDIN_FILENO, &E.orginal) == -1) die("tcgetattr");
	atexit(disable_raw_mode);
	struct termios raw = E.orginal;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
	raw.c_cflag |= (CS8);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

void init_editor() {
	E.cx = 0;
	E.cy = 0;
	E.ry = 0;
	E.num_rows = 0;
	E.row = NULL;
	E.row_off = 0;
	E.col_off = 0;
	E.dirty = 0;
	E.file_name = NULL;
	E.status_msg[0] = '\0';
	E.status_msg_time = 0;

	if (get_win_size(&E.scr_rows,&E.scr_cols) == -1) die("get_win_size");
	E.scr_rows-=2;
}

void editor_row_insert_char(erow* row, int at, int c) {
	if (at < 0 || at > row->size) at = row->size;
	row->chars = realloc(row->chars, row->size + 2);
	memmove(&row->chars[at+1],&row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = c;
	editor_update_row(row);
}

void editor_insert_char(int c) {
	if (E.cx == E.num_rows) editor_insert_row(E.num_rows, "", 0);
	editor_row_insert_char(&E.row[E.cx], E.cy, c);
	E.cy++;
	E.dirty++;
}

void editor_insert_new_line() {
	if (E.cy == 0) editor_insert_row(E.cx, "" , 0);
	else {
		erow *row = &E.row[E.cx];
		editor_insert_row(E.cx+1, &row->chars[E.cy], row->size - E.cy);
		row = &E.row[E.cx];
		row->size = E.cy;
		row->chars[row->size] = '\0';
		editor_update_row(row);
	}
	E.cx++;
	E.cy = 0;
}

void editor_move_cursor(int c) {
	erow *row = (E.cx >= E.num_rows) ? NULL : &E.row[E.cx];
	switch(c) {
		case ARROW_LEFT:
			if (E.cy != 0) E.cy--;
			else if (E.cx > 0) {
				E.cx--;
				E.cy = E.row[E.cx].size;
			}
			break;
		case ARROW_RIGHT:
			if (row && E.cy < row->size) E.cy++;
			else if (row && E.cy == row->size) {
				E.cx++;
				E.cy = 0;
			}
			break;
		case ARROW_UP:
			if (E.cx != 0) E.cx--;
			break;
		case ARROW_DOWN:
			if (E.cx != E.num_rows) E.cx++;
			break;
	}
	row = (E.cx >= E.num_rows) ? NULL : &E.row[E.cx];
	int row_len = row ? row->size : 0;
	if (E.cy > row_len) E.cy = row_len;
}

int editor_row_cytory(erow* row, int cy) {
	int ry = 0, j = 0;
	for (;j<cy;j++) {
		if (row->chars[j] == '\t') ry += ((TAB_STOP - 1) - (ry%TAB_STOP));
		ry++;
	}
	return ry;
}

void editor_draw_rows(abuf* ab) {
	int y;
  	for (y = 0; y < E.scr_rows; y++) {
  		int curr_row = y + E.row_off;
  		if (curr_row >= E.num_rows) {
  			if (E.num_rows == 0 && y == E.scr_rows/3) {
	  			char welcome[30];
	  			int welcome_len = snprintf(welcome,sizeof(welcome),"CREW editor -- version %s", CREW_VERSION);
	  			welcome_len = (welcome_len > E.scr_cols) ? E.scr_cols : welcome_len;
	  			int padding = (E.scr_cols - welcome_len) / 2;
			    if (padding) {
			    	ab_append(ab, "~", 1);
			        padding--;
			    }
			    while (padding--) ab_append(ab, " ", 1);
	  			ab_append(ab, welcome, welcome_len);
	  		} else ab_append(ab, "~", 1);	
  		} else {
  			int len = E.row[curr_row].rsize - E.col_off;
  			if (len < 0) len = 0;
  			if (len > E.scr_cols) len = E.scr_cols;
  			ab_append(ab, &E.row[curr_row].render[E.col_off], len);
  		}
  		
    	ab_append(ab, "\x1b[K", 3);
    	ab_append(ab, "\r\n", 2);
  	}
}

void editor_scroll() {
	E.ry = 0;
	if (E.cx < E.num_rows) E.ry = editor_row_cytory(&E.row[E.cx], E.cy);
	if (E.cx < E.row_off) E.row_off = E.cx;
	if (E.cx >= E.row_off + E.scr_rows) E.row_off = E.cx - E.scr_rows + 1;
	if (E.ry < E.col_off) E.col_off = E.ry;
	if (E.ry >= E.col_off + E.scr_cols) E.col_off = E.ry - E.scr_cols + 1;
}

void editor_draw_msg_bar(abuf* ab) {
	ab_append(ab, "\x1b[K", 3);
	int msg_len = strlen(E.status_msg);
	if (msg_len > E.scr_cols) msg_len = E.scr_cols;
	if (msg_len && time(NULL) - E.status_msg_time < 5) ab_append(ab, E.status_msg, msg_len);
}

void editor_draw_status_bar(abuf* ab) {
	ab_append(ab, "\x1b[7m", 4);
	char status[80], rstatus[80];
  	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", 
  		E.file_name ? E.file_name : "[No Name]", E.num_rows, E.dirty ? "(modified)" : "");
  	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cx+1, E.num_rows);
  	if (len > E.scr_cols) len = E.scr_cols;
  	ab_append(ab, status, len);
	while (len < E.scr_cols) {
		if (E.scr_cols - len == rlen) {
			ab_append(ab, rstatus, rlen);
			break;
		} else {
			ab_append(ab, " ", 1);
			len++;
		}
	}
	ab_append(ab, "\x1b[m", 3);
	ab_append(ab, "\r\n", 2);
}

void editor_refresh_screen() {
	editor_scroll();

	abuf ab = ABUF_INIT;
	// hide the cursor
	ab_append(&ab, "\x1b[?25l", 6);
	
	ab_append(&ab,"\x1b[H", 3);
	
	editor_draw_rows(&ab);
	editor_draw_status_bar(&ab);
	editor_draw_msg_bar(&ab);
	
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cx - E.row_off) + 1, (E.ry - E.col_off) + 1);
	ab_append(&ab, buf, strlen(buf));

	// show the cursor
	ab_append(&ab, "\x1b[?25h", 6);
  	
  	write(STDOUT_FILENO, ab.b, ab.len);
  	ab_free(&ab);
}

void editor_open(char* file_name) {
	E.file_name = strdup(file_name);
	FILE *fp = fopen(file_name,"r");
	if (!fp) die("fopen");

	char *line = NULL;
	size_t line_cap = 0;
	ssize_t line_len;
	while ((line_len = getline(&line,&line_cap,fp)) != -1) {
		while (line_len > 0 && (line[line_len-1] == '\n' || line[line_len-1] == '\r' || line[line_len-1] == EOF)) {
			line_len--;
			editor_insert_row(E.num_rows, line, line_len);
		}
	}
	free(line);
	fclose(fp);
	E.dirty = 0;
}

char* editor_prompt(char* prompt) {
	size_t buf_size = 128;
	char *buf = malloc(buf_size);
	size_t buf_len = 0;
	buf[0] = '\0';
	while (1) {
		editor_set_status(prompt, buf);
		editor_refresh_screen();
		int c = editor_read_key();
		if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
			if (buf_len != 0) buf[--buf_len] = '\0';
		} else if (c == '\x1b') {
			editor_set_status("");
			free(buf);
			return NULL;
		} else if (c == '\r') {
			if (buf_len != 0) {
				editor_set_status("");
				return buf;
			}
		} else if (!iscntrl(c) && c < 128) {
			if (buf_len == buf_size - 1) {
				buf_size *= 2;
				buf = realloc(buf, buf_size);
			}
			buf[buf_len++] = c;
			buf[buf_len] = '\0'; 
		}
	}
}

void editor_save() {
	if (E.file_name == NULL) {
		E.file_name = editor_prompt("Save as : %s");
		if (E.file_name == NULL) {
			editor_prompt("Save aborted!");
			return;
		}
	}
	int len;
	char *buf = editor_rows_to_string(&len);
	int fd = open(E.file_name, O_RDWR | O_CREAT, 0644);
	if (fd != -1 && ftruncate(fd,len) != -1 && write(fd, buf, len) == len)
		editor_set_status("%d bytes written to disk", len);
	else
		editor_set_status("Can't save! I/O error: %s", strerror(errno));
	if (fd != -1) close(fd);
	free(buf);
	E.dirty = 0;
}

void editor_key_press() {
	static int quit_times = QUIT_TIMES;
	int c = editor_read_key();
	switch (c) {
		case '\r':
			editor_insert_new_line();
			break;

		case CTRL_KEY('q'): {
			if (E.dirty && quit_times > 0) {
				quit_times--;
				editor_set_status("WARNING!!! File has unsaved changes. "
          			"Press Ctrl-Q %d more times to quit.", quit_times+1);
				return;
			}
			clr_scr();
			exit(0);
			break;
		}

		case CTRL_KEY('s'):
			editor_save();
			break;

		case HOME_KEY:
      		E.cy = 0;
      		break;
    	case END_KEY:
    		if (E.cx < E.num_rows) E.cy = E.row[E.cx].size;
      		break;

		case PAGE_DOWN:
		case PAGE_UP: {
			if (c == PAGE_UP) E.cx = E.row_off;
			else if (c == PAGE_DOWN) E.cx = E.row_off + E.scr_rows - 1;
			if (E.cx > E.num_rows) E.cx = E.num_rows;
			int times = E.scr_rows;
			while (times--) editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
		}

		case BACKSPACE:
		case CTRL_KEY('h'):
		case DEL_KEY:
			if (c == DEL_KEY) editor_move_cursor(ARROW_RIGHT);
			editor_del_char();
			break;

		case ARROW_DOWN:
		case ARROW_UP:
		case ARROW_RIGHT:
		case ARROW_LEFT:
			editor_move_cursor(c);
			break;

		case CTRL_KEY('l'):
		case '\x1b':
			break;

		default:
			editor_insert_char(c);
	}
	quit_times = QUIT_TIMES;
}

/*** init ***/
int main(int argc, char** argv) {
	enable_raw_mode();
	init_editor();
	if (argc == 2) editor_open(argv[1]);
	editor_set_status("HELP: Ctrl-S = save | Ctrl-Q = quit");
	while (1) {
		editor_refresh_screen();
		editor_key_press();
	}
	disable_raw_mode();
	return 0;
}
