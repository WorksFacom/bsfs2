# Documentation

The source documentation is built with Doxygen.

## Generate documentation

With Doxygen and Graphviz (`dot`, for the diagrams) installed, run this from the project root:

```bash
$ make docs
```

It runs `doxygen Doxyfile` from this directory. The paths in `Doxyfile` are relative to `doc/`, so running `doxygen doc/Doxyfile` from anywhere else finds no sources. The main page comes from `mainpage.md`.

## View the documentation

Open this file in a browser:

```
docs/html/index.html
```

The output is static, search included, so any web server can host `docs/html/` as is.
