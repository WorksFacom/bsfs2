/*
 * CSV writer. Port of core/io_csv.py::write_csv.
 *
 * Emits CRLF line endings and minimal quoting (matching Python's csv module),
 * UTF-8, header row from labels. Cells are pre-formatted strings; a NULL cell
 * is written as empty. Creates parent directories as needed.
 */
#ifndef IO_CSV_H
#define IO_CSV_H

/* cells is row-major with `ncols` columns: cell (r,c) = cells[r*ncols + c].
 * Returns 0 on success, non-zero on I/O error. */
int csv_write(const char *path,
              const char *const *headers, long ncols,
              const char *const *cells, long nrows);

#endif /* IO_CSV_H */
