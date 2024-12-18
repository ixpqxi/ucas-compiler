//===- Hello.cpp - Example code from "Writing an LLVM Pass" ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements two versions of the LLVM "Hello World" pass described
// in docs/WritingAnLLVMPass.html
//
//===----------------------------------------------------------------------===//

#include <llvm/Support/CommandLine.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/ToolOutputFile.h>

#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils.h>

#include <llvm/IR/Function.h>
#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>

#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/DebugInfo.h>

#include <vector>
#include <set>
// #define DEBUG
// #define SHOW

using namespace llvm;
static ManagedStatic<LLVMContext> GlobalContext;
static LLVMContext &getGlobalContext() { return *GlobalContext; }
/* In LLVM 5.0, when  -O0 passed to clang , the functions generated with clang will
 * have optnone attribute which would lead to some transform passes disabled, like mem2reg.
 */
struct EnableFunctionOptPass: public FunctionPass {
    static char ID;
    EnableFunctionOptPass():FunctionPass(ID){}
    bool runOnFunction(Function & F) override{
        if(F.hasFnAttribute(Attribute::OptimizeNone))
        {
            F.removeFnAttr(Attribute::OptimizeNone);
        }
        return true;
    }
};

char EnableFunctionOptPass::ID=0;

	
///!TODO TO BE COMPLETED BY YOU FOR ASSIGNMENT 2
///Updated 11/10/2017 by fargo: make all functions
///processed by mem2reg before this pass.
struct FuncPtrPass : public ModulePass {
  static char ID; // Pass identification, replacement for typeid
  FuncPtrPass() : ModulePass(ID) {}
  std::map<ValueName*, std::set<ValueName*>> PVVals;
  std::map<ValueName*, Function*> funcs;
  std::vector<CallInst *> callInsts;
  std::map<CallInst *, ValueName *> works;

  bool runOnModule(Module &M) override {
    for(Module::iterator fi = M.begin(), fe = M.end(); fi != fe; ++fi) {
        Function &f = *fi;
        funcs[f.getValueName()] = &f;
        for(BasicBlock &bb : f.getBasicBlockList()) {
            for(Instruction &inst : bb.getInstList()) {
                if(PHINode *node = dyn_cast<PHINode>(&inst)){
                    ValueName *left = node->getValueName();
                    int opNum = node->getNumOperands();
                    std::set<ValueName*> valus;
                    for(int i=0; i<opNum ; i++) {
                        ValueName *vn = node->getOperand(i)->getValueName();
                        if(!vn) continue;
                        if(PVVals.find(left) != PVVals.end())
                            PVVals[left].insert(vn);
                        else
                            PVVals[left] = std::set<ValueName*>{vn};
                    }
                }else if(CallInst *callInst = dyn_cast<CallInst>(&inst)){
                    if(callInst->getCalledValue()->getName() == "llvm.dbg.value")
                        continue;
                    callInsts.push_back(callInst);
                    ValueName *callV = callInst->getCalledValue()->getValueName();
                    if(PVVals.find(callV) != PVVals.end()){
                        for(ValueName *calledFuncName : PVVals[callV]){
                            Function *calledFunc = funcs[calledFuncName];
                            if(calledFunc)
                                resolveFunc(callInst, calledFunc);
                        }
                    }else{
                        Function *calledFunc = callInst->getCalledFunction();
                        if(calledFunc){
                            resolveFunc(callInst, calledFunc);
                        }else{
                            works[callInst] = callV;
                        }
                    }
                }
                bool flag = true;
                while(flag){
                    flag = false;
                    for(auto e: PVVals) {
                        for(auto value : e.second){
                            if(PVVals.find(value) != PVVals.end()){
                                flag = true;
                                PVVals[e.first].insert(PVVals[value].begin(), PVVals[value].end());
                                PVVals[e.first].erase(PVVals[e.first].find(value));
                            }
                        }
                    }
                }
                for(auto v : works){
                    if(PVVals.find(v.second) != PVVals.end()){
                        for(ValueName *calledFuncName : PVVals[v.second]){
                            Function *calledFunc = funcs[calledFuncName];
                            if(calledFunc)
                                resolveFunc(v.first, calledFunc);
                        }
                        // works.erase(works.find(v.first));
                    }
                }
            }
        }
    }
    bool flag = true;
    while(flag){
        flag = false;
        for(auto e: PVVals) {
            for(auto value : e.second){
                if(PVVals.find(value) != PVVals.end()){
                    flag = true;
                    PVVals[e.first].insert(PVVals[value].begin(), PVVals[value].end());
                    PVVals[e.first].erase(PVVals[e.first].find(value));
                }
            }
        }
    }

    for(CallInst *callInst : callInsts){
        errs() << callInst->getDebugLoc()->getLine() << " : ";
        if(callInst->getCalledFunction())
            errs() << callInst->getCalledFunction()->getName() << "\n";
        else{
            ValueName *callValue = callInst->getCalledOperand()->getValueName();
            if(PVVals.find(callValue) != PVVals.end()){
                int i = 0, end = PVVals[callValue].size()-1;
                for(ValueName *v : PVVals[callValue])
                    if(i++ < end)
                        errs() << v->getKey() << ", ";
                    else
                        errs() << v->getKey() << "\n";
            }
        }
    }

#ifdef DEBUG
    for(auto v: PVVals){
        errs() << v.first->getKey() << " -> ";
        for(auto v2 : v.second){
            errs() << v2->getKey() << " ";
        }
        errs() << "\n";
    }
#endif
#ifdef SHOW
     M.dump();
#endif
    return false;
  }

  void resolveFunc(CallInst *callInst, Function *func){
    for(int i=0; i<callInst->getNumArgOperands(); i++){
        if(callInst->getArgOperand(i)->hasName()){
            ValueName *right = callInst->getArgOperand(i)->getValueName();
            ValueName *left = dyn_cast<Value>(func->getArg(i))->getValueName();

            if(PVVals.find(left) != PVVals.end())
                PVVals[left].insert(right);
            else
                PVVals[left] = std::set<ValueName*>{right};
        }
    }

    for(BasicBlock &bb : func->getBasicBlockList()){
        for(Instruction &funcInst : bb.getInstList()){
            if(ReturnInst *retInst = dyn_cast<ReturnInst>(&funcInst)){
                ValueName *value =  callInst->getValueName();
                ValueName *retValue = retInst->getReturnValue()->getValueName();
                if(PVVals.find(value) != PVVals.end())
                    PVVals[value].insert(retValue);
                else
                    PVVals[value] = std::set<ValueName*>{retValue};
            }
        }
    }   
  }
};


char FuncPtrPass::ID = 0;
static RegisterPass<FuncPtrPass> X("funcptrpass", "Print function call instruction");

static cl::opt<std::string>
InputFilename(cl::Positional,
              cl::desc("<filename>.bc"),
              cl::init(""));


int main(int argc, char **argv) {
   LLVMContext &Context = getGlobalContext();
   SMDiagnostic Err;
   // Parse the command line to read the Inputfilename
   cl::ParseCommandLineOptions(argc, argv,
                              "FuncPtrPass \n My first LLVM too which does not do much.\n");


   // Load the input module
   std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
   if (!M) {
      Err.print(argv[0], errs());
      return 1;
   }

   llvm::legacy::PassManager Passes;
   	
   ///Remove functions' optnone attribute in LLVM5.0
   Passes.add(new EnableFunctionOptPass());
   ///Transform it to SSA
   Passes.add(llvm::createPromoteMemoryToRegisterPass());

   /// Your pass to print Function and Call Instructions
   Passes.add(new FuncPtrPass());
   Passes.run(*M.get());
}