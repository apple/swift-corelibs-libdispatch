/*
 * Copyright (c) 2008-2009 Apple Inc.  All rights reserved.
 *
 * @APPLE_DTS_LICENSE_HEADER_START@
 * 
 * IMPORTANT:  This Apple software is supplied to you by Apple Computer, Inc.
 * ("Apple") in consideration of your agreement to the following terms, and your
 * use, installation, modification or redistribution of this Apple software
 * constitutes acceptance of these terms.  If you do not agree with these terms,
 * please do not use, install, modify or redistribute this Apple software.
 * 
 * In consideration of your agreement to abide by the following terms, and
 * subject to these terms, Apple grants you a personal, non-exclusive license,
 * under Apple's copyrights in this original Apple software (the "Apple Software"),
 * to use, reproduce, modify and redistribute the Apple Software, with or without
 * modifications, in source and/or binary forms; provided that if you redistribute
 * the Apple Software in its entirety and without modifications, you must retain
 * this notice and the following text and disclaimers in all such redistributions
 * of the Apple Software.  Neither the name, trademarks, service marks or logos of
 * Apple Computer, Inc. may be used to endorse or promote products derived from
 * the Apple Software without specific prior written permission from Apple.  Except
 * as expressly stated in this notice, no other rights or licenses, express or
 * implied, are granted by Apple herein, including but not limited to any patent
 * rights that may be infringed by your derivative works or by other works in
 * which the Apple Software may be incorporated.
 * 
 * The Apple Software is provided by Apple on an "AS IS" basis.  APPLE MAKES NO
 * WARRANTIES, EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION THE IMPLIED
 * WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE, REGARDING THE APPLE SOFTWARE OR ITS USE AND OPERATION ALONE OR IN
 * COMBINATION WITH YOUR PRODUCTS. 
 * 
 * IN NO EVENT SHALL APPLE BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * ARISING IN ANY WAY OUT OF THE USE, REPRODUCTION, MODIFICATION AND/OR
 * DISTRIBUTION OF THE APPLE SOFTWARE, HOWEVER CAUSED AND WHETHER UNDER THEORY OF
 * CONTRACT, TORT (INCLUDING NEGLIGENCE), STRICT LIABILITY OR OTHERWISE, EVEN IF
 * APPLE HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * @APPLE_DTS_LICENSE_HEADER_END@
 */
/*!
	@header Life
	An asynchronous variation of Conway's Game of Life implemented with
	GCD.  Like the classic version, the game board consists of a grid of
	cells that can live, die or multiply by the following rules[1]:

	    1. Survivals.  Every [living cell] with two or three neighboring
	    [living cells] survives for the next generation.
	    2. Deaths. Each [living cell] wiht four or more neighbors dies (is
	    removed) from overpopulation.  Every [living cell] with one
	    neighbor or none dies from isolation.
	    3. Births. Each empty cell adjacent to exactly three neighbors--no
	    more, no fewer--is a birth cell.  A [living cell] is placed on it
	    at the next move.

	However, unlike the classic version, not all deaths and births occur
	simultaneously in a single, synchronous, "move" of the game board.
	Instead the rules are applies to each cell independently based on its
	observations of the cells around it.

	Each cell is backed by a GCD queue which manages the synchronization
	of the cells internal state (living or dead).  When a cell's state
	changes, a notification in the form of a dispatch_call() of the
	cell_needs_update() work function is sent to all adjacent cells so
	that the state of those cells may be re-evaluated.

	To re-evaluate the state of a cell, a request of the current state of
	all adjecent cells is sent in the form of a dispatch_call() of the
	_cell_is_alive() work function.  The state of the adjacent cells is
	returned to the requestor via the _cell_is_alive_callback() completion
	callback.  Once all outstanding completion callbacks have been
	received, the cell updates its state according to the aforementioned
	rules.  If the application of these rules results in another state
	change, the update_cell() notification is once again sent out,
	repeating the process.

	Due to the highly asynchronous nature of this implementation, the
	simulation's results may differ from the classic version for the same
	set of initial conditions.  In particular, due to non-deterministic
	scheduling factors, the same set of initial condiitions is likely to
	produce dramatically different results on subsequent simulations.

	[1] Martin Gardner. "MATHEMATICAL GAMES: The fantastic combinations of
	    John Conway's new solitaire game 'life'" Scientific American 223
	    (October 1970): 120-123.

	@copyright Copyright (c) 2008-2009 Apple Inc.  All rights reserved.
	@updated 2009-03-31
*/
////////////////////////////////////////////////////////////////////////////////

// Adjustable parameters
unsigned long grid_x_size = 40;
unsigned long grid_y_size = 20;

int use_curses = 1;

