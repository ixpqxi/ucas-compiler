//==--- tools/clang-check/ClangInterpreter.cpp - Clang Interpreter tool --------------===//
//===----------------------------------------------------------------------===//
#include <stdio.h>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

#define DEBUG

using namespace clang;
using namespace std;

class StackFrame {
	/// StackFrame maps Variable Declaration to Value
	/// Which are either integer or addresses (also represented using an Integer value)
	std::map<Decl*, int> mVars;
	std::map<Stmt*, int> mExprs;
	std::map<Decl*, std::vector<int>> mArrays;
	/// The current stmt
	Stmt * mPC;
public:
	StackFrame() : mVars(), mExprs(), mPC() {
	}

	StackFrame(std::map<Decl*, int> &lVars) :  mExprs(), mPC() {
		mVars.insert(lVars.begin(), lVars.end());
	}

	void bindDecl(Decl* decl, int val) {
		mVars[decl] = val;
	}
	int getDeclVal(Decl * decl) {
		assert (mVars.find(decl) != mVars.end());
		return mVars.find(decl)->second;
	}
	void bindStmt(Stmt * stmt, int val) {
		mExprs[stmt] = val;
	}
	int getStmtVal(Stmt * stmt) {
		assert (mExprs.find(stmt) != mExprs.end());
		return mExprs[stmt];
	}
	void bindArrayDecl(Decl *decl, int size) {
		mArrays[decl] = std::vector<int>(size);
	}
	void bindArrayVal(Decl *decl, int index, int val){
		assert (mArrays.find(decl) != mArrays.end());
		mArrays[decl][index] = val;
	}
	int getArrayVal(Decl *decl, int index){
		assert (mArrays.find(decl) != mArrays.end());
		return mArrays[decl][index];
	}

	void setPC(Stmt * stmt) {
		mPC = stmt;
	}
	Stmt * getPC() {
		return mPC;
	}
};

/// Heap maps address to a value

class Heap {
private:
	std::map<int, void*> space;

public:
	int Malloc(int size) {
		int start = space.size();
		for(int i=0; i<size/4; i++)
			space[start+i] = (void*)malloc(4);
		return start;
	}
	void Free (int addr) {
		assert (space.find(addr) != space.end());
		free(space[addr]);
		space[addr] = nullptr;
	}
	void Update(int addr, int val) {
		assert (space.find(addr) != space.end() && space[addr]);
		*((int*)(space[addr])) = val;
	}
	int get(int addr) {
		assert (space.find(addr) != space.end() && space[addr]);
		return *((int*)(space[addr]));
	}
};

class Environment {
	std::vector<StackFrame> mStack;
	Heap mHeap;

	FunctionDecl * mFree;				/// Declartions to the built-in functions
	FunctionDecl * mMalloc;
	FunctionDecl * mInput;
	FunctionDecl * mOutput;

	FunctionDecl * mEntry;
	std::map<std::string, FunctionDecl*> function_list;
	std::map<Decl*, int> gVars;

public:
	/// Get the declartions to the built-in functions
	Environment() : mStack(), mFree(NULL), mMalloc(NULL), mInput(NULL), mOutput(NULL), mEntry(NULL) {
	}

	/// Initialize the Environment
	void init(TranslationUnitDecl * unit) {
		for (TranslationUnitDecl::decl_iterator i =unit->decls_begin(), e = unit->decls_end(); i != e; ++ i) {
			if (FunctionDecl * fdecl = dyn_cast<FunctionDecl>(*i) ) {
				if (fdecl->getName().equals("FREE")) mFree = fdecl;
				else if (fdecl->getName().equals("MALLOC")) mMalloc = fdecl;
				else if (fdecl->getName().equals("GET")) mInput = fdecl;
				else if (fdecl->getName().equals("PRINT")) mOutput = fdecl;
				else if (fdecl->getName().equals("main")) mEntry = fdecl;
				else {
					function_list[fdecl->getName()] = fdecl;
				}
			}else if(VarDecl * vdecl = dyn_cast<VarDecl>(*i)) {
				int value = 0;
				Expr *initExpr = vdecl->getInit();
				if(initExpr)
					if (IntegerLiteral *intLit = dyn_cast<IntegerLiteral>(initExpr)) {
						value = intLit->getValue().getZExtValue();
					}
				gVars[vdecl] = value;
			}
		}

		mStack.push_back(StackFrame(gVars));
	}

	FunctionDecl * getEntry() {
		return mEntry;
	}

