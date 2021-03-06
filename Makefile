## Compiler
# On local machine
MCC := mpic++
SCC := c++

## Local directory tree ##
INCDIR := src/

SRCDIR := src/
BUILDDIR := build/
BINDIR := bin/
VPATH := $(SRCDIR) $(BUILDDIR) $(BINDIR)

## Flag settings ##
DEBUG    := #-g
OPENMP   := -fopenmp
OPTIM    := -O3 -fomit-frame-pointer
MATHFLAG := -lm
WARNFLAG := -Wall -Wextra -Wshadow -Wno-format-zero-length -Wno-write-strings
FULLFLAG := $(DEBUG) $(OPENMP) $(OPTIM) $(MATHFLAG) $(WARNFLAG) -I$(INCDIR) -std=c++11

## Target specific variables
MSUFFIX := -mpi
mpi: CC := $(MCC)
mpi: FULLFLAG += -D MPI_BUILD

SSUFFIX := -omp
omp: CC := $(SCC)

sources := mechafm messages simulation parse system utility interactions minimiser integrators force_grid data_grid cube_io fft kiss_fft kiss_fftnd
s_objects := $(addsuffix $(SSUFFIX).o, $(addprefix $(BUILDDIR), $(sources)))
m_objects := $(addsuffix $(MSUFFIX).o, $(addprefix $(BUILDDIR), $(sources)))

############################################
## Actual make code below (do not change) ##
############################################

.PHONY: clean all

## Make all ##
all: mpi omp

## Make the executable (MPI) ##
mpi: $(BUILDDIR) $(BINDIR) $(m_objects)
	$(CC) $(FULLFLAG) $(m_objects) -o $(BINDIR)mechafm-$@

## Make the executable (OpenMP) ##
omp: $(BUILDDIR) $(BINDIR) $(s_objects)
	$(CC) $(FULLFLAG) $(s_objects) -o $(BINDIR)mechafm-$@

## Create the directory for the build files
$(BUILDDIR):
	mkdir -p $(BUILDDIR)

## Create the directory for the binaries
$(BINDIR):
	mkdir -p $(BINDIR)

## Create all the mpi object files and process dependencies
$(BUILDDIR)%$(MSUFFIX).o: $(SRCDIR)%.c* ##pp
	$(CC) $(FULLFLAG) -MD -c $< -o $@
	@cp $(BUILDDIR)$*$(MSUFFIX).d $(BUILDDIR)$*$(MSUFFIX).P; \
	sed -e 's/#.*//' -e 's/^[^:]*: *//' -e 's/ *\\$$//' \
	-e '/^$$/ d' -e 's/$$/ :/' < $(BUILDDIR)$*$(MSUFFIX).d >> $(BUILDDIR)$*$(MSUFFIX).P; \
	rm -f $(BUILDDIR)$*$(MSUFFIX).d

## Create all the omp object files and process dependencies
$(BUILDDIR)%$(SSUFFIX).o: $(SRCDIR)%.c* ##pp
	$(CC) $(FULLFLAG) -MD -c $< -o $@
	@cp $(BUILDDIR)$*$(SSUFFIX).d $(BUILDDIR)$*$(SSUFFIX).P; \
	sed -e 's/#.*//' -e 's/^[^:]*: *//' -e 's/ *\\$$//' \
	-e '/^$$/ d' -e 's/$$/ :/' < $(BUILDDIR)$*$(SSUFFIX).d >> $(BUILDDIR)$*$(SSUFFIX).P; \
	rm -f $(BUILDDIR)$*$(SSUFFIX).d

## Make clean ##
clean:
	rm -f $(BINDIR)*
	rm -f $(BUILDDIR)*

## Include dependencies
-include $(sources:%=$(BUILDDIR)%$(MSUFFIX).P)
-include $(sources:%=$(BUILDDIR)%$(SSUFFIX).P)
