# SPDX-FileCopyrightText: 2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  ../include
  ../../asset_system
  ../../blenkernel
  ../../blenloader
  ../../blentranslation
  ../../bmesh
  ../../makesrna
  ../../windowmanager
)

set(INC_SYS
)

set(SRC
  ed_undo.cc
  memfile_undo.cc
  undo_system_types.cc

  undo_intern.hh
)

set(LIB
  bf_blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::dna
  PRIVATE bf::intern::clog
  PRIVATE bf::intern::guardedalloc
)

blender_add_lib(bf_editor_undo "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