   	/// !TODO Support comparison operation
	void binop(BinaryOperator *bop) {
		Expr * left = bop->getLHS();
		Expr * right = bop->getRHS();

		if (bop->isAssignmentOp()) {
			int val = mStack.back().getStmtVal(right);

			if (DeclRefExpr * declexpr = dyn_cast<DeclRefExpr>(left)) {
				Decl * decl = declexpr->getFoundDecl();
				mStack.back().bindDecl(decl, val);
			}else if(UnaryOperator *unop = dyn_cast<UnaryOperator>(left)){
				if(unop->getOpcodeStr(unop->getOpcode()) == "*"){
					int addr = mStack.back().getStmtVal(left);
					mHeap.Update(addr, val);
					cout << "update addr: " << addr << "to: " << val<< endl;
					return;
				}
			}else{
				if(ArraySubscriptExpr *arrayexpr = dyn_cast<ArraySubscriptExpr>(left)){
					Decl *arraydecl = nullptr;
					Expr *baseexpr = arrayexpr->getBase();
					if(ImplicitCastExpr *castexpr = dyn_cast<ImplicitCastExpr>(baseexpr)) {
						Expr *t = castexpr->getSubExpr();
						if(DeclRefExpr *d = dyn_cast<DeclRefExpr>(t)){
							arraydecl = d->getDecl();
						}
					}
					Expr * iexpr = arrayexpr->getIdx();
					resolver(iexpr);
					int index = mStack.back().getStmtVal(iexpr);
					
					mStack.back().bindArrayVal(arraydecl, index, val);
					cout << "array[" << index << "] = " << val << endl; 
					return;
				}else{
					resolver(left);
				}
			}
			mStack.back().bindStmt(bop, val);
		} else if (bop->isComparisonOp()) {
			int rval = 0;
			int lval = 0;

			// if (IntegerLiteral *intLit = dyn_cast<IntegerLiteral>(right)) {
			// 	rval = intLit->getValue().getZExtValue();
			// } else {
			// 	resolver(right);
			// 	rval = mStack.back().getStmtVal(right);
			// }
			rval = mStack.back().getStmtVal(right);

			// resolver(left);
			lval = mStack.back().getStmtVal(left);
			cout << "comp" << endl;

			std::string opcode = bop->getOpcodeStr();
			bool comprst = false;
			if(opcode == ">"){
				comprst = lval > rval;
			}else if(opcode == "<"){
				comprst = lval < rval;
			}else if(opcode == "=="){
				comprst = lval == rval;
			}
			mStack.back().bindStmt(bop, comprst);
		}else if(bop->isAdditiveOp()){
			int lval = 0;
			int rval = 0;

			// if (IntegerLiteral *intLit = dyn_cast<IntegerLiteral>(left)) {
			// 	lval = intLit->getValue().getZExtValue();
			// }else{
			// 	resolver(left);
			// 	lval = mStack.back().getStmtVal(left);
			// }
			
			// if (IntegerLiteral *intLit = dyn_cast<IntegerLiteral>(right)) {
			// 	rval = intLit->getValue().getZExtValue();
			// }else{
			// 	resolver(right);
			// 	rval = mStack.back().getStmtVal(right);
			// }
			lval = mStack.back().getStmtVal(left);
			rval = mStack.back().getStmtVal(right);
			// cout << "is bop point"  << bop->getType()->isPointerType() << endl;

			std::string opcode = bop->getOpcodeStr();
			if(opcode == "+"){
				cout << lval << "+" << rval << endl;
				mStack.back().bindStmt(bop, lval+rval);
			}else if(opcode == "-"){
				mStack.back().bindStmt(bop, lval-rval);
			}
		}else if(bop->isMultiplicativeOp()){
			int lval = 0;
			int rval = 0;

			if (IntegerLiteral *intLit = dyn_cast<IntegerLiteral>(left)) {
				lval = intLit->getValue().getZExtValue();
			}else{
				resolver(left);
				lval = mStack.back().getStmtVal(left);
			}
			
			if (IntegerLiteral *intLit = dyn_cast<IntegerLiteral>(right)) {
				rval = intLit->getValue().getZExtValue();
			}else{
				resolver(right);
				rval = mStack.back().getStmtVal(right);

				vector<int> *list = new vector<int>(10);
				list++;
			}

			std::string opcode = bop->getOpcodeStr();
			if(opcode == "*"){
				mStack.back().bindStmt(bop, lval * rval);
			}
		}
	}

