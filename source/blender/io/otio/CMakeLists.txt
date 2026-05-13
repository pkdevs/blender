# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  .
  exporter
  ../../editors/include
  ../../makesrna
  $ENV{HOME}/opentimelineio/include
)

set(INC_SYS
)

set(SRC
  IO_otio.cc
  exporter/otio_strip.cc
  exporter/otio_export.cc

  IO_otio.hh
  exporter/otio_strip.hh
  exporter/otio_export.hh
)

set(LIB
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::dna
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::sequencer
  PRIVATE bf::windowmanager
  PRIVATE "$ENV{HOME}/opentimelineio/lib/libopentimelineio.a"
)


blender_add_lib(bf_io_otio "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

