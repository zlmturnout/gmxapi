#!/bin/bash
#
# Build, install, and test the `gmx` Python package against a gmxapi-compatible
# GROMACS installation.
#
# Requirements:
#
# The script assumes the following environment has been set.
# - PYTHON holds the full path to the Python interpreter for a (virtual) environment
#   in which pip, pytest, and the various dependency modules are installed.
# - CC holds the compiler that is also associated with the `mpicc` used to build
#   the mpi4py Python package.
# - CXX holds the C++ compiler used to build GROMACS.
# - GMXRC for a thread-MPI GROMACS installation has been sourced.
#
set -ev

$PYTHON -m pytest python_packaging/src/test

mpiexec -n 2 $PYTHON -m mpi4py -m pytest python_packaging/src/test

# Check how well our compiler cache is working.
ccache -s

# Generate the list of Python packages present in the current environment.
# Helpful, but not necessarily sufficient, for reproducibility.
$PYTHON -m pip freeze
