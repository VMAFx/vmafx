# Praetor engine resolution; included by Makefiles to align $(PRAETORCTL) with the hook launcher (#906).
ifneq ($(wildcard .config/lefthook/engine.sh),)
PRAETOR_ENGINE = $(eval PRAETOR_ENGINE := $(or $(shell sh .config/lefthook/engine.sh --print-path),$(error praetor hooks: engine launcher failed to resolve praetorctl)))$(PRAETOR_ENGINE)
PRAETORCTL ?= $(PRAETOR_ENGINE)
endif

praetor_engine_goal := $(.DEFAULT_GOAL)
.PHONY: praetor-engine-path
praetor-engine-path:
	@echo $(PRAETORCTL)
.DEFAULT_GOAL := $(praetor_engine_goal)
