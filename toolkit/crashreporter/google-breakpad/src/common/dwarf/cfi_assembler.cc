// Copyright (c) 2010, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Original author: Jim Blandy <jimb@mozilla.com> <jimb@red-bean.com>

// cfi_assembler.cc: Implementation of google_breakpad::CFISection class.
// See cfi_assembler.h for details.

#include <cassert>
#include <stdlib.h>

#include "common/dwarf/cfi_assembler.h"

namespace google_breakpad {

using dwarf2reader::DwarfPointerEncoding;
  
CFISection &CFISection::CIEHeader(u_int64_t code_alignment_factor,
                                  int data_alignment_factor,
                                  unsigned return_address_register,
                                  u_int8_t version,
                                  const string &augmentation,
                                  bool dwarf64) {
  assert(!entry_length_);
  entry_length_ = new PendingLength();
  in_fde_ = false;

  if (dwarf64) {
    D32(0xffffffff);
    D64(entry_length_->length);
    entry_length_->start = Here();
    // Write the CIE distinguished value. In .debug_frame sections, it's
    // ~0; in .eh_frame sections, it's zero.
    D64(eh_frame_ ? 0 : ~(u_int64_t)0);
  } else {
    D32(entry_length_->length);
    entry_length_->start = Here();
    // Write the CIE distinguished value. In .debug_frame sections, it's
    // ~0; in .eh_frame sections, it's zero.
    D32(eh_frame_ ? 0 : ~(u_int32_t)0);
  }
  D8(version);
  AppendCString(augmentation);
  ULEB128(code_alignment_factor);
  LEB128(data_alignment_factor);
  if (version == 1)
    D8(return_address_register);
  else
    ULEB128(return_address_register);
  return *this;
}

CFISection &CFISection::FDEHeader(Label cie_pointer,
                                  u_int64_t initial_location,
                                  u_int64_t address_range,
                                  bool dwarf64) {
  assert(!entry_length_);
  entry_length_ = new PendingLength();
  in_fde_ = true;
  fde_start_address_ = initial_location;

  if (dwarf64) {
    D32(0xffffffff);
    D64(entry_length_->length);
    entry_length_->start = Here();
    if (eh_frame_)
      D64(Here() - cie_pointer);
    else
      D64(cie_pointer);
  } else {
    D32(entry_length_->length);
    entry_length_->start = Here();
    if (eh_frame_)
      D32(Here() - cie_pointer);
    else
      D32(cie_pointer);
  }
  EncodedPointer(initial_location);
  // The FDE length in an .eh_frame section uses the same encoding as the
  // initial location, but ignores the base address (selected by the upper
  // nybble of the encoding), as it's a length, not an address that can be
  // made relative.
  EncodedPointer(address_range,
                 DwarfPointerEncoding(pointer_encoding_ & 0x0f));
  return *this;
}

CFISection &CFISection::FinishEntry() {
  assert(entry_length_);
  Align(address_size_, dwarf2reader::DW_CFA_nop);
  entry_length_->length = Here() - entry_length_->start;
  delete entry_length_;
  entry_length_ = NULL;
  in_fde_ = false;
  return *this;
}

CFISection &CFISection::EncodedPointer(u_int64_t address,
                                       DwarfPointerEncoding encoding,
                                       const EncodedPointerBases &bases) {
  // Omitted data is extremely easy to emit.
  if (encoding == dwarf2reader::DW_EH_PE_omit)
    return *this;

  // If (encoding & dwarf2reader::DW_EH_PE_indirect) != 0, then we assume
  // that ADDRESS is the address at which the pointer is stored --- in
  // other words, that bit has no effect on how we write the pointer.
  encoding = DwarfPointerEncoding(encoding & ~dwarf2reader::DW_EH_PE_indirect);

  // Find the base address to which this pointer is relative. The upper
  // nybble of the encoding specifies this.
  u_int64_t base;
  switch (encoding & 0xf0) {
    case dwarf2reader::DW_EH_PE_absptr:  base = 0;                  break;
    case dwarf2reader::DW_EH_PE_pcrel:   base = bases.cfi + Size(); break;
    case dwarf2reader::DW_EH_PE_textrel: base = bases.text;         break;
    case dwarf2reader::DW_EH_PE_datarel: base = bases.data;         break;
    case dwarf2reader::DW_EH_PE_funcrel: base = fde_start_address_; break;
    case dwarf2reader::DW_EH_PE_aligned: base = 0;                  break;
    default: abort();
  };

  // Make ADDRESS relative. Yes, this is appropriate even for "absptr"
  // values; see gcc/unwind-pe.h.
  address -= base;

  // Align the pointer, if required.
  if ((encoding & 0xf0) == dwarf2reader::DW_EH_PE_aligned)
    Align(AddressSize());

  // Append ADDRESS to this section in the appropriate form. For the
  // fixed-width forms, we don't need to differentiate between signed and
  // unsigned encodings, because ADDRESS has already been extended to 64
  // bits before it was passed to us.
  switch (encoding & 0x0f) {
    case dwarf2reader::DW_EH_PE_absptr:
      Address(address);
      break;

    case dwarf2reader::DW_EH_PE_uleb128:
      ULEB128(address);
      break;

    case dwarf2reader::DW_EH_PE_sleb128:
      LEB128(address);
      break;

    case dwarf2reader::DW_EH_PE_udata2:
    case dwarf2reader::DW_EH_PE_sdata2:
      D16(address);
      break;

    case dwarf2reader::DW_EH_PE_udata4:
    case dwarf2reader::DW_EH_PE_sdata4:
      D32(address);
      break;

    case dwarf2reader::DW_EH_PE_udata8:
    case dwarf2reader::DW_EH_PE_sdata8:
      D64(address);
      break;

    default:
      abort();
  }

  return *this;
};

} // namespace google_breakpad
