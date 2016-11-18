# Davrods build system.
# Author: Chris Smeele
# Copyright (c) 2016, Utrecht University

-include user.prefs

MODNAME      ?= davrods
SHARED_FNAME := mod_$(MODNAME).so
SHARED       := ./.libs/$(SHARED_FNAME)
HTTPD_SERVICE ?= "httpd"

ifeq ($(strip $(APXS)),)
APXS	:= $(shell which apxs)
endif


# XXX: These are currently unused as we rely on the apxs utility for module
#      installation.
INSTALL_DIR  ?= /usr/lib64/httpd/modules
INSTALLED    := $(INSTALL_DIR)/mod_$(MODNAME).so

CFILES := mod_davrods.c auth.c common.c config.c prop.c propdb.c repo.c meta.c theme.c rest.c listing.c
HFILES := mod_davrods.h auth.h common.h config.h prop.h propdb.h repo.h meta.h theme.h rest.h listing.h

# The DAV providers supported by default (you can override this in the shell using DAV_PROVIDERS="..." make).
DAV_PROVIDERS ?= LOCALLOCK NOLOCKS

# A string that's prepended to davrods's DAV provider names (<string>-nolocks, <string>-locallock, ...).
DAV_PROVIDER_NAME_PREFIX    ?= $(MODNAME)

# A string that's prepended to davrods's configuration directives (<string>Zone, <string>DefaultResource, ...).
# Note: This is NOT case-sensitive.
DAV_CONFIG_DIRECTIVE_PREFIX ?= $(MODNAME)

ifneq (,$(findstring LOCALLOCK,$(DAV_PROVIDERS)))
# Compile local locking support using DBM if requested.
CFILES += lock_local.c
HFILES += lock_local.h
endif

SRCFILES := $(CFILES) $(HFILES)
OUTFILES := $(CFILES:%.c=%.o) $(CFILES:%.c=%.lo) $(CFILES:%.c=%.slo) $(CFILES:%.c=%.la)

INCLUDE_PATHS := /usr/include/irods

# Most of these are iRODS client lib dependencies.
LIBS :=                  \
	dl               \
	m                \
	pthread          \
	crypto           \
	irods_client    \
        irods_common     \
        irods_plugin_dependencies \
        RodsAPIs \
	stdc++           \
	boost_system     \
	boost_filesystem \
	boost_regex      \
	boost_thread     \
	boost_chrono     \
	boost_program_options \
	ssl              \
	jansson

#LIBPATHS := /usr/lib/irods/externals
LIBPATHS := /opt/irods-externals/
	
WARNINGS :=                           \
	all                           \
	extra                         \
	no-unused-parameter           \
	no-missing-field-initializers \
	no-format                     \
	fatal-errors \
	shadow

MACROS := \
	$(addprefix DAVRODS_ENABLE_PROVIDER_, $(DAV_PROVIDERS))      \
	DAVRODS_PROVIDER_NAME=\\\"$(DAV_PROVIDER_NAME_PREFIX)\\\"    \
	DAVRODS_CONFIG_PREFIX=\\\"$(DAV_CONFIG_DIRECTIVE_PREFIX)\\\" \
	DAVRODS_DEBUG_VERY_DESPERATE=1

ifdef DEBUG
MACROS += DAVRODS_DEBUG_VERY_DESPERATE
endif

CFLAGS +=                              \
	-g3                            \
	-ggdb                          \
	-O0							\
	-std=c99                       \
	-pedantic                      \
	$(addprefix -W, $(WARNINGS))   \
	$(addprefix -D, $(MACROS)) \
	$(addprefix -I, $(INCLUDE_PATHS))

LDFLAGS +=                           \
	$(addprefix -l, $(LIBS))     \
	$(addprefix -L, $(LIBPATHS))

comma := ,

.PHONY: all shared install test clean apxs

all: shared

install: $(SHARED)
	sudo $(APXS) -i -n $(MODNAME)_module $(SHARED)
	sudo service $(HTTPD_SERVICE) restart

shared: $(SHARED)

$(SHARED): apxs $(SRCFILES)
	$(APXS) -c                                  \
		$(addprefix -Wc$(comma), $(CFLAGS))  \
		$(addprefix -Wl$(comma), $(LDFLAGS)) \
		-o $(SHARED_FNAME) $(SRCFILES) -lrt

clean:
	rm -vf $(OUTFILES)
	rm -rvf .libs
	
apxs:
ifeq ($(strip $(APXS)),)
	echo "No APXS variable has been set, cannot compile"
	exit 1
endif