	void decl(DeclStmt * declstmt) {
		for (DeclStmt::decl_iterator it = declstmt->decl_begin(), ie = declstmt->decl_end();
				it != ie; ++ it) {
			Decl * decl = *it;
			if (VarDecl * vardecl = dyn_cast<VarDecl>(decl)) {
				Expr *expr = vardecl->getInit();
				if(vardecl->getType()->isArrayType()) {
#ifdef DEBUG
					cout << "array decl" << endl;
#endif						
					if (const ConstantArrayType *constArray = dyn_cast<ConstantArrayType>(vardecl->getType())) {
						int size = constArray->getSize().getZExtValue();
						mStack.back().bindArrayDecl(vardecl, size);
					}
				}else if(vardecl->getType()->isPointerType()) {
					cout << "point decl" << endl;
					mStack.back().bindDecl(vardecl, 0);
				}else if(vardecl->getType()->isIntegerType()){
					if(expr){
						if(IntegerLiteral *intLit = dyn_cast<IntegerLiteral>(expr)){
							mStack.back().bindDecl(vardecl, intLit->getValue().getZExtValue());
						}
						else
							mStack.back().bindDecl(vardecl, 0);
						
					}else{
						mStack.back().bindDecl(vardecl, 0);
					}
				}
			}
		}
	}
	void declref(DeclRefExpr * declref) {
		mStack.back().setPC(declref);
		if (declref->getType()->isIntegerType()) {
			Decl* decl = declref->getFoundDecl();
			int val = mStack.back().getDeclVal(decl);
			mStack.back().bindStmt(declref, val);
		} else if(declref->getType()->isPointerType() 
			&& !declref->getType()->isFunctionPointerType()) {
			cout << "declref test" << endl;
			Decl* decl = declref->getFoundDecl();
			int val = mStack.back().getDeclVal(decl);
			mStack.back().bindStmt(declref, val);
		} else if (declref->getType()->isArrayType()) {

		}
	}

	void cast(CastExpr * castexpr) {
		mStack.back().setPC(castexpr);
		if (castexpr->getType()->isIntegerType()) {
			Expr *expr = castexpr->getSubExpr();
			if(UnaryOperator *unop = dyn_cast<UnaryOperator>(expr)){
				if((unop->getOpcodeStr(unop->getOpcode()) == "*")){
					int addr = mStack.back().getStmtVal(expr);
					mStack.back().bindStmt(castexpr, mHeap.get(addr));
				}
			}else{
				int val = mStack.back().getStmtVal(expr);
				mStack.back().bindStmt(castexpr, val);
			}
		}else if(castexpr->getType()->isPointerType() && !castexpr->getType()->isFunctionPointerType()) {
			Expr *expr = castexpr->getSubExpr();
			if(UnaryOperator *unop = dyn_cast<UnaryOperator>(expr)){
				if((unop->getOpcodeStr(unop->getOpcode()) == "*")){
					int addr = mStack.back().getStmtVal(expr);
					mStack.back().bindStmt(castexpr, mHeap.get(addr));
				}
			} else if (DeclRefExpr *declexpr = dyn_cast<DeclRefExpr>(expr)) {
				if(declexpr->getType()->isArrayType()){
					cout << "point decl test" << endl;
				}else{
					int addr = mStack.back().getStmtVal(expr);
					mStack.back().bindStmt(castexpr, addr);
				}
			}else{
				int addr = mStack.back().getStmtVal(expr);
				mStack.back().bindStmt(castexpr, addr);
			}
		}
	}

