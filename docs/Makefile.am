if HAVE_DOXYGEN
docs/doxygen: doxyfile-stamp

doxyfile-stamp: Doxyfile $(top_srcdir)/README.GPI-2.md
	cp $(abs_top_srcdir)/README.GPI-2.md $(abs_top_builddir)/docs/README.md
	$(DOXYGEN) Doxyfile
	date > $@

clean:
	-rm -rf doxygen doxyfile-stamp Doxyfile README.md
endif

EXTRA_DIST = Doxyfile.in

CLEANFILES = *~ *.bak doxyfile-stamp Doxyfile
