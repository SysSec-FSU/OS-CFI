#include "DDA/DDAPass.h"
#include "DDA/ContextDDA.h"
#include "DDA/DDAClient.h"
#include "DDA/FlowDDA.h"
#include "MemoryModel/PointerAnalysis.h"
#include <limits.h>
#include <llvm/Support/CommandLine.h>
#include <sstream>

using namespace llvm;

char DDAPass::ID = 0;

static cl::opt<unsigned> maxPathLen("maxpath", cl::init(100000),
                                    cl::desc("Maximum path limit for DDA"));

static cl::opt<unsigned>
    maxContextLen("maxcxt", cl::init(3),
                  cl::desc("Maximum context limit for DDA"));

static cl::opt<string> userInputQuery(
    "query", cl::init("all"),
    cl::desc("Please specify queries by inputing their pointer ids"));

static cl::opt<bool> insenRecur(
    "inrecur", cl::init(false),
    cl::desc("Mark context insensitive SVFG edges due to function recursions"));

static cl::opt<bool> insenCycle(
    "incycle", cl::init(false),
    cl::desc("Mark context insensitive SVFG edges due to value-flow cycles"));

static cl::opt<bool> printCPts("cpts", cl::init(false),
                               cl::desc("Dump conditional points-to set "));

static cl::opt<bool>
    printQueryPts("print-query-pts", cl::init(false),
                  cl::desc("Dump queries' conditional points-to set "));

static cl::opt<bool> WPANUM("wpanum", cl::init(false),
                            cl::desc("collect WPA FS number only "));

static RegisterPass<DDAPass> DDAPA("dda",
                                   "Demand-driven Pointer Analysis Pass");

/// register this into alias analysis group
// static RegisterAnalysisGroup<AliasAnalysis> AA_GROUP(DDAPA);

static cl::bits<PointerAnalysis::PTATY> DDASelected(
    cl::desc("Select pointer analysis"),
    cl::values(clEnumValN(PointerAnalysis::FlowS_DDA, "dfs",
                          "Demand-driven flow sensitive analysis"),
               clEnumValN(PointerAnalysis::Cxt_DDA, "cxt",
                          "Demand-driven context- flow- sensitive analysis")));

DDAPass::~DDAPass() {
  // _pta->dumpStat();
  if (_client != NULL)
    delete _client;
}

// [OS-CFI] getHashID(): returns an unique id for an instruction
unsigned long DDAPass::getHashID(const Instruction *inst) {
  std::hash<std::string> hash_fn;
  string str;
  raw_string_ostream rso(str);
  inst->print(rso);
  str += ("[" + inst->getParent()->getParent()->getName().str() + "]");
  return (hash_fn(str) % HASH_ID_RANGE);
}

bool DDAPass::runOnModule(SVFModule module) {
  /// initialization for llvm alias analyzer
  // InitializeAliasAnalysis(this, SymbolTableInfo::getDataLayout(&module));

  // [OS-CFI] list address-taken functions
  unsigned int nModule = module.getModuleNum();
  for (unsigned int im = 0; im < nModule; ++im) {
    Module *md = module.getModule(im);
    for (Module::iterator it = md->begin(); it != md->end(); ++it) {
      Function *fn = &(*it);
      if (fn->hasAddressTaken()) {
        setAddrFunc.insert(fn);
      }
    }
  }

  selectClient(module);

  for (u32_t i = PointerAnalysis::FlowS_DDA; i < PointerAnalysis::Default_PTA;
       i++) {
    if (DDASelected.isSet(i))
      runPointerAnalysis(module, i);
  }

  return false;
}

/// select a client to initialize queries
void DDAPass::selectClient(SVFModule module) {

  if (!userInputQuery.empty()) {
    /// solve function pointer
    if (userInputQuery == "funptr") {
      _client = new FunptrDDAClient(module);
    }
    /// allow user specify queries
    else {
      _client = new DDAClient(module);
      if (userInputQuery != "all") {
        u32_t buf; // Have a buffer
        stringstream ss(
            userInputQuery); // Insert the user input string into a stream
        while (ss >> buf)
          _client->setQuery(buf);
      }
    }
  } else {
    assert(false && "Please specify query options!");
  }

  _client->initialise(module);
}

