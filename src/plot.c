/*
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

static bool plot_unicode = false;
static bool plot_color = false;

void nvtop_plot_set_unicode(bool use_unicode) { plot_unicode = use_unicode; }

void nvtop_plot_set_color(bool use_color) { plot_color = use_color; }

static inline int data_level(double rows, double data, double increment) {
  return (int)(rows - round(data / increment));
}

int plot_label_row(int rows_inner, unsigned percent) {
  double increment = 100. / (double)rows_inner;
  return data_level((double)rows_inner, (double)percent, increment);
}

// Faint dotted reference lines at 25/50/75%, drawn BEFORE the trace so the
// graph always paints over them. Uses the exact same data_level mapping as
// the trace and the axis labels, so it never drifts at any terminal height.
static void draw_plot_grid(WINDOW *win, int rows, int cols, double increment) {
  if (rows < 6 || cols < 4)
    return;
  if (plot_color)
    wcolor_set(win, grid_color, NULL);
  static const unsigned grid_levels[3] = {25, 50, 75};
  for (unsigned g = 0; g < ARRAY_SIZE(grid_levels); ++g) {
    int r = data_level((double)rows, (double)grid_levels[g], increment);
    if (r < 1 || r > rows - 1)
      continue;
    for (int x = 1; x < cols; x += 3)
      mvwaddch(win, r, x, ACS_BULLET | A_DIM);
  }
  if (plot_color)
    wcolor_set(win, 0, NULL);
}

void nvtop_line_plot(WINDOW *win, size_t num_data, const double *data, unsigned num_lines, bool legend_left,
                     char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE]) {
  if (num_data == 0)
    return;
  int rows, cols;
  getmaxyx(win, rows, cols);
  rows -= 1;
  double increment = 100. / (double)(rows);

  draw_plot_grid(win, rows, cols, increment);

  assert(num_lines <= MAX_LINES_PER_PLOT && "Cannot plot more than " EXPAND_AND_QUOTE(MAX_LINES_PER_PLOT) " lines");
  static const short plot_line_colors[MAX_LINES_PER_PLOT] = {7, 8, 9, 10};
  unsigned lvl_before[MAX_LINES_PER_PLOT];
  for (size_t k = 0; k < num_lines && k < num_data; ++k)
    lvl_before[k] = data_level(rows, data[k], increment);
  // Lines beyond the data are never drawn (guarded below), so leave their
  // lvl_before slot untouched.

  for (size_t i = 0; i < num_data; i += num_lines) {
    for (unsigned k = 0; k < num_lines; ++k) {
      // The last group of columns may be partial: never read (or draw) past
      // the end of the data array. num_data is the plot width, so this also
      // keeps the x coordinate inside the window.
      if (i + k >= num_data)
        continue;
      unsigned lvl_now_k = data_level(rows, data[i + k], increment);
      wcolor_set(win, plot_line_colors[k], NULL);
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
              wcolor_set(win, plot_line_colors[j], NULL);
              mvwaddch(win, lvl_before[j], i + k, ACS_HLINE);
              wcolor_set(win, plot_line_colors[k], NULL);
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
              wcolor_set(win, plot_line_colors[j], NULL);
              mvwaddch(win, lvl_before[j], i + k, ACS_HLINE);
              wcolor_set(win, plot_line_colors[k], NULL);
            }
          }
        }
      }
      lvl_before[k] = lvl_now_k;
    }
  }
  // The legend shares this window with the trace: row 0 coincides with the
  // outer window's top border (the inner window starts at outer row 1), so
  // start the legend at row 1 to avoid erasing the border.
  int plot_y_position = 1;
  for (unsigned i = 0; i < num_lines && plot_y_position < rows; ++i) {
    wcolor_set(win, plot_line_colors[i], NULL);
    if (plot_unicode) {
      // Colored swatch before the legend text
      char legend_with_swatch[PLOT_MAX_LEGEND_SIZE + 8];
      snprintf(legend_with_swatch, sizeof(legend_with_swatch), "\xe2\x96\x89 %s", legend[i]); // ▉
      if (legend_left) {
        mvwprintw(win, plot_y_position, 0, "%.*s", cols, legend_with_swatch);
      } else {
        size_t length = strlen(legend_with_swatch);
        if (length <= (size_t)cols) {
          mvwprintw(win, plot_y_position, cols - length, "%s", legend_with_swatch);
        } else {
          mvwprintw(win, plot_y_position, 0, "%.*s", cols, legend_with_swatch);
        }
      }
    } else if (legend_left) {
      mvwprintw(win, plot_y_position, 0, "%.*s", cols, legend[i]);
    } else {
      size_t length = strlen(legend[i]);
      if (length <= (size_t)cols) {
        mvwprintw(win, plot_y_position, cols - length, "%s", legend[i]);
      } else {
        // Legend wider than the window: print its first cols characters.
        mvwprintw(win, plot_y_position, 0, "%.*s", cols, legend[i]);
      }
    }
    plot_y_position++;
  }
}

void draw_rectangle(WINDOW *win, unsigned startX, unsigned startY, unsigned sizeX, unsigned sizeY) {
  if (plot_color)
    wattr_set(win, A_DIM, dim_color, NULL);
  else
    wattron(win, A_DIM);

  if (plot_unicode && sizeX >= 2 && sizeY >= 2) {
    // Rounded unicode frame
    for (unsigned x = startX + 1; x < startX + sizeX - 1; ++x) {
      mvwaddstr(win, startY, x, "\xe2\x94\x80");           // ─
      mvwaddstr(win, startY + sizeY - 1, x, "\xe2\x94\x80"); // ─
    }
    for (unsigned y = startY + 1; y < startY + sizeY - 1; ++y) {
      mvwaddstr(win, y, startX, "\xe2\x94\x82");               // │
      mvwaddstr(win, y, startX + sizeX - 1, "\xe2\x94\x82");   // │
    }
    mvwaddstr(win, startY, startX, "\xe2\x95\xad");                       // ╭
    mvwaddstr(win, startY, startX + sizeX - 1, "\xe2\x95\xae");           // ╮
    mvwaddstr(win, startY + sizeY - 1, startX, "\xe2\x95\xb0");           // ╰
    mvwaddstr(win, startY + sizeY - 1, startX + sizeX - 1, "\xe2\x95\xaf"); // ╯
  } else {
    mvwhline(win, startY, startX + 1, 0, sizeX - 2);
    mvwhline(win, startY + sizeY - 1, startX + 1, 0, sizeX - 2);
    mvwvline(win, startY + 1, startX, 0, sizeY - 2);
    mvwvline(win, startY + 1, startX + sizeX - 1, 0, sizeY - 2);
    mvwaddch(win, startY, startX, ACS_ULCORNER);
    mvwaddch(win, startY, startX + sizeX - 1, ACS_URCORNER);
    mvwaddch(win, startY + sizeY - 1, startX, ACS_LLCORNER);
    mvwaddch(win, startY + sizeY - 1, startX + sizeX - 1, ACS_LRCORNER);
  }
  wattroff(win, A_DIM);
  if (plot_color)
    wattr_set(win, A_NORMAL, 0, NULL);
}