	/// !TODO Support Function Call
	void call(CallExpr * callexpr) {
		mStack.back().setPC(callexpr);
		int val = 0;
		FunctionDecl * callee = callexpr->getDirectCallee();
		if (callee == mInput) {
			llvm::errs() << "Please Input an Integer Value : ";
			scanf("%d", &val);

			mStack.back().bindStmt(callexpr, val);
		} else if (callee == mOutput) {
			Expr *decl = callexpr->getArg(0);
			val = mStack.back().getStmtVal(decl);
			llvm::errs() << val << "\n";
		} else if (callee == mMalloc) {
#ifdef DEBUG
			cout << "call malloc" << endl;
#endif
			Expr *sexpr = callexpr->getArg(0);
			int size = mStack.back().getStmtVal(sexpr);
			int addr = mHeap.Malloc(size);
			mStack.back().bindStmt(callexpr, addr);
#ifdef DEBUG
			cout << "malloc size: " << size
			<< "\tmalloc addr: " << addr << endl;
#endif
		} else if (callee == mFree){
#ifdef DEBUG
			cout << "call free" << endl;
#endif
			Expr *argexpr = callexpr->getArg(0);
			int addr = mStack.back().getStmtVal(argexpr);
			mHeap.Free(addr);
#ifdef DEBUG
			cout << "free addr: " << addr << endl;
#endif		

		} else {
			/// You could add your code here for Function call Return
			FunctionDecl::param_const_iterator paramIt = callee->param_begin();
    		FunctionDecl::param_const_iterator paramEnd = callee->param_end();

			for (unsigned i = 0, e = callexpr->getNumArgs(); i != e; ++i) {
				Expr *argExpr = callexpr->getArg(i);
				resolver(argExpr);
				if (paramIt != paramEnd) {
					ParmVarDecl *paramDecl = *paramIt;
					int argVal = mStack.back().getStmtVal(argExpr);
					mStack.back().bindDecl(paramDecl, argVal);
					cout << "arg" << i << ": " << argVal << endl;
					paramIt++;
				}
			}
			int val = exec_func(callee);
        	mStack.back().bindStmt(callexpr, val);
		}
	}

	void resolveIf(IfStmt *ifstmt) {
		int val = 0;
		bool condrst;
		Expr *condExpr = ifstmt->getCond();

		resolver(condExpr);
		condrst = mStack.back().getStmtVal(condExpr);
		cout << "if expr " << condrst << endl;

		if(condrst){
			Stmt *thenstmt = ifstmt->getThen();
			if(CompoundStmt *comp = dyn_cast<CompoundStmt>(thenstmt)){
				for(auto &s : comp->body()){
					resolver(s);
					val = mStack.back().getStmtVal(s);
				}
			}else{
				resolver(thenstmt);
				val = mStack.back().getStmtVal(thenstmt);
			}
			cout << "then val: " << val << endl;
		} else if(ifstmt->hasElseStorage()) {
			Stmt *elsestmt = ifstmt->getElse();
			if(CompoundStmt *comp = dyn_cast<CompoundStmt>(elsestmt)){
				for(auto &s : comp->body()){
					resolver(s);
					val = mStack.back().getStmtVal(s);
				}
			}else{
				resolver(elsestmt);
				val = mStack.back().getStmtVal(elsestmt);
			}
			cout << "else val: " << val << endl;
		}
		
		mStack.back().bindStmt(ifstmt, val);
		cout << "end IF" << endl;
	}

	void resolveWhile(WhileStmt *whilestmt){
		Expr *condexpr = whilestmt->getCond();
		resolver(condexpr);
		int condrst = mStack.back().getStmtVal(condexpr);
		
		while(condrst){
			Stmt *whilebody =  whilestmt->getBody();
			if(CompoundStmt *comp = dyn_cast<CompoundStmt>(whilebody)){
				for(auto &s : comp->body()){
					resolver(s);
				}
			}else{
				resolver(whilebody);
			}

			resolver(condexpr);
			condrst = mStack.back().getStmtVal(condexpr);
		}

		mStack.back().bindStmt(whilestmt, condrst);
	}

	void resolveFor(ForStmt *forstmt){
		Stmt *stmt = forstmt->getInit();
		if(stmt)
			resolver(stmt);

		Expr *condexpr = forstmt->getCond();
		Stmt *incstmt = forstmt->getInc();

		resolver(condexpr);
		int condrst = mStack.back().getStmtVal(condexpr);

		cout << "condrst " << condrst << endl;
		while(condrst){
			Stmt *forbody =  forstmt->getBody();
			if(CompoundStmt *comp = dyn_cast<CompoundStmt>(forbody)){
				for(auto &s : comp->body()){
					resolver(s);
				}
			}else{
				resolver(forbody);
			}

			resolver(incstmt);

			resolver(condexpr);
			condrst = mStack.back().getStmtVal(condexpr);
		}
	}

	void resolveArraySubscriptExpr(ArraySubscriptExpr *expr){
		std::cout << "ArraySubscriptExpr OP" << std::endl;
		Expr *bexpr = expr->getBase();
		Expr *iexpr = expr->getIdx();

		Decl *arraydecl = nullptr;
		if(ImplicitCastExpr *castexpr = dyn_cast<ImplicitCastExpr>(bexpr)) {
			Expr *t = castexpr->getSubExpr();
			if(DeclRefExpr *d = dyn_cast<DeclRefExpr>(t)){
				arraydecl = d->getDecl();
			}
		}

		int index = mStack.back().getStmtVal(iexpr);
		
		int val = mStack.back().getArrayVal(arraydecl, index);
		cout << "array[" << index << "] = " << val << endl;
		mStack.back().bindStmt(expr, val);
	}