/// Create pointer analysis according to specified kind and analyze the module.
void DDAPass::runPointerAnalysis(SVFModule module, u32_t kind) {

  VFPathCond::setMaxPathLen(maxPathLen);
  ContextCond::setMaxCxtLen(maxContextLen);

  /// Initialize pointer analysis.
  switch (kind) {
  case PointerAnalysis::Cxt_DDA: {
    _pta = new ContextDDA(module, _client);
    break;
  }
  case PointerAnalysis::FlowS_DDA: {
    _pta = new FlowDDA(module, _client);
    break;
  }
  default:
    llvm::outs() << "This pointer analysis has not been implemented yet.\n";
    break;
  }

  if (WPANUM) {
    _client->collectWPANum(module);
  } else {
    /// initialize
    _pta->initialize(module);
    /// compute points-to
    answerQueries(_pta);
    /// finalize
    _pta->finalize();
    if (printCPts)
      _pta->dumpCPts();

    if (_pta->printStat())
      _client->performStat(_pta);

    if (printQueryPts)
      printQueryPTS();

    // [OS-CFI] SUPA completes process and it is time to compute our CFGs
    computeCFG();

    // [OS-CFI] ToDo
    insertIndirectBranch();

    // [OS-CFI] print out our CFGs
    dumpSUPACFG();
    dumpoCFG();
    dumpcCFG();
    dumpatCFG();
  }
}

/*!
 * Initialize queries
 */
void DDAPass::answerQueries(PointerAnalysis *pta) {

  DDAStat *stat = static_cast<DDAStat *>(pta->getStat());
  u32_t vmrss = 0;
  u32_t vmsize = 0;
  analysisUtil::getMemoryUsageKB(&vmrss, &vmsize);
  stat->setMemUsageBefore(vmrss, vmsize);

  _client->answerQueries(pta);

  vmrss = vmsize = 0;
  analysisUtil::getMemoryUsageKB(&vmrss, &vmsize);
  stat->setMemUsageAfter(vmrss, vmsize);
}

/*!
 * Initialize context insensitive Edge for DDA
 */
void DDAPass::initCxtInsensitiveEdges(PointerAnalysis *pta, const SVFG *svfg,
                                      const SVFGSCC *svfgSCC,
                                      SVFGEdgeSet &insensitveEdges) {
  if (insenRecur)
    collectCxtInsenEdgeForRecur(pta, svfg, insensitveEdges);
  else if (insenCycle)
    collectCxtInsenEdgeForVFCycle(pta, svfg, svfgSCC, insensitveEdges);
}

/*!
 * Whether SVFG edge in a SCC cycle
 */
bool DDAPass::edgeInSVFGSCC(const SVFGSCC *svfgSCC, const SVFGEdge *edge) {
  return (svfgSCC->repNode(edge->getSrcID()) ==
          svfgSCC->repNode(edge->getDstID()));
}

/*!
 *  Whether call graph edge in SVFG SCC
 */
bool DDAPass::edgeInCallGraphSCC(PointerAnalysis *pta, const SVFGEdge *edge) {
  const BasicBlock *srcBB = edge->getSrcNode()->getBB();
  const BasicBlock *dstBB = edge->getDstNode()->getBB();

  if (srcBB && dstBB)
    return pta->inSameCallGraphSCC(srcBB->getParent(), dstBB->getParent());

  assert(edge->isRetVFGEdge() == false &&
         "should not be an inter-procedural return edge");

  return false;
}

/*!
 * Mark insensitive edge for function recursions
 */