////////////////////////////////////////////////////////////////////////////////
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <curses.h>
#include <sys/ioctl.h>
#include <dispatch/dispatch.h>

#define CELL_MAX_NEIGHBORS 8

struct cell {
	dispatch_queue_t q;
	int alive;
	char display;

	// tracks whether a update_cell() notification arrived while
	// an update was already in progress
	int needs_update;
	int living_neighbors;
	int queries_outstanding;

	struct cell* neighbors[CELL_MAX_NEIGHBORS];
	char* label;
} __attribute__((aligned(64)));

////////////////////////////////////////////////////////////////////////////////

/*! @function init_grid
	Initializes the grid data structure based on the global variables
	grid_x_size and grid_y_size.  Must be called before any calls to
	cell_set_alive. */
struct cell* init_grid(size_t grid_x_size, size_t grid_y_size);

/*! @function init_display
	Initializes the display subsystem.  Starts a periodic timer to update the
	display based on the current contents of the cell grid.
 */
void init_display(struct cell* grid);

////////////////////////////////////////////////////////////////////////////////

// Macro to test whether x,y coordinates are within bounds of the grid
#define GRID_VALID(u,v) (((u) >= 0) && ((v) >= 0) && \
			((u) < grid_x_size) && ((v) < grid_y_size))
// Macro to translate from 2d grid coordinates to array offest
#define GRID_OFF(u,v) ((v) * grid_x_size + (u))

#if !defined(DISPATCH_LIFE_GL)
int main(int argc, char* argv[]) {

	struct ttysize tsz;
	int res;

	res = ioctl(STDIN_FILENO, TIOCGWINSZ, &tsz);
	if (res == 0) {
		grid_x_size = tsz.ts_cols;
		grid_y_size = tsz.ts_lines;
	}

	int dispflag = 1;
	int ch;
	
	while ((ch = getopt(argc, argv, "x:y:q")) != -1) {
		char* endptr;
		switch (ch) {
			case 'x':
				grid_x_size = strtol(optarg, &endptr, 10);
				if (grid_x_size < 0 || (endptr && *endptr != 0)) {
					fprintf(stderr, "life: invalid x size\n");
					exit(1);
				}
				break;
			case 'y':
				grid_y_size = strtol(optarg, &endptr, 10);
				if (grid_y_size < 0 || (endptr && *endptr != 0)) {
					fprintf(stderr, "life: invalid y size\n");
					exit(1);
				}
				break;
			case 'q':
				dispflag = 0;
				break;
			case '?':
			default:
				fprintf(stderr, "usage: life [-q] [-x size] [-y size]\n");
				fprintf(stderr, "\t-x: grid x size (default is terminal columns)\n");
				fprintf(stderr, "\t-y: grid y size (default is terminal rows)\n");
				fprintf(stderr, "\t-q: suppress display output\n");
				exit(1);
		}
	}

	struct cell* grid = init_grid(grid_x_size, grid_y_size);

	if (dispflag) {
		init_display(grid);
		if (use_curses) {
			initscr(); cbreak(); noecho();
			nonl();
			intrflush(stdscr, FALSE);
			keypad(stdscr, TRUE);
		}
	}
	
	dispatch_main();

	if (dispflag && use_curses) {
		endwin();
	}
	
	return 0;
}
#endif /* defined(DISPATCH_LIFE_GL) */

////////////////////////////////////////////////////////////////////////////////

static void cell_set_alive(struct cell*, int alive);

/*!	@function update_cell
	GCD work function.  Begins the update process for a cell by
	sending cell_is_alive() messages with cell_is_alive_callback()
	completion callbacks to all adjacent cells.  If an update is already
	in progress, simply sets the needs_update flag of the cell. */
static void update_cell(struct cell*);

/*!	@function cell_is_alive_callback
	GCD completion callback.  Receives the result from cell_is_alive.  When
	all _cell_is_alive_callback() completion callbacks have been received
	from an update, recalculates the internal state of the cell.  If the
	state changes, sends update_cell() to all adjacent cells. */
static void update_cell_response(struct cell*, int);

////////////////////////////////////////////////////////////////////////////////

void
foreach_neighbor(struct cell* self, void (^action)(struct cell* other)) {
	int i;
	for (i = 0; i < CELL_MAX_NEIGHBORS; ++i) {
		struct cell* other = self->neighbors[i];
		if (other) {
			action(other);
		}
	}
}


// Change cell state, update the screen, and update neighbors.
void
cell_set_alive(struct cell* self, int alive) {
	if (alive == self->alive) return; // nothing to do
	
	dispatch_async(self->q, ^{
		self->alive = alive;
		self->display = (self->alive) ? '#' : ' ';

		foreach_neighbor(self, ^(struct cell* other) {
			dispatch_async(other->q, ^{ update_cell(other); });
		});
	});
}

