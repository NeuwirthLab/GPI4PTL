# GPI4PTL

GPI4PTL is a fork of [GPI-2](https://github.com/cc-hpc-itwm/GPI-2) that
adds a Portals4 transport to the GASPI communication library. It is based on
GPI-2 1.7.0 (upstream commit `3f5dbc68ef8c`), originally developed by
Fraunhofer ITWM.

## Modifications

See [ChangeLog.txt](ChangeLog.txt) for the dated list of changes.

## License

GPI4PTL is released under the GNU General Public License, version 3. 
See [COPYING](COPYING) for the full license text.

GPI-2 copyright © 2013–2026 Fraunhofer ITWM. The source files carry the
applicable copyright, license, and warranty notices.

## Build with Portals4

Install a C compiler, GNU Autotools, and a Portals4 implementation that
provides `portals4.h` and `libportals`. A Fortran compiler is needed for the
Fortran bindings.

We highly suggest to compile GPI4PTL with MPI.

```sh
./autogen.sh
./configure --with-portals4 --prefix=<PATH> --with-mpi
make -j
make install
```

Use `--with-portals4=/path/to/portals4` for an installation outside the
default search paths, or set `PORTALS4_CFLAGS` and `PORTALS4_LIBS` explicitly.

See the [GPI-2 guide](README.GPI-2.md) for other build configurations,
application linking, `gaspi_run`, and tracing. See the [test guide](tests/README)
for running the test suite.