void DDAPass::collectCxtInsenEdgeForRecur(PointerAnalysis *pta,
                                          const SVFG *svfg,
                                          SVFGEdgeSet &insensitveEdges) {

  for (SVFG::SVFGNodeIDToNodeMapTy::const_iterator it = svfg->begin(),
                                                   eit = svfg->end();
       it != eit; ++it) {

    SVFGEdge::SVFGEdgeSetTy::const_iterator edgeIt = it->second->InEdgeBegin();
    SVFGEdge::SVFGEdgeSetTy::const_iterator edgeEit = it->second->InEdgeEnd();
    for (; edgeIt != edgeEit; ++edgeIt) {
      const SVFGEdge *edge = *edgeIt;
      if (edge->isCallVFGEdge() || edge->isRetVFGEdge()) {
        if (edgeInCallGraphSCC(pta, edge))
          insensitveEdges.insert(edge);
      }
    }
  }
}

/*!
 * Mark insensitive edge for value-flow cycles
 */
void DDAPass::collectCxtInsenEdgeForVFCycle(PointerAnalysis *pta,
                                            const SVFG *svfg,
                                            const SVFGSCC *svfgSCC,
                                            SVFGEdgeSet &insensitveEdges) {

  std::set<NodePair> insensitvefunPairs;

  for (SVFG::SVFGNodeIDToNodeMapTy::const_iterator it = svfg->begin(),
                                                   eit = svfg->end();
       it != eit; ++it) {

    SVFGEdge::SVFGEdgeSetTy::const_iterator edgeIt = it->second->InEdgeBegin();
    SVFGEdge::SVFGEdgeSetTy::const_iterator edgeEit = it->second->InEdgeEnd();
    for (; edgeIt != edgeEit; ++edgeIt) {
      const SVFGEdge *edge = *edgeIt;
      if (edge->isCallVFGEdge() || edge->isRetVFGEdge()) {
        if (this->edgeInSVFGSCC(svfgSCC, edge)) {

          const BasicBlock *srcBB = edge->getSrcNode()->getBB();
          const BasicBlock *dstBB = edge->getDstNode()->getBB();

          if (srcBB && dstBB) {
            NodeID src = pta->getPTACallGraph()
                             ->getCallGraphNode(srcBB->getParent())
                             ->getId();
            NodeID dst = pta->getPTACallGraph()
                             ->getCallGraphNode(dstBB->getParent())
                             ->getId();
            insensitvefunPairs.insert(std::make_pair(src, dst));
            insensitvefunPairs.insert(std::make_pair(dst, src));
          } else
            assert(edge->isRetVFGEdge() == false &&
                   "should not be an inter-procedural return edge");
        }
      }
    }
  }

  for (SVFG::SVFGNodeIDToNodeMapTy::const_iterator it = svfg->begin(),
                                                   eit = svfg->end();
       it != eit; ++it) {
    SVFGEdge::SVFGEdgeSetTy::const_iterator edgeIt = it->second->InEdgeBegin();
    SVFGEdge::SVFGEdgeSetTy::const_iterator edgeEit = it->second->InEdgeEnd();
    for (; edgeIt != edgeEit; ++edgeIt) {
      const SVFGEdge *edge = *edgeIt;

      if (edge->isCallVFGEdge() || edge->isRetVFGEdge()) {
        const BasicBlock *srcBB = edge->getSrcNode()->getBB();
        const BasicBlock *dstBB = edge->getDstNode()->getBB();

        if (srcBB && dstBB) {
          NodeID src = pta->getPTACallGraph()
                           ->getCallGraphNode(srcBB->getParent())
                           ->getId();
          NodeID dst = pta->getPTACallGraph()
                           ->getCallGraphNode(dstBB->getParent())
                           ->getId();
          if (insensitvefunPairs.find(std::make_pair(src, dst)) !=
              insensitvefunPairs.end())
            insensitveEdges.insert(edge);
          else if (insensitvefunPairs.find(std::make_pair(dst, src)) !=
                   insensitvefunPairs.end())
            insensitveEdges.insert(edge);
        }
      }
    }
  }
}

/*!
 * Return alias results based on our points-to/alias analysis
 * TODO: Need to handle PartialAlias and MustAlias here.
 */
