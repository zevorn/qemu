Prebuilt binary packages
========================

Process Mission publishes complete QEMU binary packages for Linux, Windows,
and macOS in the project's GitHub Releases.  Each package is built from the
default QEMU target selection for one host operating system and host CPU
architecture.  It contains all installed ``qemu-system-*`` binaries and QEMU
tools; Linux packages also contain the Linux user-mode emulators selected by
the default configuration.  The release builds disable only the SDL display
backend, so all guest targets remain available while the packages avoid host
SDL runtime compatibility problems.

The packages include the QEMU firmware and data files, required non-system
runtime libraries, the ``COPYING``, ``COPYING.LIB``, and ``LICENSE`` files, and
the corresponding third-party notices in the ``licenses`` directory.

Release tags
------------

Downstream releases use the following tag convention::

  vX.Y.Z-process-mission-YYYYMMDD

``vX.Y.Z`` identifies the upstream QEMU release on which the downstream tree
is based.  ``YYYYMMDD`` records the downstream release date.  Pushing such a
tag builds and publishes the following host packages:

* Linux x86_64 and aarch64
* Windows x86_64, built with MinGW
* macOS x86_64 and aarch64

The corresponding Actions run retains the same archives as workflow artifacts
for fourteen days.  Release archives remain downloadable from the GitHub
Release and are accompanied by a ``SHA256SUMS`` file.

Compatibility
-------------

Linux packages are built on CentOS Stream 9 and require glibc 2.34 or newer.
The macOS Intel package is built and checked for macOS 15.0, while the Apple
Silicon package is built and checked for macOS 14.0.  The deployment check
applies to QEMU and every bundled dynamic library.

Using a package
---------------

Download the archive matching the host operating system and host CPU
architecture, verify it against ``SHA256SUMS``, and extract it.  For example,
on Linux::

  sha256sum -c SHA256SUMS --ignore-missing
  tar xf qemu-vX.Y.Z-process-mission-YYYYMMDD-linux-x86_64.tar.gz
  ./qemu-vX.Y.Z-process-mission-YYYYMMDD-linux-x86_64/bin/qemu-system-mcs51 \
      -M stc8g1k08a -bios firmware.hex -nographic

On Windows, extract the ``.zip`` archive and run the executables in its
``bin`` directory.  On macOS, extract the matching ``.tar.gz`` archive and
run the executables in ``bin``.  The macOS archives are ad-hoc signed but not
notarized; macOS may require removing the download quarantine attribute before
the first run.