void
update_cell(struct cell* self) {
	if (self->queries_outstanding == 0) {
		self->needs_update = 0;
		self->living_neighbors = 0;

		foreach_neighbor(self, ^(struct cell* other) {
			++self->queries_outstanding;
			dispatch_async(other->q, ^{
				dispatch_async(self->q, ^{ update_cell_response(self, other->alive); });
			});
		});

		// '.' indicates the cell is not alive but needs an update
		if (!self->alive) self->display = '.';
	} else {
		self->needs_update = 1;
	}
}

void
update_cell_response(struct cell* self, int response) {
	if (response) ++self->living_neighbors;
	--self->queries_outstanding;

	// when all neighbors have replied with their state,
	// recalculate our internal state
	if (self->queries_outstanding == 0) {
		const int living_neighbors = self->living_neighbors;
		int alive = self->alive;
		
		// Conway's Game of Life
		if (living_neighbors < 2 || living_neighbors > 3) {
			alive = 0;
		} else if (living_neighbors == 3) {
			alive = 1;
		}

		// Notify neighbors of state change
		cell_set_alive(self, alive);

		// if a request for an update came in while we were
		// already processing one, kick off the next update
		if (self->needs_update) {
			dispatch_async(self->q, ^{ update_cell(self); });
		} else {
			// otherwise clear the '.' character that was
			// displayed during the update
			if (!self->alive) {
				self->display = ' ';
			}
		}
	}
}

////////////////////////////////////////////////////////////////////////////////

struct cell*
init_grid(size_t grid_x_size, size_t grid_y_size) {
	struct cell* grid = calloc(sizeof(struct cell),grid_x_size*grid_y_size);

	int i,j;
	for (i = 0; i < grid_x_size; ++i) {
		for (j = 0; j < grid_y_size; ++j) {
			struct cell* ptr = &grid[GRID_OFF(i,j)];

			asprintf(&ptr->label, "x%dy%d", i, j);

			ptr->q = dispatch_queue_create(ptr->label, NULL);
			dispatch_set_target_queue(ptr->q, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_LOW, 0));
			dispatch_queue_set_context(ptr->q, ptr);

			ptr->neighbors[0] = GRID_VALID(i  ,j-1) ?
				&grid[GRID_OFF(i  ,j-1)] : NULL;	// N
			ptr->neighbors[1] = GRID_VALID(i+1,j-1) ?
				&grid[GRID_OFF(i+1,j-1)] : NULL;	// NE
			ptr->neighbors[2] = GRID_VALID(i+1,j  ) ?
				&grid[GRID_OFF(i+1,j  )] : NULL;	// E
			ptr->neighbors[3] = GRID_VALID(i+1,j+1) ?
				&grid[GRID_OFF(i+1,j+1)] : NULL;	// SE
			ptr->neighbors[4] = GRID_VALID(i  ,j+1) ?
				&grid[GRID_OFF(i  ,j+1)] : NULL;	// S
			ptr->neighbors[5] = GRID_VALID(i-1,j+1) ?
				&grid[GRID_OFF(i-1,j+1)] : NULL;	// SW
			ptr->neighbors[6] = GRID_VALID(i-1,j  ) ?
				&grid[GRID_OFF(i-1,j  )] : NULL;	// W
			ptr->neighbors[7] = GRID_VALID(i-1,j-1) ?
				&grid[GRID_OFF(i-1,j-1)] : NULL;	// NW
		}
	}
	
	srandomdev();
	for (i = 0; i < grid_x_size; ++i) {
		for (j = 0; j < grid_y_size; ++j) {
			if (random() & 1) {
				cell_set_alive(&grid[GRID_OFF(i,j)], 1);
			}
		}
	}

	return grid;
}

#if defined(DISPATCH_LIFE_GL)
char
get_grid_display_char(struct cell* grid, size_t x, size_t y) {
	return grid[GRID_OFF(x,y)].display;
}
#endif /* defined(DISPATCH_LIFE_GL) */

#if !defined(DISPATCH_LIFE_GL)
void
init_display(struct cell* grid)
{
	dispatch_source_t timer;

	timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	dispatch_source_set_timer(dispatch_time(DISPATCH_TIME_NOW, 0). 10000000, 1000);
	dispatch_source_set_event_handler(^{
		int x,y;
		x = 0;
		for (x = 0; x < grid_x_size; ++x) {
			for (y = 0; y < grid_y_size; ++y) {
				mvaddnstr(y, x, &grid[GRID_OFF(x,y)].display, 1);
			}
		}
		refresh();
	});
	dispatch_resume(timer);
}
#endif /* defined(DISPATCH_LIFE_GL) */
