/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LoweringPasses.h"
#include "hermes/BCGen/CommonPeepholeLowering.h"
#include "hermes/IR/IRBuilder.h"

namespace hermes::sh {

namespace {

class PeepholeLowering {
  Function *const F_;
  IRBuilder builder_{F_};
  IRBuilder::InstructionDestroyer destroyer_{};

 public:
  explicit PeepholeLowering(Function *F) : F_(F) {}

  bool run() {
    bool changed = false;
    for (auto &BB : *F_) {
      for (auto &I : BB) {
        if (Value *replaceVal = peep(&I)) {
          if (replaceVal != &I)
            I.replaceAllUsesWith(replaceVal);
          changed = true;
        }
      }
    }
    return changed;
  }

 private:
  /// Perform peephole optimization on the specified instruction. Replacement
  /// instructions need to be created and inserted in the correct position by
  /// using the builder. Instructions for deletion should be inserted in the
  /// destroyer in the correct order (users first). If a change is made, a
  /// non-null value must be returned; if it is different from \p I, it will be
  /// used to replace all uses of \p I.
  Value *peep(Instruction *I) {
    switch (I->getKind()) {
      case ValueKind::CoerceThisNSInstKind: {
        return lowerCoerceThisNSInst(
            llvh::cast<CoerceThisNSInst>(I), builder_, destroyer_);
      }
      case ValueKind::BinaryExponentiationInstKind: {
        return lowerBinaryExponentiationInst(
            llvh::cast<BinaryOperatorInst>(I), builder_, destroyer_);
      }
      case ValueKind::CallInstKind: {
        return stripEnvFromCall(llvh::cast<CallInst>(I), builder_);
      }
      case ValueKind::DebuggerInstKind:
      case ValueKind::EvalCompilationDataInstKind:
        // Instructions aren't supported when compiling to the native
        // backend, so delete it if it was generated so that the lowering
        // doesn't have to deal with it.
        destroyer_.add(I);
        return nullptr;
      default:
        return nullptr;
    }
  }
};

} // namespace

Pass *createPeepholeLowering() {
  class ThisPass : public FunctionPass {
   public:
    explicit ThisPass() : FunctionPass("PeepholeLowering") {}
    bool runOnFunction(Function *F) override {
      return PeepholeLowering(F).run();
    }
  };
  return new ThisPass();
}

} // namespace hermes::sh
