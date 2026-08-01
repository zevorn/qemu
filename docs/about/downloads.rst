Prebuilt binary packages
========================

Process Mission publishes complete Windows x86_64 MinGW64 QEMU binary packages
in the project's GitHub Releases.  Each package is built from the default QEMU
target selection and contains all installed ``qemu-system-*`` binaries and
QEMU tools.  The release builds disable only the SDL display backend, so all
guest targets remain available while the packages avoid host SDL runtime
compatibility problems.

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

* Windows x86_64, built with MinGW64

The corresponding Actions run retains the same archives as workflow artifacts
for fourteen days.  Release archives remain downloadable from the GitHub
Release and are accompanied by a ``SHA256SUMS`` file.

Using a package
---------------

Download the ``.zip`` archive, verify it against ``SHA256SUMS``, and extract
it on a Windows x86_64 host.  Run the executables in its ``bin`` directory.