llvm::AliasResult DDAPass::alias(const Value *V1, const Value *V2) {
  PAG *pag = _pta->getPAG();

  /// TODO: When this method is invoked during compiler optimizations, the IR
  ///       used for pointer analysis may been changed, so some Values may not
  ///       find corresponding PAG node. In this case, we only check alias
  ///       between two Values if they both have PAG nodes. Otherwise, MayAlias
  ///       will be returned.
  if (pag->hasValueNode(V1) && pag->hasValueNode(V2)) {
    PAGNode *node1 = pag->getPAGNode(pag->getValueNode(V1));
    if (pag->isValidTopLevelPtr(node1))
      _pta->computeDDAPts(node1->getId());

    PAGNode *node2 = pag->getPAGNode(pag->getValueNode(V2));
    if (pag->isValidTopLevelPtr(node2))
      _pta->computeDDAPts(node2->getId());

    return _pta->alias(V1, V2);
  }

  return MayAlias;
}

/*!
 * Print queries' pts
 */
void DDAPass::printQueryPTS() {
  llvm::outs() << "+++++++++++++++++++++++++++[SVF] printQueryPTS "
                  "[SVF]+++++++++++++++++++++++++++"
               << "\n";
  const NodeSet &candidates = _client->getCandidateQueries();
  for (NodeSet::iterator it = candidates.begin(), eit = candidates.end();
       it != eit; ++it) {
    const PointsTo &pts = _pta->getPts(*it);
    _pta->dumpPts(*it, pts);
  }
  llvm::outs() << "\n";
}

// [OS-CFI] ToDo
void DDAPass::insertLabelAfterCall(const llvm::Instruction *callInst) {
  /*Instruction *iInst = (llvm::Instruction *)callInst;

  BasicBlock *iBB = (llvm::BasicBlock *)callInst->getParent();
  Function *FN = (llvm::Function *)iBB->getParent();

  funToCallSiteMap[FN].insert(iInst);*/
}

// [OS-CFI] ToDo
void DDAPass::insertIndirectBranch() {
  /*
  for (std::map<llvm::Function *, std::set<llvm::Instruction *>>::iterator fit =
           funToCallSiteMap.begin();
       fit != funToCallSiteMap.end(); ++fit) {
    Function *FN = fit->first;
    for (Function::iterator b = FN->begin(), be = FN->end(); b != be; ++b) {
      BasicBlock &iBB = *b;
      for (BasicBlock::reverse_iterator i = b->rbegin(), ie = b->rend();
           i != ie; ++i) {
        Instruction &inst = *i;
        if (fit->second.find(&inst) != fit->second.end()) {
          // create a new basic block
          BasicBlock *nBB = BasicBlock::Create(FN->getContext(), "", FN, &iBB);
          iBB.replaceSuccessorsPhiUsesWith(nBB);
          funToBBMap[FN].insert(nBB);


          BBToHashIDMap[nBB] = getHashID(inst);

          // create branch instruction to new basic block
          IRBuilder<> iBuilder(&iBB);
          BranchInst *iBr = iBuilder.CreateBr(nBB);

          // move the branch instruction before the next instruction of call
          // instruction
          iBr->moveAfter(&inst);

          nBB->moveAfter(&iBB);
        }
      }
    }
  }

  for (std::map<llvm::Function *, std::set<llvm::BasicBlock *>>::iterator fit =
           funToBBMap.begin();
       fit != funToBBMap.end(); ++fit) {
    Function *fn = fit->first;

    BasicBlock *nBB = BasicBlock::Create(fn->getContext(), "", fn, nullptr);
    IRBuilder<> iBuilder(nBB);

    unsigned int nBr = fit->second.size();

    // integer pointet type
    PointerType *int8PtTy = Type::getInt8PtrTy(fn->getContext());
    IntegerType *int64Ty = Type::getInt64Ty(fn->getContext());

    // create the indirect branch
    Value *udef = llvm::UndefValue::get(int8PtTy);
    IndirectBrInst *iBr = iBuilder.CreateIndirectBr(udef, nBr);

    // list the BlockAddress from BasicBlock
    std::vector<Constant *> listBA;
    for (std::set<llvm::BasicBlock *>::iterator it = fit->second.begin();
         it != fit->second.end(); ++it) {
      iBr->addDestination(*it);
      BlockAddress *bba = BlockAddress::get(fn, *it);

      Constant *tag_id = ConstantInt::get(int64Ty, BBToHashIDMap[*it], false);
      Constant *tag = ConstantFolder().CreateIntToPtr(tag_id, int8PtTy);

      listBA.push_back(tag);
      listBA.push_back(bba);
    }
    ArrayRef<Constant *> blockArray(listBA);

    // create the constant type and array
    ArrayType *pArrTy = ArrayType::get(int8PtTy, listBA.size());
    Constant *blockItems = ConstantArray::get(pArrTy, blockArray);

    // Global Variable Declarations
    GlobalVariable *gvar_ptr_abc =
        new GlobalVariable(*fn->getParent(), blockItems->getType(), true,
                           GlobalValue::InternalLinkage, blockItems,
                           fn->getName().str() + "@labelTracker");
    gvar_ptr_abc->setAlignment(16);
    gvar_ptr_abc->setSection("cfg_label_data");
    gvar_ptr_abc->addAttribute(llvm::Attribute::OptimizeNone);
  }*/
}

