/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm_SharedICHelpers_arm_h
#define jit_arm_SharedICHelpers_arm_h

#include "jit/BaselineFrame.h"
#include "jit/BaselineIC.h"
#include "jit/MacroAssembler.h"
#include "jit/SharedICRegisters.h"

namespace js {
namespace jit {

// Distance from sp to the top Value inside an IC stub (no return address on the
// stack on ARM).
static const size_t ICStackValueOffset = 0;

inline void EmitRestoreTailCallReg(MacroAssembler& masm) {
  // No-op on ARM because link register is always holding the return address.
}

inline void EmitRepushTailCallReg(MacroAssembler& masm) {
  // No-op on ARM because link register is always holding the return address.
}

inline void EmitCallIC(MacroAssembler& masm, CodeOffset* callOffset) {
  // The stub pointer must already be in ICStubReg.
  // Load stubcode pointer from the ICStub.
  // R2 won't be active when we call ICs, so we can use r0.
  static_assert(R2 == ValueOperand(r1, r0));
  masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), r0);

  // Call the stubcode via a direct branch-and-link.
  masm.ma_blx(r0);
  *callOffset = CodeOffset(masm.currentOffset());
}

inline void EmitEnterTypeMonitorIC(
    MacroAssembler& masm,
    size_t monitorStubOffset = ICMonitoredStub::offsetOfFirstMonitorStub()) {
  // This is expected to be called from within an IC, when ICStubReg is
  // properly initialized to point to the stub.
  masm.loadPtr(Address(ICStubReg, (uint32_t)monitorStubOffset), ICStubReg);

  // Load stubcode pointer from BaselineStubEntry.
  // R2 won't be active when we call ICs, so we can use r0.
  static_assert(R2 == ValueOperand(r1, r0));
  masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), r0);

  // Jump to the stubcode.
  masm.branch(r0);
}

inline void EmitReturnFromIC(MacroAssembler& masm) { masm.ma_mov(lr, pc); }

inline void EmitBaselineLeaveStubFrame(MacroAssembler& masm,
                                       bool calledIntoIon = false) {
  ScratchRegisterScope scratch(masm);

  // Ion frames do not save and restore the frame pointer. If we called into
  // Ion, we have to restore the stack pointer from the frame descriptor. If
  // we performed a VM call, the descriptor has been popped already so in that
  // case we use the frame pointer.
  if (calledIntoIon) {
    masm.Pop(scratch);
    masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), scratch);
    masm.add32(scratch, BaselineStackReg);
  } else {
    masm.mov(BaselineFrameReg, BaselineStackReg);
  }

  masm.Pop(BaselineFrameReg);
  masm.Pop(ICStubReg);

  // Load the return address.
  masm.Pop(ICTailCallReg);

  // Discard the frame descriptor.
  masm.Pop(scratch);
}

template <typename AddrType>
inline void EmitPreBarrier(MacroAssembler& masm, const AddrType& addr,
                           MIRType type) {
  // On ARM, lr is clobbered by guardedCallPreBarrier. Save it first.
  masm.push(lr);
  masm.guardedCallPreBarrier(addr, type);
  masm.pop(lr);
}

inline void EmitStubGuardFailure(MacroAssembler& masm) {
  // Load next stub into ICStubReg.
  masm.loadPtr(Address(ICStubReg, ICStub::offsetOfNext()), ICStubReg);

  // Return address is already loaded, just jump to the next stubcode.
  static_assert(ICTailCallReg == lr);
  masm.jump(Address(ICStubReg, ICStub::offsetOfStubCode()));
}

}  // namespace jit
}  // namespace js

#endif /* jit_arm_SharedICHelpers_arm_h */
