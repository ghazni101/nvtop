/*
 * Copyright (C) 2018 Maxime Schmitt <maxime.schmitt91@gmail.com>
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

#ifndef __PLOT_H_
#define __PLOT_H_

#include "nvtop/common.h"

#include <ncurses.h>
#include <stdbool.h>
#include <stddef.h>

#define PLOT_MAX_LEGEND_SIZE 35

// Enable/disable the unicode look (rounded frame, dotted grid, legend
// swatches). When disabled the plot falls back to ACS line drawing.
void nvtop_plot_set_unicode(bool use_unicode);

// Enable/disable colored plot chrome (dim frame/grid via color pairs).
void nvtop_plot_set_color(bool use_color);

void nvtop_line_plot(WINDOW *win, size_t num_data, const double *data, unsigned num_plots, bool legend_left,
                     char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE]);

void draw_rectangle(WINDOW *win, unsigned startX, unsigned startY, unsigned sizeX, unsigned sizeY);

// Row (in the outer plot window coordinates) where a percentage level is
// drawn by nvtop_line_plot for an inner plot window of rows_inner rows.
// Axis labels and reference grids MUST use this mapping so they stay glued
// to the trace at every terminal height.
int plot_label_row(int rows_inner, unsigned percent);

#endif // __PLOT_H_
