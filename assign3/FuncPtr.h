#include <llvm/IR/Function.h>
#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/IntrinsicInst.h>

#include "Dataflow.h"
#include "set"
using namespace llvm;

// #define DEBUG

class Compare
{
public:
    bool operator()(CallInst* i1, CallInst* i2)
    {
        return i1->getDebugLoc().getLine() < i2->getDebugLoc().getLine();
    }
};

struct FuncPtrInfo
{
    std::map<Value*, std::set<Value*>> S;
    std::map<CallInst*, std::set<Value*>, Compare> callFuncs;
    std::set<Value*> funcRet;
    FuncPtrInfo() : S(), callFuncs() {}
    FuncPtrInfo(const FuncPtrInfo & info) : S(info.S), callFuncs(info.callFuncs), funcRet(info.funcRet) {}

    // 重载集合操作
    bool operator == (const FuncPtrInfo & info) const {
        return S == info.S;
    }

    void addCallInst(CallInst *callInst, Value *value) {
        if(callFuncs.find(callInst) != callFuncs.end())
            callFuncs[callInst].insert(value);
        else
            callFuncs[callInst] = std::set<Value*>{value};
    }
};

inline raw_ostream &operator<<(raw_ostream &out, const FuncPtrInfo &info) {
   for(auto entry : info.S) {
        out << entry.first << " "  <<  entry.first->getName() << "{";
        for(auto v : entry.second) {
            out <<  v->getName() << ",";
        }
        out << "}";
   }
   return out;
}

template<class T>
class FuncPtrVisitor : public DataflowVisitor<struct FuncPtrInfo> {
public:
    FuncPtrVisitor() {}
    
    // mint操作
    void merge(FuncPtrInfo *dest, const FuncPtrInfo & src) override {
        for(auto a : src.S) {
            if(dest->S.find(a.first) != dest->S.end()) {
                (dest->S)[a.first].insert(a.second.begin(), a.second.end());
            } else {
                std::set<Value*> tmp;
                tmp.insert(a.second.begin(), a.second.end());
                (dest->S)[a.first] = tmp;
            }
        }

        for(auto a : src.callFuncs) {
            if(dest->callFuncs.find(a.first) != dest->callFuncs.end()) {
                (dest->callFuncs)[a.first].insert(a.second.begin(), a.second.end());
            } else {
                (dest->callFuncs)[a.first] = a.second;
            }
        }
    }

