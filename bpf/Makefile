CLANG ?= clang
BPFTOOL ?= bpftool

ifeq ($(V),1)
  Q =
  msg =
else
  Q = @
  msg = @printf '  %-8s %s%s\n' "$(1)" "$(2)" "$(if $(3), $(3))";
endif

# Architecture detection
ARCH := $(shell uname -m | sed 's/x86_64/x86/' | sed 's/aarch64/arm64/')

# BPF compilation flags
BPF_CFLAGS := -O2 -g -Wall -Werror
BPF_CFLAGS += -target bpf -DBPF=1 -D__TARGET_ARCH_$(ARCH)
ifneq ($(strip $(BPF_CPU_VERSION)),)
BPF_CFLAGS += -mcpu=$(BPF_CPU_VERSION)
endif
BPF_CFLAGS += -I../common -I. -I../libbpf/src/root/usr/include
ifeq ($(ENABLE_XDP_INGRESS),1)
BPF_CFLAGS += -DENABLE_XDP_INGRESS=1
endif

# BTF support
USE_BTF ?= 1
ifeq ($(USE_BTF),1)
BPF_CFLAGS += -D__BPF_USE_BTF__=1
endif

ifeq ($(DISABLE_BPF_TIMER),1)
BPF_CFLAGS += -DDISABLE_BPF_TIMER=1
endif

# BPF programs
PROGRAMS := tutuicmptunnel
BPF_OBJS := $(addsuffix .bpf.o, $(PROGRAMS))
BPF_SKELS := $(addsuffix .skel.h, $(PROGRAMS))
DEBUG_OBJS := $(addsuffix .debug.bpf.o, $(PROGRAMS))
DEBUG_SKELS := $(addsuffix .debug.skel.h, $(PROGRAMS))

# Base dependencies (always needed)
TUTUICMPTUNNEL_BASE_DEPS := ../common/defs.h ../common/try.h

# BTF-specific dependencies
ifeq ($(USE_BTF),1)
TUTUICMPTUNNEL_BTF_DEPS := vmlinux.h
else
TUTUICMPTUNNEL_BTF_DEPS :=
endif

# Complete dependencies
TUTUICMPTUNNEL_DEPS := $(TUTUICMPTUNNEL_BASE_DEPS) $(TUTUICMPTUNNEL_BTF_DEPS)

.PHONY: all clean check help update-deps
.PRECIOUS: %.bpf.o %.debug.bpf.o
.DEFAULT_GOAL := all

all: $(BPF_SKELS) $(DEBUG_SKELS)

%.bpf.o: %.bpf.c $(TUTUICMPTUNNEL_DEPS)
	$(Q)$(CLANG) $(BPF_CFLAGS) -c $< -o $*.tmp.bpf.o
	$(Q)$(BPFTOOL) gen object $@ $*.tmp.bpf.o
	@rm -f $*.tmp.bpf.o

%.debug.bpf.o: %.bpf.c $(TUTUICMPTUNNEL_DEPS)
	$(Q)$(CLANG) -g $(BPF_CFLAGS) -DBPF_DEBUG=1 -DDEBUG -c $< -o $*.debug.tmp.bpf.o
	$(Q)$(BPFTOOL) gen object $@ $*.debug.tmp.bpf.o
	@rm -f $*.debug.tmp.bpf.o

# Generate skeleton headers
tutuicmptunnel.skel.h: tutuicmptunnel.bpf.o
	$(Q)$(BPFTOOL) gen skeleton $< name tutuicmptunnel > $@

tutuicmptunnel.debug.skel.h: tutuicmptunnel.debug.bpf.o
	$(Q)$(BPFTOOL) gen skeleton $< name tutuicmptunnel_debug > $@

# Generate vmlinux.h (only if BTF is enabled)
ifeq ($(USE_BTF),1)
vmlinux.h:
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@ || \
	{ rm -f "$@"; echo "Error: $(BPFTOOL) not found or not executable"; exit 1; }
endif

# Update dependencies
update-deps:
	@echo "Run: ./gen-deps.sh $(USE_BTF) to update dependencies"
	@echo "Then manually copy the output to this Makefile"

# Show current configuration
show-config:
	@echo "Configuration:"
	@echo "  USE_BTF=$(USE_BTF)"
	@echo "  Dependencies: $(TUTUICMPTUNNEL_DEPS)"

clean:
	@echo "Cleaning up..."
	$(Q)rm -f $(BPF_OBJS) $(BPF_SKELS) $(DEBUG_OBJS) $(DEBUG_SKELS)
ifeq ($(USE_BTF),1)
	$(Q)rm -f vmlinux.h
endif

help:
	@echo "Available targets:"
	@echo "  all         - Build all BPF programs and skeletons"
	@echo "  update-deps - Show how to update dependencies"
	@echo "  show-config - Show current configuration"
	@echo "  clean       - Remove built files"
	@echo "  help        - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  USE_BTF=1   - Enable BTF support (default: 1)"
	@echo "  CLANG=path  - Path to clang compiler"
	@echo "  BPFTOOL=path- Path to bpftool"

# vim: set ts=2 sw=2:
