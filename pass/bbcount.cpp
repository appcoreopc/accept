#include <sstream>
#include <string>
#include <iostream>

#include "llvm/IRBuilder.h"
#include "llvm/Module.h"
#include "llvm/Analysis/Verifier.h"

#include "accept.h"

#define INSTRUMENT_FP false

using namespace llvm;

namespace {
  static unsigned bbIndex;
  static unsigned bbTotal;
  static unsigned fpTotal;
  static unsigned fpIndex;

  struct BBCount : public FunctionPass {
    static char ID;
    ACCEPTPass *transformPass;
    Module *module;

    BBCount();
    virtual const char *getPassName() const;
    virtual bool doInitialization(llvm::Module &M);
    virtual bool doFinalization(llvm::Module &M);
    virtual bool runOnFunction(llvm::Function &F);
    bool instrumentBasicBlocks(llvm::Function &F);
  };
}

BBCount::BBCount() : FunctionPass(ID) {
  initializeBBCountPass(*PassRegistry::getPassRegistry());
}

const char *BBCount::getPassName() const {
  return "Basic Block instrumentation";
}
bool BBCount::doInitialization(Module &M) {
  module = &M;

  // ACCEPT shared transform pass
  transformPass = (ACCEPTPass*)sharedAcceptTransformPass;

  // We'll insert the initialization call in main
  Function *Main = M.getFunction("main");
  assert(Main && "Error: count-bb requires a main function");

  // Initialization call, logbb_init() defined in run time
  LLVMContext &Ctx = Main->getContext();
  Constant *initFunc = Main->getParent()->getOrInsertFunction(
    "logbb_init", Type::getVoidTy(Ctx), Type::getInt32Ty(Ctx), Type::getInt32Ty(Ctx), NULL
  );
  BasicBlock *bb = &Main->front();
  Instruction *op = &bb->front();
  IRBuilder<> builder(op);
  builder.SetInsertPoint(bb, builder.GetInsertPoint());

  // Initialize statics
  bbIndex = 0;
  bbTotal = 0;
  fpIndex = 0;
  fpTotal = 0;
  // Determine the number of basic blocks in the module
  for (Module::iterator mi = M.begin(); mi != M.end(); ++mi) {
    Function *F = mi;
    // if (!transformPass->shouldSkipFunc(*F))
    bbTotal += F->size();
    for (Function::iterator fi = F->begin(); fi != F->end(); ++fi) {
      BasicBlock *bb = fi;
      // Count the number fp
      for (BasicBlock::iterator bi = bb->begin(); bi != bb->end(); ++bi) {
        Instruction *inst = bi;
        if (isa<BinaryOperator>(inst) ||
            isa<StoreInst>(inst) ||
            isa<LoadInst>(inst) ||
            isa<CallInst>(inst)) {
          Type * opType = inst->getType();
          if (opType == Type::getHalfTy(Ctx) ||
              opType == Type::getFloatTy(Ctx) ||
              opType == Type::getDoubleTy(Ctx)) {
            fpTotal++;
          }
        }
      }
    }
  }

  Value* bbTotalVal = builder.getInt32(bbTotal);;
  Value* fpTotalVal = builder.getInt32(fpTotal);;
  Value* args[] = {bbTotalVal, fpTotalVal};
  builder.CreateCall(initFunc, args);

  return true; // modified IR
}

bool BBCount::doFinalization(Module &M) {
  return false;
}

bool BBCount::runOnFunction(Function &F) {
  bool modified = false;

  // Skip optimizing functions that seem to be in standard libraries.
  if (!transformPass->shouldSkipFunc(F)) {
    assert (!llvm::verifyFunction(F) && "Verification failed before code alteration");
    modified = instrumentBasicBlocks(F);
    assert (!llvm::verifyFunction(F) && "Verification failed after code alteration");
  }

  return modified;
}