// [OS-CFI] isTypeMatch(): return true if params/args and return types matched
// between an indirect call and a function signature
bool DDAPass::isTypeMatch(const Instruction *sink, const Value *source) {
  int nFnArg = 0, nCallArg = 0;
  vector<const Type *> fnArgList, callArgList;
  if (isa<Function>(source)) {
    const Function *fn = dyn_cast<Function>(source);
    const Type *rTy = fn->getReturnType();
    nFnArg = fn->arg_size();
    for (Function::const_arg_iterator AI = fn->arg_begin(), AE = fn->arg_end();
         AI != AE; ++AI) {
      const Value *arg = AI;
      const Type *argType = arg->getType();
      fnArgList.push_back(argType);
    }
    if (isa<CallInst>(sink)) {
      const CallInst *cBase = dyn_cast<CallInst>(sink);
      if (cBase->getFunctionType()->getReturnType() != rTy) {
        return false;
      }
      nCallArg = cBase->getNumArgOperands();
      for (int i = 0; i < cBase->getNumArgOperands(); i++) {
        const Value *arg = cBase->getArgOperand(i);
        const Type *argType = arg->getType();
        callArgList.push_back(argType);
      }
    } else if (isa<InvokeInst>(sink)) {
      const InvokeInst *cBase = dyn_cast<InvokeInst>(sink);
      if (cBase->getFunctionType()->getReturnType() != rTy) {
        return false;
      }
      nCallArg = cBase->getNumArgOperands();
      for (int i = 0; i < cBase->getNumArgOperands(); i++) {
        const Value *arg = cBase->getArgOperand(i);
        const Type *argType = arg->getType();
        callArgList.push_back(argType);
      }
    }

    if (nFnArg == nCallArg) {
      for (int i = 0; i < nFnArg; i++) {
        if (fnArgList[i] != callArgList[i]) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

// [OS-CFI] use address-taken type check entry for SUPA points-to set empty
// sinks
void DDAPass::fillEmptyPointsToSet(const Instruction *iCallInst) {
  string str;
  for (FuncSetIt it = setAddrFunc.begin(); it != setAddrFunc.end(); ++it) {
    Function *val = *it;
    if (isTypeMatch(iCallInst, val)) {
      atCFG *atItem = (atCFG *)malloc(sizeof(atCFG));
      atItem->type = UNDER_APPROXIMATE;
      atItem->iCallInst = iCallInst;
      atItem->iCallID = getHashID(iCallInst);
      atItem->iCallTarget = val;
      atItem->iCallTargetID = val->getValueID();
      atCFGList.push_back(atItem);
    }
  }
}

// [OS-CFI] computeCFG(): prepares the CFG listings from SUPA analysis
void DDAPass::computeCFG() {
  // get candidate queries
  const NodeSet &candidates = _client->getCandidateQueries();
  for (NodeSet::iterator cit = candidates.begin(), ceit = candidates.end();
       cit != ceit; ++cit) {
    // for each queries, extract candidate SVFG node and points-to set
    // information
    const SVFGNode *node = _pta->getSVFGForCandidateNode(*cit);
    const PointsTo &pts = _pta->getPts(*cit); // points-to set

    // [OS-CFI] initially the sink is not the iCall but immediate load
    // instruction
    llvm::Instruction *iCallInst = nullptr;
    if (isa<StmtSVFGNode>(node)) {
      const StmtSVFGNode *canStmt = dyn_cast<StmtSVFGNode>(node);
      iCallInst = (llvm::Instruction *)canStmt->getInst();
      int c = 0;
      while (1) {
        if (isa<CallInst>(iCallInst)) {
          if (c == 0) {
            c++;
          } else {
            break;
          }
        }
        if (iCallInst->isTerminator()) {
          iCallInst = nullptr;
          break;
        }
        iCallInst = iCallInst->getNextNonDebugInstruction();
      }
    }
    if (iCallInst == nullptr) {
      continue;
    }

    // insertLabelAfterCall(iCallInst);

    // list origin sensitive cfg
    const OriginSensitiveTupleSet *opts =
        _pta->getOriginSensitiveTupleSet(*cit);
    if (opts) {
      for (PointsTo::iterator pit = pts.begin(), peit = pts.end(); pit != peit;
           ++pit) {
        if (_pta->getValueFromNodeID(*pit) &&
            (isa<GlobalValue>(_pta->getValueFromNodeID(*pit)) ||
             isa<Function>(_pta->getValueFromNodeID(*pit)))) {
          for (OriginSensitiveTupleSetIt oit = opts->begin(),
                                         oeit = opts->end();
               oit != oeit; ++oit) {

            if (std::get<0>(*oit) == *pit) {
              unsigned long target =
                  std::get<0>(*oit); // origin sensitive tuple target
              Instruction *inst = (Instruction *)(std::get<1>(*oit))->getInst();
              Instruction *oInst = inst; // origin sensitive tuple store
                                         // instruction aka. origin
              unsigned long originID = -1;

              // replace origin store instruction to call to update mpx table
              // instruction
              while (1) {
                // update mpx from inside object creation
                oInst = (Instruction *)oInst->getNextNonDebugInstruction();
                if (isa<CallInst>(oInst)) {
                  CallInst *call = cast<CallInst>(oInst);
                  if (call->getCalledFunction() &&
                      call->getCalledFunction()->getName() ==
                          "update_mpx_table") {
                    if (isa<ConstantInt>(oInst->getOperand(2))) {
                      ConstantInt *cint =
                          dyn_cast<ConstantInt>(oInst->getOperand(2));
                      originID = cint->getZExtValue();
                    }
                    break;
                  }
                }
                if (oInst->isTerminator()) {
                  oInst = nullptr;
                  break;
                }
              }

              if (!oInst) {
                // update mpx from function pointer store
                llvm::Instruction *bInst = inst->getParent()->getFirstNonPHI();
                llvm::StoreInst *sinst = llvm::dyn_cast<llvm::StoreInst>(inst);

                while (1) {
                  if (bInst->getNextNonDebugInstruction() == inst &&
                      llvm::isa<llvm::CallInst>(bInst)) {
                    llvm::CallInst *call =
                        llvm::dyn_cast<llvm::CallInst>(bInst);
                    if (call->getNumArgOperands() >= 3 &&
                        call->getArgOperand(0) == sinst->getValueOperand()) {
                      oInst = call;
                      if (isa<ConstantInt>(oInst->getOperand(1))) {
                        ConstantInt *cint =
                            dyn_cast<ConstantInt>(oInst->getOperand(1));
                        originID = cint->getZExtValue();
                      }
                      break;
                    }
                  }
                  if (bInst->isTerminator()) {
                    break;
                  }
                  bInst = bInst->getNextNonDebugInstruction();
                }
              }

              oCFG *oItem = (oCFG *)malloc(sizeof(oCFG));
              oItem->iCallInst = iCallInst;
              oItem->iCallID = getHashID(iCallInst);
              oItem->iCallTarget = _pta->getValueFromNodeID(*pit);
              oItem->iCallTargetID = *pit;
              oItem->originID = originID;
              if (std::get<2>(*oit)) {
                oItem->originCTXInst = std::get<2>(*oit);
                oItem->originCTXID = getHashID(std::get<2>(*oit));
                insertLabelAfterCall(std::get<2>(*oit));
              } else {
                oItem->originCTXInst = nullptr;
              }
              oCFGList.push_back(oItem);
            }
          }
        }
      }
    }

    // list callsite sensitive cfg
    CallStackSet *cfilbRes = _pta->getCSSensitiveSet(*cit);
    /* for (PointsTo::iterator pit = pts.begin(), peit = pts.end(); pit != peit;
         ++pit) {
      if (_pta->isPointsTo(*pit)) {
        for (callStackSetDDA::iterator itt = cfilbRes->begin(),
                                       eitt = cfilbRes->end();
             itt != eitt; ++itt) {
          if (itt->first == *pit) {
            source = itt->first;
            callStackDDA tcStack(itt->second);

            if (!tcStack.empty()) {
              CFILB *cfilbItem = (CFILB *)malloc(sizeof(CFILB));

              cfilbItem->sink = iCallInst;
              cfilbItem->sinkID = getHashID(iCallInst);

              cfilbItem->source = _pta->getTargetValue(*pit);
              cfilbItem->sourceID = *pit;

              cfilbItem->callStack = new vector<const llvm::Instruction *>();
              cfilbItem->callStackID = new vector<unsigned long>();

              while (!tcStack.empty()) {
                if (tcStack.top().second) {
                  cfilbItem->callStack->push_back(tcStack.top().second);
                  cfilbItem->callStackID->push_back(getHashID(tcStack.top().second));

                  insertLabelAfterCall(tcStack.top().second);
                } else {
                  break;
                }
                tcStack.pop();
              }
              cfilbCFG.push_back(cfilbItem);
            }
          }
        }
      }
    } */

    // list CI-CFG using SUPA
    for (PointsTo::iterator pit = pts.begin(), peit = pts.end(); pit != peit;
         ++pit) {
      if (_pta->getValueFromNodeID(*pit) &&
          (isa<GlobalValue>(_pta->getValueFromNodeID(*pit)) ||
           isa<Function>(_pta->getValueFromNodeID(*pit)))) {
        supaCFG *sItem = (supaCFG *)malloc(sizeof(supaCFG));

        sItem->iCallInst = iCallInst;
        sItem->iCallTarget = _pta->getValueFromNodeID(*pit);
        sItem->iCallTargetID = *pit;

        supaCFGList.push_back(sItem);
        // if the points-to set is overapproximated, then type mismatch can
        // detect it
        if (isTypeMatch(iCallInst, _pta->getValueFromNodeID(*pit))) {
          atCFG *atItem = (atCFG *)malloc(sizeof(atCFG));
          atItem->type = OVER_APPROXIMATE;
          atItem->iCallInst = iCallInst;
          atItem->iCallID = getHashID(iCallInst);

          atItem->iCallTarget = _pta->getValueFromNodeID(*pit);
          atItem->iCallTargetID = *pit;
          atCFGList.push_back(atItem);
        }
      }
    }
    // if the points-to set is empty, address-taken type matched set will be
    // used
    if (pts.empty()) {
      fillEmptyPointsToSet(iCallInst);
    }
  }
}

// [OS-CFI] dumpSUPACFG(): print CI-CFG based on SUPA only analysis
void DDAPass::dumpSUPACFG() {
  std::hash<std::string> hash_fn;
  if (DUMP_CFG_DEBUG)
    outs() << "++++++++++++++[SUPA] Dump CI-CFG [SUPA]++++++++++++++\n";
  for (std::vector<supaCFG *>::iterator cit = supaCFGList.begin();
       cit != supaCFGList.end(); ++cit) {
    supaCFG *item = *cit;
    if (DUMP_CFG_DEBUG) {
      outs() << "iCall Instruction: " << *(item->iCallInst) << "\n";
      outs() << "iCall Target: " << (item->iCallTarget)->getName() << "\n";
      outs() << "\n";
    }
    errs() << SUPA_CFG << "\t" << item->iCallID << "\t" << item->iCallTargetID
           << "\n";
  }
}

// [OS-CFI] dumpatCFG(): print CI-CFG based on Address Taken and Type Check CFG
void DDAPass::dumpatCFG() {
  std::hash<std::string> hash_fn;
  if (DUMP_CFG_DEBUG)
    outs() << "++++++++++++++[OS-CFG] Address Taken and Type Checked CFG "
              "[OS-CFG]++++++++++++++\n";
  for (std::vector<atCFG *>::iterator cit = atCFGList.begin();
       cit != atCFGList.end(); ++cit) {
    atCFG *item = *cit;
    if (DUMP_CFG_DEBUG) {
      outs() << "[" << item->type
             << "] iCall Instruction: " << *(item->iCallInst) << "\n";
      outs() << "iCall Target: " << (item->iCallTarget)->getName() << "\n";
      outs() << "\n";
    }
    errs() << ATCFG << "\t" << item->iCallID << "\t" << item->iCallTargetID
           << "\n";
  }
}

// [OS-CFI] dumpoCFG(): print origin sensitive CFG
void DDAPass::dumpoCFG() {
  std::hash<std::string> hash_fn;
  if (DUMP_CFG_DEBUG)
    outs() << "++++++++++++++[OS-CFG] Origin Sensitive CFG "
              "[OS-CFG]++++++++++++++\n";
  for (std::vector<oCFG *>::iterator cit = oCFGList.begin();
       cit != oCFGList.end(); ++cit) {
    oCFG *item = *cit;
    if (DUMP_CFG_DEBUG) {
      outs() << "iCall Instruction: " << *(item->iCallInst) << "\n";
      outs() << "iCall Target: " << (item->iCallTarget)->getName() << "\n";
      outs() << "Origin ID: " << item->originID << "\n";
      if (item->originCTXInst) {
        outs() << "Origin CS Instruction: " << *(item->originCTXInst) << "\n";
      }
      outs() << "\n";
    }
    errs() << OCFG << "\t" << item->iCallID << "\t" << item->iCallTargetID
           << "\t" << item->originID;
    if (item->originCTXInst) {
      errs() << ":" << item->originCTXID;
    }
    errs() << "\n";
  }
}

// [OS-CFI] dumpcCFG(): print callsite sensitive CFG
void DDAPass::dumpcCFG() {
  std::hash<std::string> hash_fn;
  if (DUMP_CFG_DEBUG)
    outs() << "++++++++++++++[OS-CFG] Callsite Sensitive CFG "
              "[OS-CFG]++++++++++++++\n";
  for (std::vector<cCFG *>::iterator cit = cCFGList.begin();
       cit != cCFGList.end(); ++cit) {
    cCFG *item = *cit;
    if (DUMP_CFG_DEBUG) {
      outs() << "iCall Instruction: " << *(item->iCallInst) << "\n";
      outs() << "iCall Target: " << (item->iCallTarget)->getName() << "\n";
      outs() << "iCall CSites:\n";
      for (std::vector<const llvm::Instruction *>::iterator ctxit =
               item->cInstStack->begin();
           ctxit != item->cInstStack->end(); ++ctxit) {
        llvm::outs() << **ctxit << "\n";
      }
      llvm::outs() << "\n";
    }

    errs() << CCFG << "\t" << item->iCallID << "\t" << item->iCallTargetID;
    for (std::vector<unsigned long>::iterator ctxit = item->cIDStack->begin();
         ctxit != item->cIDStack->end(); ++ctxit) {
      errs() << "\t" << *ctxit;
    }
    errs() << "\n";
  }
}