	int exec_func(FunctionDecl *fdecl) {
		int val = 0;
		CompoundStmt *compoundStmt = dyn_cast<CompoundStmt>(const_cast<Stmt*>(fdecl->getBody()));

		int i = 0;
		for(auto &stmt : compoundStmt->body()) {
			cout << "cul " << i++ << endl;
			resolver(stmt);
			val = mStack.back().getStmtVal(stmt);
		}
		
		return val;
	}

	void resolver(Stmt *stmt) {
		if(IfStmt *ifstmt = dyn_cast<IfStmt>(stmt)){
			resolveIf(ifstmt);
		}else if(ReturnStmt *retstmt = dyn_cast<ReturnStmt>(stmt)){
			cout << "retrun expr" << endl;
			resolver(retstmt->getRetValue());
			resolveReturn(retstmt);
		}else if(BinaryOperator *bop = dyn_cast<BinaryOperator>(stmt)){
			resolver(bop->getLHS());
			resolver(bop->getRHS());
			binop(bop);
		}else if(CastExpr *castexpr = dyn_cast<CastExpr>(stmt)){
			resolver(castexpr->getSubExpr());
			cast(castexpr);
		}else if(DeclRefExpr *declrefexpr = dyn_cast<DeclRefExpr>(stmt)){
			declref(declrefexpr);
		}else if(CallExpr *callexpr = dyn_cast<CallExpr>(stmt)){
			call(callexpr);
		}else if(UnaryOperator *unop = dyn_cast<UnaryOperator>(stmt)){
			resolver(unop->getSubExpr());
			resolveUnaryOperator(unop);
		}else if(WhileStmt *whilestmt = dyn_cast<WhileStmt>(stmt)){
			cout << "while stmt" << endl;
			resolveWhile(whilestmt);
		}else if(DeclStmt *declstmt = dyn_cast<DeclStmt>(stmt)) {
			decl(declstmt);
			mStack.back().bindStmt(stmt, 0);
		}else if(ForStmt *forstmt = dyn_cast<ForStmt>(stmt)) {
			resolveFor(forstmt);
			mStack.back().bindStmt(stmt, 0);
		}else if(IntegerLiteral *expr = dyn_cast<IntegerLiteral>(stmt)){
			resolveIntegerLiteral(expr);
		}else if(ArraySubscriptExpr *arrayexpr = dyn_cast<ArraySubscriptExpr>(stmt)){
			resolver(arrayexpr->getIdx());
			resolveArraySubscriptExpr(arrayexpr);
		}
	}

	void resolveReturn(ReturnStmt *retstmt){
		mStack.back().setPC(retstmt);
		Expr *expr = retstmt->getRetValue();
		int val = mStack.back().getStmtVal(expr);
		mStack.back().bindStmt(retstmt, val);
	}

	void resolveUnaryOperator(UnaryOperator *unop){
		cout << "UnaryOperator OP" << endl;
		Expr *expr = unop->getSubExpr();
		string opcode =  unop->getOpcodeStr(unop->getOpcode());

		int val = mStack.back().getStmtVal(expr);

		if(opcode == "-")
			mStack.back().bindStmt(unop, -val);
		else if(opcode == "*"){
			cout << "get addr: " << val << endl;
			mStack.back().bindStmt(unop, val);
		}
	}

	void resolveUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *expr) {
		std::string type = expr->getArgumentType().getAsString();
		if(type == "int"){
			mStack.back().bindStmt(expr, 4);
		}else {
			mStack.back().bindStmt(expr, 4);
		}
	}

	void resolveIntegerLiteral(IntegerLiteral *expr) {
		cout << "IntegerLiteral OP" << endl;
		int val = expr->getValue().getZExtValue();
		mStack.back().bindStmt(expr, val);
	}

	void resolveParenExpr(ParenExpr *expr) {
		cout << "ParenExpr OP" << endl;
		Expr *subexpr = expr->getSubExpr();
		int val = mStack.back().getStmtVal(subexpr);
		mStack.back().bindStmt(expr, val);
		cout << "resolveParenExpr val: " << val << endl;
	}
};