    // 转换函数
    void compDFVal(Instruction *inst, FuncPtrInfo * dfval) override {
        if (isa<DbgInfoIntrinsic>(inst)) return;

        if(StoreInst *storeInst = dyn_cast<StoreInst>(inst)) {
#ifdef DEBUG
            storeInst->dump();
#endif
            Value *left = storeInst->getOperand(1);
            Value *right = storeInst->getOperand(0);
            std::set<Value*> lValueList;
            
            if(GetElementPtrInst *getInst = dyn_cast<GetElementPtrInst>(left)) {
                Value *getVar = getInst->getOperand(0);
                // lValueList.insert(getVar);
                if(LoadInst *loadInst = dyn_cast<LoadInst>(getVar)) {
                    Value *value = loadInst->getOperand(0);
                    if(value->getName().endswith(".addr"))
                        lValueList.insert(dfval->S[loadInst].begin(), dfval->S[loadInst].end());
                    else if(GetElementPtrInst *getInst = dyn_cast<GetElementPtrInst>(value)) {
                        Value *v =  getInst->getOperand(0);
                        if(LoadInst *l = dyn_cast<LoadInst>(v)) {
                            if(v->hasName())
                                lValueList.insert(dfval->S[l].begin(), dfval->S[l].end());
                            else {
                                for(auto vt : dfval->S[l])
                                    lValueList.insert(dfval->S[vt].begin(), dfval->S[vt].end());
                            }
                        }else
                           lValueList.insert(v); 
                    } else
                        lValueList.insert(value);
                } else {
                    lValueList.insert(getVar);
                }
            } else if(LoadInst *loadInst = dyn_cast<LoadInst>(left)) {
                Value *value = loadInst->getOperand(0);
                lValueList.insert(value);
            } else {
                lValueList.insert(left);
            }

            std::set<Value*> ptrs;
            if(LoadInst *loadInst = dyn_cast<LoadInst>(right)) {
                //TODO test26
                // Value *value = loadInst->getOperand(0);
                // if(GetElementPtrInst *getInst = dyn_cast<GetElementPtrInst>(value)) {
                    
                // }
                if(dfval->S.find(loadInst) != dfval->S.end()) {
                    for(auto v : dfval->S[loadInst]) {
                        if(dfval->S.find(v) != dfval->S.end())
                            ptrs.insert(dfval->S[v].begin(), dfval->S[v].end());
                        else
                            ptrs.insert(dfval->S[loadInst].begin(), dfval->S[loadInst].end());
                    }
                }
            } else {
                if(dfval->S.find(right) != dfval->S.end()) {
                    ptrs.insert(dfval->S[right].begin(), dfval->S[right].end());
                } else {
                    if(right->getValueName())
                        ptrs.insert(right);
                }
            }

            for(auto lv : lValueList) {
#ifdef DEBUG
                errs() << "\t\t" << lv << " " << lv->getName() << " <=> {";
                for(auto v : ptrs) {
                    errs() << v->getName() << ",";
                }
                errs() << "}\n";
#endif          
                bool flag = false;
                if(dfval->S.find(lv) != dfval->S.end()) {
                    for(auto l : dfval->S[lv])
                        if(dfval->S[l].size() > 1) {
                            flag = true;
                            break;
                        }
                }
                if(flag){
                    dfval->S[lv].insert(ptrs.begin(), ptrs.end());
                }else
                    dfval->S[lv] = ptrs;
            }
        } else if(LoadInst *loadInst = dyn_cast<LoadInst>(inst)) {
#ifdef DEBUG
            loadInst->dump();
#endif
            Value *right = loadInst->getOperand(0);
            Value *left = dyn_cast<Value>(loadInst);

            std::set<Value*> ptrs;
            if(GetElementPtrInst *getInst = dyn_cast<GetElementPtrInst>(right)) {
                // right = getInst->getOperand(0);

                Value *value = getInst->getOperand(0);
                if(LoadInst *loadInst = dyn_cast<LoadInst>(value)) {
                    if(LoadInst *LoadI = dyn_cast<LoadInst>(loadInst->getOperand(0)))
                        for(auto v : dfval->S[loadInst])
                            ptrs.insert(dfval->S[v].begin(), dfval->S[v].end());
                    else
                        ptrs.insert(dfval->S[loadInst].begin(), dfval->S[loadInst].end());
                } else {
                    if(dfval->S.find(value) != dfval->S.end())
                        ptrs.insert(dfval->S[value].begin(), dfval->S[value].end());
                }
            } else {
                if(dfval->S.find(right) != dfval->S.end()) 
                    ptrs.insert(dfval->S[right].begin(), dfval->S[right].end());
            }
#ifdef DEBUG
            errs() << "\t\t" << left << " " <<  left->getName() << " <=> {";
            for(auto v : ptrs) {
                errs() << v->getName() << ",";
            }
            errs() << "}\n";
#endif      
            dfval->S[left].insert(ptrs.begin(), ptrs.end());
        } else if(CallInst *callInst = dyn_cast<CallInst>(inst)) {
            Value *callValue = callInst->getCalledValue();
            DataflowResult<FuncPtrInfo>::Type result;
#ifdef DEBUG
            errs() << "call Start " << *dfval << "\n";
#endif
            if(dfval->S.find(callValue) != dfval->S.end()) {
                for(auto v : dfval->S[callValue]) {
                    if(Function *callFunc = dyn_cast<Function>(v)) {
#ifdef DEBIG
                        errs() << callInst->getDebugLoc().getLine() << " call ptr " << v->getName() << "\n";
#endif
                        resolvFunc(callInst, dfval, &result, callFunc);
                        dfval->addCallInst(callInst, v);
                    } else if(dfval->S.find(v) != dfval->S.end()) {
                        for(auto v2 : dfval->S[v]) {
                            if(Function *callFunc = dyn_cast<Function>(v2)) {
                                resolvFunc(callInst, dfval, &result, callFunc);
                                dfval->addCallInst(callInst, v2);
                            }
                        }
                    }
                }
            } else {
                Function *callFunc = callInst->getCalledFunction();
                if(callFunc && !callFunc->getName().consume_front("llvm.")) {
#ifdef DEBIG
                    errs() << callInst->getDebugLoc().getLine() << " call func " << callFunc->getName() << "\n";
#endif
                    resolvFunc(callInst, dfval, &result, callFunc);
                    dfval->addCallInst(callInst, callFunc);
                } else if(callFunc && callFunc->getName().consume_front("llvm.memcpy")) {
                    Value *left = callInst->getArgOperand(0);
                    Value *right = callInst->getArgOperand(1);

                    if(BitCastInst *bitCastInst = dyn_cast<BitCastInst>(left)) {
                        Value *tmp = bitCastInst->getOperand(0);
                        if(LoadInst *loadInst = dyn_cast<LoadInst>(tmp))
                            left = loadInst->getOperand(0);
                        else
                            left = tmp;
                    }

                    if(BitCastInst *bitCastInst = dyn_cast<BitCastInst>(right)) {
                        Value *tmp = bitCastInst->getOperand(0);
                        if(dfval->S.find(tmp) != dfval->S.end()) {
                            right = tmp;
                        }
                    }

                    dfval->S[left] = dfval->S[right];
                }
            }
            if(!result.empty()){
                auto r = --result.end();
                FuncPtrInfo callEndInfo = r->second.second;
                for(auto orgV : dfval->S) {
                    dfval->S[orgV.first] = callEndInfo.S[orgV.first];
                }
            }
#ifdef DEBUG
            errs() << "call End " << *dfval << "\n";
#endif

            for(auto bbs : result) {
                FuncPtrInfo info = bbs.second.second;
                for(auto c : info.callFuncs) {
                    if(dfval->callFuncs.find(c.first) != dfval->callFuncs.end())
                        dfval->callFuncs[c.first].insert(c.second.begin(), c.second.end());
                    else
                        dfval->callFuncs[c.first] = c.second;
                }

                if(info.funcRet.size() > 0) {
                    dfval->S[callInst].insert(info.funcRet.begin(), info.funcRet.end());
                }
            }
        } else if(ReturnInst *retInst = dyn_cast<ReturnInst>(inst)) {
            Value *retValue = retInst->getReturnValue();
            if(retValue){
                std::set<Value*> ret;
                if(dfval->S.find(retValue) != dfval->S.end())
                    ret.insert(dfval->S[retValue].begin(), dfval->S[retValue].end());
                else
                    ret.insert(retValue);
                dfval->funcRet.insert(ret.begin(), ret.end());
            }
        } else if(BitCastInst *bitCastInst = dyn_cast<BitCastInst>(inst)) {
            Value *value = bitCastInst->getOperand(0);
            if(LoadInst *loadInst = dyn_cast<LoadInst>(value)) {

            }
        }
        // else if(GetElementPtrInst *getInst = dyn_cast<GetElementPtrInst>(inst)) {
        //     Value *getVar = getInst->getOperand(0);

        //     if(dfval->S.find(getVar) != dfval->S.end())
        //         dfval->S[getInst].insert(dfval->S[getVar].begin(), dfval->S[getVar].end());
        //     else
        //         dfval->S[getInst].insert(getVar);
        // }
    }

