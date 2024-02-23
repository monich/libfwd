# -*- Mode: makefile-gmake -*-

all:
%:
	@$(MAKE) -C test_api $*
	@$(MAKE) -C test_io $*
	@$(MAKE) -C test_util $*

clean: unitclean
	rm -f *~
	rm -f coverage/*~
	rm -f coverage/*.gcov
	rm -fr coverage/report
