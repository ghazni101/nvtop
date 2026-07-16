/*
 *
 * Copyright (C) 2019-2021 Maxime Schmitt <maxime.schmitt91@gmail.com>
 *
 * This file is part of Nvtop.
 *
 * Nvtop is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nvtop is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nvtop.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "nvtop/plot.h"
#include "nvtop/common.h"
#include "nvtop/interface_internal_common.h"

#include <assert.h>
#include <ncurses.h>
#include <stdbool.h>
#include <string.h>
#include <tgmath.h>

// Map a plot metric index to its color pair. GPU rate (0) -> maroon,
// GPU mem rate (1) -> purple; the rest keep the default 1-based pairs.
static inline short plot_color_for(unsigned k) {
  if (k == 0) return gpu_color;
  if (k == 1) return mem_color;
  return (short)(k + 1);
}

// Linear 0-100% vertical scale: norm(p) = p/100, so 0%->bottom, 100%->top with
// even spacing (each 25% band occupies an equal quarter of the height).
static inline double plot_norm(double data) {
  return data / 100.0;
}

static inline int data_level(double rows, double data) {
  double norm = plot_norm(data);
  return (int)round(rows - norm * rows);
}

void nvtop_line_plot(WINDOW *win, size_t num_data, const double *data, unsigned num_lines, bool legend_left,
                     char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE]) {
  if (num_data == 0)
    return;
  int rows, cols;
  getmaxyx(win, rows, cols);
  rows -= 1;

  assert(num_lines <= MAX_LINES_PER_PLOT && "Cannot plot more than " EXPAND_AND_QUOTE(MAX_LINES_PER_PLOT) " lines");
  unsigned lvl_before[MAX_LINES_PER_PLOT];
  for (size_t k = 0; k < num_lines; ++k)
    lvl_before[k] = data_level(rows, data[k]);

  // Faint dotted horizontal grid lines at 25/50/75%, drawn BEFORE the trace so
  // the graph lines paint over them. They use the same data_level(rows, p)
  // mapping as the trace itself, so each grid line sits exactly on the trace's
  // 25/50/75% level at every plot height (no drift). With the linear scale this
  // puts 25% a quarter up, 50% at the vertical middle, 75% a quarter down.
  for (unsigned p = 25; p <= 75; p += 25) {
    int gy = data_level(rows, p);
    if (gy > 0 && gy < rows) {
      wcolor_set(win, grid_color, NULL);
      for (int c = 2; c < cols; c += 2)
        mvwaddch(win, gy, c, ACS_BULLET | A_DIM);
    }
  }

  for (size_t i = 0; i < num_data || i < (size_t)cols; i += num_lines) {
    for (unsigned k = 0; k < num_lines; ++k) {
      unsigned lvl_now_k = data_level(rows, data[i + k]);
      wcolor_set(win, plot_color_for(k), NULL);
      // Three cases: has increased, has decreased and remained level
      if (lvl_before[k] < lvl_now_k || lvl_before[k] > lvl_now_k) {
        // Case 1 and 2: has increased/decreased

        // An increase goes down on the plot because (0,0) is top left
        bool drawing_down = lvl_before[k] < lvl_now_k;
        unsigned bottom = drawing_down ? lvl_before[k] : lvl_now_k;
        unsigned top = drawing_down ? lvl_now_k : lvl_before[k];

        // Draw the vertical line corners
        mvwaddch(win, bottom, i + k, drawing_down ? ACS_URCORNER : ACS_ULCORNER);
        mvwaddch(win, top, i + k, drawing_down ? ACS_LLCORNER : ACS_LRCORNER);
        // Draw the vertical line between the corners
        if (top - bottom > 1) {
          mvwvline(win, bottom + 1, i + k, 0, top - bottom - 1);
        }

        // Draw the continuation of the other metrics
        for (unsigned j = 0; j < num_lines; ++j) {
          if (j != k) {
            if (lvl_before[j] == top)
              // The continuation is at the same level as the bottom corner
              mvwaddch(win, top, i + k, ACS_BTEE);
            else if (lvl_before[j] == bottom)
              // The continuation is at the same level as the top corner
              mvwaddch(win, bottom, i + k, ACS_TTEE);
            else if (lvl_before[j] > bottom && lvl_before[j] < top)
              // The continuation lies on the vertical line
              mvwaddch(win, lvl_before[j], i + k, ACS_PLUS);
            else {
              // The continuation lies outside the update interval so keep the
              // color
              wcolor_set(win, plot_color_for(j), NULL);
              mvwaddch(win, lvl_before[j], i + k, ACS_HLINE);
              wcolor_set(win, plot_color_for(k), NULL);
            }
          }
        }
      } else {
        // Case 3: stayed level
        mvwhline(win, lvl_now_k, i + k, 0, 1);
        for (unsigned j = 0; j < num_lines; ++j) {
          if (j != k) {
            if (lvl_before[j] != lvl_now_k) {
              // Add the continuation of other metric lines
              wcolor_set(win, plot_color_for(j), NULL);
              mvwaddch(win, lvl_before[j], i + k, ACS_HLINE);
              wcolor_set(win, plot_color_for(k), NULL);
            }
          }
        }
      }
      lvl_before[k] = lvl_now_k;
    }
  }
  int plot_y_position = 0;
  for (unsigned i = 0; i < num_lines && plot_y_position < rows; ++i) {
    wcolor_set(win, i + 1, NULL);
    if (legend_left) {
      mvwprintw(win, plot_y_position, 0, "%.*s", cols, legend[i]);
    } else {
      size_t length = strlen(legend[i]);
      if (length <= (size_t)cols) {
        mvwprintw(win, plot_y_position, cols - length, "%s", legend[i]);
      } else {
        mvwprintw(win, plot_y_position, 0, "%.*s", (int)(length - cols), legend[i]);
      }
    }
    plot_y_position++;
  }
}

void draw_rectangle(WINDOW *win, unsigned startX, unsigned startY, unsigned sizeX, unsigned sizeY) {
  mvwhline(win, startY, startX + 1, 0, sizeX - 2);
  mvwhline(win, startY + sizeY - 1, startX + 1, 0, sizeX - 2);

  mvwvline(win, startY + 1, startX, 0, sizeY - 2);
  mvwvline(win, startY + 1, startX + sizeX - 1, 0, sizeY - 2);

  mvwaddch(win, startY, startX, ACS_ULCORNER);
  mvwaddch(win, startY, startX + sizeX - 1, ACS_URCORNER);
  mvwaddch(win, startY + sizeY - 1, startX, ACS_LLCORNER);
  mvwaddch(win, startY + sizeY - 1, startX + sizeX - 1, ACS_LRCORNER);
}