    void resolvFunc(CallInst *callInst, FuncPtrInfo *dfval, typename  DataflowResult<T>::Type *result, Function *callFunc) {
        FuncPtrInfo info = *dfval;
        // std::map<Value*, Value*> args;
        
        for(int i=0; i<callInst->getNumArgOperands(); i++){
            std::set<Value*> argPtrs;
            bool flag = false;

            Value *left = dyn_cast<Value>(callFunc->getArg(i));
            Value *right = callInst->getArgOperand(i);

            if(GetElementPtrInst *getInst = dyn_cast<GetElementPtrInst>(right)) {
                Value *value = getInst->getOperand(0);
                argPtrs.insert(value);      
            } else if(LoadInst *loadInst = dyn_cast<LoadInst>(right)) {
                Value *value = loadInst->getOperand(0);
                if(GetElementPtrInst *getInst = dyn_cast<GetElementPtrInst>(value)) {
                    Value *v = getInst->getOperand(0);
                    if(LoadInst *loadI = dyn_cast<LoadInst>(v))
                        argPtrs.insert(loadI);
                    else
                        argPtrs.insert(dfval->S[loadInst].begin(), dfval->S[loadInst].end());
                } else if(value->getName().endswith(".addr"))
                    argPtrs.insert(dfval->S[loadInst].begin(), dfval->S[loadInst].end());
                else
                    argPtrs.insert(value);
            } else {
                argPtrs.insert(right);
            }
#ifdef DEBUG
            errs() << left->getName() << " <=> {";
            for(auto v : argPtrs) {
                errs() << v << " " << v->getName() << ",";
            }
            errs() << "}\n";
#endif

            info.S[left] = argPtrs;
        }

        FuncPtrVisitor visitor;
        FuncPtrInfo initval;
        compForwardDataflow(callFunc, &visitor, result, info);
    }
};
