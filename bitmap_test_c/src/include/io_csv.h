/*
 * CSV writer, port of core/io_csv.py::write_csv.
 *
 * CRLF line endings and minimal quoting like Python's csv module, UTF-8, header row from labels.
 * cells are preformatted strings, NULL means empty. creates parent dirs as needed.
 */
#ifndef IO_CSV_H
#define IO_CSV_H

/* cells is row-major, cell (r,c) = cells[r*ncols + c]. nonzero on I/O error. */
int csv_write(const char *path,
              const char *const *headers, long ncols,
              const char *const *cells, long nrows);

#endif /* IO_CSV_H */
