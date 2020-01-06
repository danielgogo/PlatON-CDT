
#include "llvm/IR/Type.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/FileSystem.h"
#include <set>
#include <map>
#include <vector>
#include "../Option.h"

using namespace llvm;
using namespace json;
using namespace std;

json::Value handleSubprogram(DISubprogram*  SP, vector<DILocalVariable*> &LVs, StringRef SPType);

StringRef getAnnoteKind(llvm::Value* cs) {
  if(ConstantStruct* CS=dyn_cast<ConstantStruct>(cs))
    if(CS->getNumOperands()>1)
      if(ConstantExpr* CE = dyn_cast<ConstantExpr>(CS->getAggregateElement(1)))
        if(CE->getNumOperands()>0)
          if(GlobalVariable* ActionString = dyn_cast<GlobalVariable>(CE->getOperand(0)))
            if (ConstantDataArray *arr = dyn_cast<ConstantDataArray>(ActionString->getInitializer()))
              return arr->getAsCString();

  return "";
}

DISubprogram* getFuncInfo(llvm::Value* cs){
  if(ConstantStruct* CS=dyn_cast<ConstantStruct>(cs)) {
    if (CS->getNumOperands() > 0)
      if (ConstantExpr *CE = dyn_cast<ConstantExpr>(CS->getAggregateElement((unsigned) 0)))
        if(CE->getNumOperands()>0)
          if(Function* F = dyn_cast<Function>(CE->getOperand(0)))
            if(DISubprogram* DI = dyn_cast<DISubprogram>(F->getMetadata(LLVMContext::MD_dbg)))
              return DI;
  }
  return nullptr;
}


typedef map<llvm::DISubprogram*, StringRef> SubprogramMap;
typedef multimap<llvm::DISubprogram*, llvm::DILocalVariable*> ParamsMap;

void exportParams(DISubprogram* SP, ParamsMap &PMap, vector<DILocalVariable*> &Params){

  unsigned num = SP->getType()->getTypeArray()->getNumOperands();
  Params.resize(num-2);
  for(auto iter = PMap.lower_bound(SP); iter != PMap.upper_bound(SP); iter++) {
    DILocalVariable* LV = iter->second;
    Params[LV->getArg()-2] = LV;
  }

}

void collectParams(Function* DbgDecl, SubprogramMap &SPMap, ParamsMap &PMap){
  if(DbgDecl==nullptr)return;
  for(User* U : DbgDecl->users()){
    if(CallInst* CI = dyn_cast<CallInst>(U)){
      MetadataAsValue* MV = cast<MetadataAsValue>(CI->getOperand(1));
      DILocalVariable* LV = cast<DILocalVariable>(MV->getMetadata());
      if(LV->isParameter() && LV->getArg()>1){
        if(DISubprogram* SP = dyn_cast<DISubprogram>(LV->getScope())){
          if(SPMap.find(SP) != SPMap.end()){
            PMap.insert(make_pair(SP, LV));
          }
        }
      }
    }
  }
}

void collectAnnote(GlobalVariable* annote, SubprogramMap &SPMap){
  if(annote==nullptr)return;
  if(ConstantArray* annotes = dyn_cast<ConstantArray>(annote->getInitializer())) {
    for (auto cs:annotes->operand_values()) {
      StringRef kind = getAnnoteKind(cs);
      DISubprogram* SP = getFuncInfo(cs);
      if(kind=="Action"){
        SPMap[SP] = "function";
      } else if(kind=="Event"){
        SPMap[SP] = "event";
      } 
    }
  }
}


json::Value makeAbi(Module* M){

  SubprogramMap SPMap;
  ParamsMap PMap;

  GlobalVariable* Annote = M->getGlobalVariable("llvm.global.annotations");
  collectAnnote(Annote, SPMap);

  if(SPMap.size()==0)
    report_fatal_error("have not Action and Event");

  collectParams(M->getFunction("llvm.dbg.declare"), SPMap, PMap);
  collectParams(M->getFunction("llvm.dbg.value"), SPMap, PMap);

  json::Value Funcs = {};

  for(auto iter : SPMap){
    DISubprogram* SP = iter.first;
    StringRef SPType = iter.second;

    vector<DILocalVariable*> Params;
    exportParams(SP, PMap, Params);
    json::Value func = handleSubprogram(SP, Params, SPType);

    Funcs.getAsArray()->push_back(func);
  }

  return Funcs;
}

int GenerateABI(PCCOption &Option, llvm::Module* M){
  SmallString<128> abiPath(Option.Output);
  llvm::sys::path::replace_extension(abiPath, "abi.json");

  json::Value v = makeAbi(M);

//  .str();

  std::error_code EC;
  ToolOutputFile Out(abiPath, EC, sys::fs::F_None);
  if (EC) {
    errs() << EC.message() << '\n';
    return 1; 
  }
  Out.os() << llvm::formatv("{0:4}", v);
  Out.keep();

  return 0;
}