bool BBCount::instrumentBasicBlocks(Function & F){

  LLVMContext &Ctx = F.getContext();
  Type* voidty = Type::getVoidTy(Ctx);
  Type* int16ty = Type::getInt16Ty(Ctx);
  Type* int32ty = Type::getInt32Ty(Ctx);
  Type* int64ty = Type::getInt64Ty(Ctx);
  Type* stringty = Type::getInt8PtrTy(Ctx);
  Type* halfty = Type::getHalfTy(Ctx);
  Type* floatty = Type::getFloatTy(Ctx);
  Type* doublety = Type::getDoubleTy(Ctx);

  const std::string bb_injectFn_name = "logbb";
  Constant *bbLogFunc = module->getOrInsertFunction(
    bb_injectFn_name, voidty, int32ty, NULL
  );
  const std::string fp_injectFn_name = "logfp";
  Constant *fpLogFunc = module->getOrInsertFunction(
    fp_injectFn_name, voidty, int32ty, stringty,
    int32ty, int64ty, NULL
  );

  bool modified = false;


  for (Function::iterator fi = F.begin(); fi != F.end(); ++fi) {

    // Only instrument if the function is white-listed
    if (transformPass->shouldInjectError(F)) {

      BasicBlock *bb = fi;

#if INSTRUMENT_FP==true
      for (BasicBlock::iterator bi = bb->begin(); bi != bb->end(); ++bi) {
        Instruction *inst = bi;
        Instruction *nextInst = next(bi, 1);

        // Check that the instruction is of interest to us
        if ( (isa<BinaryOperator>(inst) ||
              isa<StoreInst>(inst) ||
              isa<LoadInst>(inst) ||
              isa<CallInst>(inst)) && inst->getMetadata("iid") )
        {
          assert(nextInst && "next inst is NULL");

          // Check if the float is double or half precision op
          Type * opType = inst->getType();
          if (opType == halfty ||
              opType == floatty ||
              opType == doublety) {

            // Builder
            IRBuilder<> builder(module->getContext());
            builder.SetInsertPoint(nextInst);

            // Identify the type
            int opTypeEnum;
            Type* dst_type;
            if (opType == halfty) {
              opTypeEnum = 1;
              dst_type = int16ty;
            }
            else if (opType == floatty) {
              opTypeEnum = 2;
              dst_type = int32ty;
            }
            else if (opType == doublety) {
              opTypeEnum = 3;
              dst_type = int64ty;
            } else {
              assert(NULL && "Type unknown!");
            }

            // Arg1: Type enum
            Value* param_opType = builder.getInt32(opTypeEnum);
            // Arg2: Instruction ID string
            StringRef iid = cast<MDString>(inst->getMetadata("iid")->getOperand(0))->getString();
            Value* instIdx_global_str = builder.CreateGlobalString(iid.str().c_str());
            Value* param_instIdx = builder.CreateBitCast(instIdx_global_str, stringty);
            // Arg3: FP instruction index
            Value* param_fpIdx = builder.getInt32(fpIndex);
            // Arg4: Destination value and type
            Value* dst_to_be_casted = builder.CreateBitCast(inst, dst_type);
            Value* param_val = builder.CreateZExtOrBitCast(dst_to_be_casted, int64ty);

            // Create vector
            Value* args[] = {
              param_opType,
              param_instIdx,
              param_fpIdx,
              param_val
            };

            // Inject function
            builder.CreateCall(fpLogFunc, args);

            ++fpIndex;
          }
        }
      }
#endif //INSTRUMENT_FP

      // Insert logbb call before the BB terminator
      Instruction *op = bb->getTerminator();
      IRBuilder<> builder(op);
      builder.SetInsertPoint(bb, builder.GetInsertPoint());

      // Pass the BB id (global)
      assert(bbIndex<bbTotal && "bbIndex exceeds bbTotal");
      Value* bbIdxValue = builder.getInt32(bbIndex);
      Value* args[] = {bbIdxValue};
      builder.CreateCall(bbLogFunc, args);

      modified = true;
    }

    ++bbIndex;
  }

  return modified;
}

char BBCount::ID = 0;
INITIALIZE_PASS_BEGIN(BBCount, "bb count", "ACCEPT basic block instrumentation pass", false, false)
INITIALIZE_PASS_DEPENDENCY(ApproxInfo)
INITIALIZE_PASS_END(BBCount, "bb count", "ACCEPT basic block instrumentation pass", false, false)
FunctionPass *llvm::createBBCountPass() { return new BBCount(); }
