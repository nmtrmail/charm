/**
 * \addtogroup CkLdb
 */
/*@{*/

#include "converse.h"
#include <charm++.h>
#include "cksyncbarrier.h"

#include "DistributedLB.h"
#include "LBManager.h"
#include "LBSimulation.h"
#include "TreeLB.h"
#include "topology.h"

#include "json.hpp"

CkGroupID _lbmgr;

CkpvDeclare(LBUserDataLayout, lbobjdatalayout);
CkpvDeclare(int, _lb_obj_index);

CkpvDeclare(bool, lbmanagerInited); /**< true if lbdatabase is inited */

// command line options
CkLBArgs _lb_args;
bool _lb_predict = false;
int _lb_predict_delay = 10;
int _lb_predict_window = 20;
bool _lb_psizer_on = false;

// registry class stores all load balancers linked and created at runtime
class LBDBRegistry
{
  friend class LBMgrInit;
  friend class LBManager;

 private:
  // table for all available LBs linked in
  struct LBDBEntry
  {
    const char* name;
    LBCreateFn cfn;
    LBAllocFn afn;
    const char* help;
    int shown;  // if 0, donot show in help page
    LBDBEntry() : name(0), cfn(0), afn(0), help(0), shown(1) {}
    LBDBEntry(int) {}
    LBDBEntry(const char* n, LBCreateFn cf, LBAllocFn af, const char* h, int show = 1)
        : name(n), cfn(cf), afn(af), help(h), shown(show){};
  };
  CkVec<LBDBEntry> lbtables;       // a list of available LBs linked
  CkVec<const char*> compile_lbs;  // load balancers at compile time
  CkVec<const char*> runtime_lbs;  // load balancers at run time
  // map of {index in runtime_lbs, name of legacy LB to instantiate TreeLB with}
  // for use with the legacy LBs (e.g. GreedyLB -> the predefined Greedy version of TreeLB)
  std::unordered_map<int, const char*> legacy_runtime_treelbs;
 public:
  LBDBRegistry() {}
  void displayLBs()
  {
    CmiPrintf("\nAvailable load balancers:\n");
    for (int i = 0; i < lbtables.length(); i++)
    {
      LBDBEntry& entry = lbtables[i];
      if (entry.shown) CmiPrintf("* %s:	%s\n", entry.name, entry.help);
    }
    CmiPrintf("\n");
  }
  void addEntry(const char* name, LBCreateFn fn, LBAllocFn afn, const char* help,
                int shown)
  {
    lbtables.push_back(LBDBEntry(name, fn, afn, help, shown));
  }
  void addCompiletimeBalancer(const char* name) { compile_lbs.push_back(name); }
  void addRuntimeBalancer(const char* name, const char* legacyLBName = nullptr)
  {
    if (legacyLBName != nullptr)
    {
      legacy_runtime_treelbs.emplace(runtime_lbs.size(), legacyLBName);
    }

    runtime_lbs.push_back(name);
  }
  LBCreateFn search(const char* name)
  {
    char* ptr = strpbrk((char*)name, ":,");
    int slen = ptr != NULL ? ptr - name : strlen(name);
    for (int i = 0; i < lbtables.length(); i++)
      if (0 == strncmp(name, lbtables[i].name, slen)) return lbtables[i].cfn;
    return NULL;
  }
  LBAllocFn getLBAllocFn(const char* name)
  {
    char* ptr = strpbrk((char*)name, ":,");
    int slen = ptr - name;
    for (int i = 0; i < lbtables.length(); i++)
      if (0 == strncmp(name, lbtables[i].name, slen)) return lbtables[i].afn;
    return NULL;
  }
};

static LBDBRegistry lbRegistry;
static std::vector<std::string> lbNames;

void LBDefaultCreate(const char* lbname) { lbRegistry.addCompiletimeBalancer(lbname); }

// default is to show the helper
void LBRegisterBalancer(const char* name, LBCreateFn fn, LBAllocFn afn, const char* help,
                        int shown)
{
  lbRegistry.addEntry(name, fn, afn, help, shown);
}

LBAllocFn getLBAllocFn(const char* lbname) { return lbRegistry.getLBAllocFn(lbname); }

// create a load balancer group using the strategy name
static void createLoadBalancer(const char* lbname, const char* legacybalancer = nullptr)
{
  LBCreateFn fn = lbRegistry.search(lbname);
  if (!fn)
  {  // invalid lb name
    CmiPrintf("Abort: Unknown load balancer: '%s'!\n", lbname);
    lbRegistry.displayLBs();  // display help page
    CkAbort("Abort");
  }
  // invoke function to create load balancer
  int seqno = LBManagerObj()->getLoadbalancerTicket();
  fn(CkLBOptions(seqno, legacybalancer));
}

// mainchare
LBMgrInit::LBMgrInit(CkArgMsg* m)
{
#if CMK_LBDB_ON
  _lbmgr = CProxy_LBManager::ckNew();

  // runtime specified load balancer
  if (lbRegistry.runtime_lbs.size() > 0)
  {
    for (int i = 0; i < lbRegistry.runtime_lbs.size(); i++)
    {
      const char* balancer = lbRegistry.runtime_lbs[i];
      // If this is a legacy TreeLB, pass in the legacy LB name
      const char* legacybalancer = lbRegistry.legacy_runtime_treelbs.count(i) > 0
                                       ? lbRegistry.legacy_runtime_treelbs[i]
                                       : nullptr;
      createLoadBalancer(balancer, legacybalancer);
    }
  }
  else if (lbRegistry.compile_lbs.size() > 0)
  {
    for (int i = 0; i < lbRegistry.compile_lbs.size(); i++)
    {
      const char* balancer = lbRegistry.compile_lbs[i];
      createLoadBalancer(balancer);
    }
  }

  // simulation mode
  if (LBSimulation::doSimulation)
  {
    CmiPrintf("Charm++> Entering Load Balancer Simulation Mode ... \n");
    CProxy_LBManager(_lbmgr).ckLocalBranch()->StartLB();
  }
#endif
  delete m;
}

// called from init.C
void _loadbalancerInit()
{
  CkpvInitialize(bool, lbmanagerInited);
  CkpvAccess(lbmanagerInited) = false;

  CkpvInitialize(LBUserDataLayout, lbobjdatalayout);
  CkpvInitialize(int, _lb_obj_index);
  CkpvAccess(_lb_obj_index) = -1;

  char** argv = CkGetArgv();
  char* balancer = NULL;
  CmiArgGroup("Charm++", "Load Balancer");

  CmiGetArgStringDesc(argv, "+TreeLBFile", &_lb_args.treeLBFile(), "TreeLB config file");

  // turn on MetaBalancer if set
  _lb_args.metaLbOn() = CmiGetArgFlagDesc(argv, "+MetaLB", "Turn on MetaBalancer");
  CmiGetArgStringDesc(argv, "+MetaLBModelDir", &_lb_args.metaLbModelDir(),
                      "Use this directory to read model for MetaLB");

  if (_lb_args.metaLbOn() && _lb_args.metaLbModelDir() != nullptr)
  {
#if CMK_USE_ZLIB
    if (CkMyRank() == 0)
    {
      lbRegistry.addRuntimeBalancer("TreeLB");
      lbNames.push_back("Greedy");
      lbNames.push_back("GreedyRefine");
      lbNames.push_back("DistributedLB");
      lbNames.push_back("Refine");
      lbNames.push_back("Hybrid");
      lbNames.push_back("MetisLB");
      if (CkMyPe() == 0)
      {
        if (CmiGetArgStringDesc(argv, "+balancer", &balancer, "Use this load balancer"))
          CkPrintf(
              "Warning: Ignoring the +balancer option, since Meta-Balancer's model-based "
              "load balancer selection is enabled.\n");
        CkPrintf(
            "Warning: Automatic strategy selection in MetaLB is activated. This is an "
            "experimental feature.\n");
      }
      while (CmiGetArgStringDesc(argv, "+balancer", &balancer, "Use this load balancer"))
        ;
    }
#else
    if (CkMyPe() == 0)
      CkAbort(
          "MetaLB random forest model not supported because Charm++ was built without "
          "zlib support.\n");
#endif
  }
  else
  {
    if (CkMyPe() == 0 && _lb_args.metaLbOn())
      CkPrintf(
          "Warning: MetaLB is activated. For Automatic strategy selection in MetaLB, "
          "pass directory of model files using +MetaLBModelDir.\n");
    if (CkMyRank() == 0)
    {
      while (CmiGetArgStringDesc(argv, "+balancer", &balancer, "Use this load balancer"))
      {
        bool isLegacyTreeLB = true;
        const char* legacyBalancer;
        if (strcmp(balancer, "GreedyLB") == 0)
          legacyBalancer = "Greedy";
        else if (strcmp(balancer, "GreedyRefineLB") == 0)
          legacyBalancer = "GreedyRefine";
        else if (strcmp(balancer, "RefineLB") == 0)
          legacyBalancer = "RefineA";
        else if (strcmp(balancer, "RandCentLB") == 0)
          legacyBalancer = "Random";
        else if (strcmp(balancer, "DummyLB") == 0)
          legacyBalancer = "Dummy";
        else if (strcmp(balancer, "RotateLB") == 0)
          legacyBalancer = "Rotate";
        else
        {
          lbRegistry.addRuntimeBalancer(balancer); /* lbRegistry is a static */
          isLegacyTreeLB = false;
        }

        if (isLegacyTreeLB)
        {
          lbRegistry.addRuntimeBalancer("TreeLB", legacyBalancer);
        }
      }
    }
    else
    {
      // For other ranks, consume the +balancer arguments to avoid spuriously
      // passing them to the application
      while (CmiGetArgStringDesc(argv, "+balancer", &balancer, "Use this load balancer"))
        ;
    }
  }

  CmiGetArgDoubleDesc(
      argv, "+DistLBTargetRatio", &_lb_args.targetRatio(),
      "The max/avg load ratio that DistributedLB will attempt to achieve");
  CmiGetArgIntDesc(argv, "+DistLBMaxPhases", &_lb_args.maxDistPhases(),
                   "The maximum number of phases that DistributedLB will attempt");

  // set up init value for LBPeriod time in seconds
  // it can also be set by calling LDSetLBPeriod()
  CmiGetArgDoubleDesc(argv, "+LBPeriod", &_lb_args.lbperiod(),
                      "the minimum time period in seconds allowed for two consecutive "
                      "automatic load balancing");
  _lb_args.loop() = CmiGetArgFlagDesc(argv, "+LBLoop",
                                      "Use multiple load balancing strategies in loop");

  // now called in cldb.C: CldModuleGeneralInit()
  // registerLBTopos();
  CmiGetArgStringDesc(argv, "+LBTopo", &_lbtopo, "define load balancing topology");

  /**************** FUTURE PREDICTOR ****************/
  _lb_predict = CmiGetArgFlagDesc(argv, "+LBPredictor", "Turn on LB future predictor");
  CmiGetArgIntDesc(argv, "+LBPredictorDelay", &_lb_predict_delay,
                   "Number of balance steps before learning a model");
  CmiGetArgIntDesc(argv, "+LBPredictorWindow", &_lb_predict_window,
                   "Number of steps to use to learn a model");
  if (_lb_predict_window < _lb_predict_delay)
  {
    CmiPrintf(
        "LB> [%d] Argument LBPredictorWindow (%d) less than LBPredictorDelay (%d) , "
        "fixing\n",
        CkMyPe(), _lb_predict_window, _lb_predict_delay);
    _lb_predict_delay = _lb_predict_window;
  }

  /******************* SIMULATION *******************/
  // get the step number at which to dump the LB database
  CmiGetArgIntDesc(argv, "+LBVersion", &_lb_args.lbversion(),
                   "LB database file version number");
  CmiGetArgIntDesc(argv, "+LBCentPE", &_lb_args.central_pe(), "CentralLB processor");
  bool _lb_dump_activated = false;
  if (CmiGetArgIntDesc(argv, "+LBDump", &LBSimulation::dumpStep,
                       "Dump the LB state from this step"))
    _lb_dump_activated = true;
  if (_lb_dump_activated && LBSimulation::dumpStep < 0)
  {
    CmiPrintf("LB> Argument LBDump (%d) negative, setting to 0\n",
              LBSimulation::dumpStep);
    LBSimulation::dumpStep = 0;
  }
  CmiGetArgIntDesc(argv, "+LBDumpSteps", &LBSimulation::dumpStepSize,
                   "Dump the LB state for this amount of steps");
  if (LBSimulation::dumpStepSize <= 0)
  {
    CmiPrintf("LB> Argument LBDumpSteps (%d) too small, setting to 1\n",
              LBSimulation::dumpStepSize);
    LBSimulation::dumpStepSize = 1;
  }
  CmiGetArgStringDesc(argv, "+LBDumpFile", &LBSimulation::dumpFile,
                      "Set the LB state file name");
  // get the simulation flag and number. Now the flag can also be avoided by the presence
  // of the number
  LBSimulation::doSimulation =
      CmiGetArgIntDesc(argv, "+LBSim", &LBSimulation::simStep,
                       "Read LB state from LBDumpFile since this step");
  // check for stupid LBSim parameter
  if (LBSimulation::doSimulation && LBSimulation::simStep < 0)
  {
    CkAbort("LB> Argument LBSim (%d) invalid, should be >= 0\n", LBSimulation::simStep);
    return;
  }
  CmiGetArgIntDesc(argv, "+LBSimSteps", &LBSimulation::simStepSize,
                   "Read LB state for this number of steps");
  if (LBSimulation::simStepSize <= 0)
  {
    CmiPrintf("LB> Argument LBSimSteps (%d) too small, setting to 1\n",
              LBSimulation::simStepSize);
    LBSimulation::simStepSize = 1;
  }

  LBSimulation::simProcs = 0;
  CmiGetArgIntDesc(argv, "+LBSimProcs", &LBSimulation::simProcs,
                   "Number of target processors.");

  LBSimulation::showDecisionsOnly =
      CmiGetArgFlagDesc(argv, "+LBShowDecisions",
                        "Write to File: Load Balancing Object to Processor Map decisions "
                        "during LB Simulation");

  // force a global barrier after migration done
  _lb_args.syncResume() = CmiGetArgFlagDesc(
      argv, "+LBSyncResume", "LB performs a barrier after migration is finished");

  // both +LBDebug and +LBDebug level should work
  if (!CmiGetArgIntDesc(argv, "+LBDebug", &_lb_args.debug(),
                        "Turn on LB debugging printouts"))
    _lb_args.debug() =
        CmiGetArgFlagDesc(argv, "+LBDebug", "Turn on LB debugging printouts");

  // ask to print summary/quality of load balancer
  _lb_args.printSummary() =
      CmiGetArgFlagDesc(argv, "+LBPrintSummary", "Print load balancing result summary");

  // to ignore baclground load
  _lb_args.ignoreBgLoad() = CmiGetArgFlagDesc(
      argv, "+LBNoBackground", "Load balancer ignores the background load.");
  _lb_args.migObjOnly() = CmiGetArgFlagDesc(
      argv, "+LBObjOnly", "Only load balancing migratable objects, ignoring all others.");
  if (_lb_args.migObjOnly()) _lb_args.ignoreBgLoad() = true;

  // assume all CPUs are identical
  _lb_args.testPeSpeed() =
      CmiGetArgFlagDesc(argv, "+LBTestPESpeed", "Load balancer test all CPUs speed.");
  _lb_args.samePeSpeed() = CmiGetArgFlagDesc(
      argv, "+LBSameCpus", "Load balancer assumes all CPUs are of same speed.");
  if (!_lb_args.testPeSpeed()) _lb_args.samePeSpeed() = true;

  _lb_args.useCpuTime() = CmiGetArgFlagDesc(
      argv, "+LBUseCpuTime", "Load balancer uses CPU time instead of wallclock time.");

  // turn instrumentation off at startup
  _lb_args.statsOn() =
      !CmiGetArgFlagDesc(argv, "+LBOff", "Turn load balancer instrumentation off");

  // turn instrumentation of communicatin off at startup
  _lb_args.traceComm() = !CmiGetArgFlagDesc(
      argv, "+LBCommOff", "Turn load balancer instrumentation of communication off");

  // set alpha and beta
  _lb_args.alpha() = PER_MESSAGE_SEND_OVERHEAD_DEFAULT;
  _lb_args.beta() = PER_BYTE_SEND_OVERHEAD_DEFAULT;
  CmiGetArgDoubleDesc(argv, "+LBAlpha", &_lb_args.alpha(), "per message send overhead");
  CmiGetArgDoubleDesc(argv, "+LBBeta", &_lb_args.beta(), "per byte send overhead");

  if (CkMyPe() == 0)
  {
    if (_lb_args.debug())
    {
      CmiPrintf("CharmLB> Verbose level %d, load balancing period: %g seconds\n",
                _lb_args.debug(), _lb_args.lbperiod());
    }
    if (_lb_args.debug() > 1)
    {
      CmiPrintf("CharmLB> Topology %s alpha: %es beta: %es.\n", _lbtopo, _lb_args.alpha(),
                _lb_args.beta());
    }
    if (_lb_args.printSummary())
      CmiPrintf("CharmLB> Load balancer print summary of load balancing result.\n");
    if (_lb_args.ignoreBgLoad())
      CmiPrintf("CharmLB> Load balancer ignores processor background load.\n");
    if (_lb_args.samePeSpeed())
      CmiPrintf("CharmLB> Load balancer assumes all CPUs are same.\n");
    if (_lb_args.useCpuTime())
      CmiPrintf("CharmLB> Load balancer uses CPU time instead of wallclock time.\n");
    if (LBSimulation::doSimulation)
      CmiPrintf(
          "CharmLB> Load balancer running in simulation mode on file '%s' version %d.\n",
          LBSimulation::dumpFile, _lb_args.lbversion());
    if (_lb_args.statsOn() == 0)
      CkPrintf("CharmLB> Load balancing instrumentation is off.\n");
    if (_lb_args.traceComm() == 0)
      CkPrintf("CharmLB> Load balancing instrumentation for communication is off.\n");
    if (_lb_args.migObjOnly())
      CkPrintf("LB> Load balancing strategy ignores non-migratable objects.\n");
  }
}

bool LBManager::manualOn = false;
char* LBManager::avail_vector = NULL;
bool LBManager::avail_vector_set = false;
CmiNodeLock avail_vector_lock;

static LBRealType* _expectedLoad = NULL;

void LBManager::initnodeFn()
{
  int proc;
  int num_proc = CkNumPes();
  avail_vector = new char[num_proc];
  for (proc = 0; proc < num_proc; proc++) avail_vector[proc] = 1;
  avail_vector_lock = CmiCreateLock();

  _expectedLoad = new LBRealType[num_proc];
  for (proc = 0; proc < num_proc; proc++) _expectedLoad[proc] = 0.0;

  _registerCommandLineOpt("+balancer");
  _registerCommandLineOpt("+LBPeriod");
  _registerCommandLineOpt("+TreeLBFile");
  _registerCommandLineOpt("+LBLoop");
  _registerCommandLineOpt("+LBTopo");
  _registerCommandLineOpt("+LBPredictor");
  _registerCommandLineOpt("+LBPredictorDelay");
  _registerCommandLineOpt("+LBPredictorWindow");
  _registerCommandLineOpt("+LBVersion");
  _registerCommandLineOpt("+LBCentPE");
  _registerCommandLineOpt("+LBDump");
  _registerCommandLineOpt("+LBDumpSteps");
  _registerCommandLineOpt("+LBDumpFile");
  _registerCommandLineOpt("+LBSim");
  _registerCommandLineOpt("+LBSimSteps");
  _registerCommandLineOpt("+LBSimProcs");
  _registerCommandLineOpt("+LBShowDecisions");
  _registerCommandLineOpt("+LBSyncResume");
  _registerCommandLineOpt("+LBDebug");
  _registerCommandLineOpt("+LBPrintSummary");
  _registerCommandLineOpt("+LBNoBackground");
  _registerCommandLineOpt("+LBObjOnly");
  _registerCommandLineOpt("+LBTestPESpeed");
  _registerCommandLineOpt("+LBSameCpus");
  _registerCommandLineOpt("+LBUseCpuTime");
  _registerCommandLineOpt("+LBOff");
  _registerCommandLineOpt("+LBCommOff");
  _registerCommandLineOpt("+MetaLB");
  _registerCommandLineOpt("+LBAlpha");
  _registerCommandLineOpt("+LBBeta");
}

void LBManager::InvokeLB()
{
  if (loadbalancers.size() > 0)
  {
    loadbalancers[currentLBIndex]->InvokeLB();
  }
}

// Called at end of each load balancing cycle
void LBManager::periodicLB(void* in) { ((LBManager*)in)->InvokeLB(); }

void LBManager::setTimer()
{
  CcdCallFnAfterOnPE((CcdVoidFn)periodicLB, (void*)this, 1000 * _lb_args.lbperiod(),
                     CkMyPe());
}

// called my constructor
void LBManager::init(void)
{
  mystep = 0;
  new_ld_balancer = 0;
  chare_count = 0;
  metabalancer = nullptr;
  lbdb_obj = new LBDatabase();
  currentLBIndex = 0;
#if CMK_LB_CPUTIMER
  obj_cputime = 0;
#endif
  useBarrier = true;
  predictCBFn = nullptr;
  startLBFn_count = 0;

  CkpvAccess(lbmanagerInited) = true;
#if CMK_LBDB_ON
  if (manualOn) TurnManualLBOn();
#endif

  if (_lb_args.lbperiod() != -1.0)  // check if user set LBPeriod - fix later
  {
    setTimer();
  }
  else
  {
    AddLocalBarrierReceiver([this](void) { this->InvokeLB(); });
  }
}

int LBManager::AddStartLBFn(std::function<void()> fn)
{
  // Save startLB function
  StartLBCB* callbk = new StartLBCB;

  callbk->fn = fn;
  callbk->on = true;
  startLBFnList.push_back(callbk);
  startLBFn_count++;
  return startLBFnList.size() - 1;
}

void LBManager::RemoveStartLBFn(int handle)
{
  StartLBCB* callbk = startLBFnList[handle];
  if (callbk)
  {
    delete callbk;
    startLBFnList[handle] = nullptr;
    startLBFn_count--;
  }
}

void LBManager::StartLB()
{
  if (startLBFn_count == 0)
  {
    CmiAbort("StartLB is not supported in this LB");
  }
  for (int i = 0; i < startLBFnList.size(); i++)
  {
    StartLBCB* startLBFn = startLBFnList[i];
    if (startLBFn && startLBFn->on) startLBFn->fn();
  }
}

int LBManager::AddMigrationDoneFn(std::function<void()> fn)
{
  // Save migrationDone callback function
  MigrationDoneCB* callbk = new MigrationDoneCB;

  callbk->fn = fn;
  migrationDoneCBList.push_back(callbk);
  return migrationDoneCBList.size() - 1;
}

void LBManager::RemoveMigrationDoneFn(int handle)
{
  MigrationDoneCB* callbk = migrationDoneCBList[handle];
  if (callbk)
  {
    delete callbk;
    migrationDoneCBList[handle] = nullptr;
  }
}

void LBManager::MigrationDone()
{
  for (int i = 0; i < migrationDoneCBList.size(); i++)
  {
    MigrationDoneCB* callbk = migrationDoneCBList[i];
    if (callbk) callbk->fn();
  }
}

void LBManager::DumpDatabase()
{
#ifdef DEBUG
  CmiPrintf("Database contains %d object managers\n", omCount);
  CmiPrintf("Database contains %d objects\n", objs.size());
#endif
}

void LBManager::Migrated(LDObjHandle h, int waitBarrier)
{
  // Object migrated, inform load balancers
  if (loadbalancers.size() > 0) loadbalancers[currentLBIndex]->Migrated(waitBarrier);
}

LBManager::LastLBInfo::LastLBInfo() { expectedLoad = _expectedLoad; }

void LBManager::get_avail_vector(char* bitmap)
{
  CmiAssert(bitmap && avail_vector);
  const int num_proc = CkNumPes();
  for (int proc = 0; proc < num_proc; proc++)
  {
    bitmap[proc] = avail_vector[proc];
  }
}

// new_ld == -1(default) : calcualte a new ld
//           -2 : ignore new ld
//           >=0: given a new ld
void LBManager::set_avail_vector(char* bitmap, int new_ld)
{
  int assigned = 0;
  const int num_proc = CkNumPes();
  if (new_ld == -2)
    assigned = 1;
  else if (new_ld >= 0)
  {
    CmiAssert(new_ld < num_proc);
    new_ld_balancer = new_ld;
    assigned = 1;
  }
  CmiAssert(bitmap && avail_vector);
  for (int count = 0; count < num_proc; count++)
  {
    avail_vector[count] = bitmap[count];
    if ((bitmap[count] == 1) && !assigned)
    {
      new_ld_balancer = count;
      assigned = 1;
    }
  }
}

// called in CreateFooLB() when multiple load balancers are created
// on PE0, BaseLB of each load balancer applies a ticket number
// and broadcast the ticket number to all processors
int LBManager::getLoadbalancerTicket()
{
  loadbalancers.push_back(nullptr);
  return loadbalancers.size() - 1;
}

void LBManager::addLoadbalancer(BaseLB* lb, int seq)
{
  //  CmiPrintf("[%d] addLoadbalancer for seq %d\n", CkMyPe(), seq);
  if (seq == -1) return;
  if (CkMyPe() == 0)
  {
    CmiAssert(seq < loadbalancers.size());
    if (loadbalancers[seq])
    {
      CmiPrintf("Duplicate load balancer created at %d\n", seq);
      CmiAbort("LBManager");
    }
  }
  if (loadbalancers.size() < seq + 1)
    loadbalancers.resize(seq + 1);
  loadbalancers[seq] = lb;
}

// switch strategy in order
void LBManager::nextLoadbalancer(int seq)
{
  if (seq == -1) return;  // -1 means this is the only LB
  currentLBIndex = seq + 1;
  if (_lb_args.loop())
  {
    if (currentLBIndex == loadbalancers.size()) currentLBIndex = 0;
  }
  else
  {
    if (currentLBIndex == loadbalancers.size()) currentLBIndex--;  // keep using the last one
  }
  if (seq != currentLBIndex)
  {
    loadbalancers[seq]->turnOff();
    CmiAssert(loadbalancers[currentLBIndex]);
    loadbalancers[currentLBIndex]->turnOn();
  }
}

// switch strategy
void LBManager::switchLoadbalancer(int switchFrom, int switchTo)
{
  if (lbNames[switchTo] != "DistributedLB" && lbNames[switchTo] != "MetisLB")
  {
    json config;
    if (lbNames[switchTo] == "Hybrid")
    {
      config["tree"] = "PE_Process_Root";
      config["Root"]["pe"] = 0;
      config["Root"]["step_freq"] = 3;
      config["Root"]["strategies"] = {"GreedyRefine"};
      config["Process"]["strategies"] = {"GreedyRefine"};
    }
    else
    {
      config["tree"] = "PE_Root";
      config["Root"]["pe"] = 0;
      config["Root"]["strategies"] = {lbNames[switchTo]};
    }
    configureTreeLB(config);
  }
  else
  {
    // TODO: Implement turn off / on for Distributed
  }
}

// return the seq-th load balancer string name of
// it can be specified in either compile time or runtime
// runtime has higher priority
const char* LBManager::loadbalancer(int seq)
{
  if (lbRegistry.runtime_lbs.length())
  {
    CmiAssert(seq < lbRegistry.runtime_lbs.length());
    return lbRegistry.runtime_lbs[seq];
  }
  else
  {
    CmiAssert(seq < lbRegistry.compile_lbs.length());
    return lbRegistry.compile_lbs[seq];
  }
}

void LBManager::pup(PUP::er& p)
{
  IrrGroup::pup(p);
  // the memory should be already allocated
  int np;
  if (!p.isUnpacking()) np = CkNumPes();
  p | np;
  // in case number of processors changes
  if (p.isUnpacking())
  {
    CmiLock(avail_vector_lock);
    if (!avail_vector_set)
    {
      avail_vector_set = true;
      CmiAssert(avail_vector);
      if (np > CkNumPes())
      {
        delete[] avail_vector;
        avail_vector = new char[np];
        for (int i = 0; i < np; i++) avail_vector[i] = 1;
      }
      p(avail_vector, np);
    }
    else
    {
      char* tmp_avail_vector = new char[np];
      p(tmp_avail_vector, np);
      delete[] tmp_avail_vector;
    }
    CmiUnlock(avail_vector_lock);
  }
  else
  {
    CmiAssert(avail_vector);
    p(avail_vector, np);
  }
  p | mystep;
  if (p.isUnpacking())
  {
    if (_lb_args.metaLbOn())
    {
      // if unpacking set metabalancer using the id
      metabalancer = (MetaBalancer*)CkLocalBranch(_metalb);
    }
  }
}

void configureTreeLB(const char* json_str)
{
  ((LBManager*)CkLocalBranch(_lbmgr))->configureTreeLB(json_str);
}

void LBManager::configureTreeLB(const char* json_str)
{
  json config = json::parse(json_str);
  configureTreeLB(config);
}

void LBManager::configureTreeLB(json& config)
{
  bool found = false;
  for (int i = 0; i < loadbalancers.size(); i++)
  {
    if (strcmp(loadbalancers[i]->lbName(), "TreeLB") == 0)
    {
      ((TreeLB*)loadbalancers[i])->configure(config);
      found = true;
      // break; // not sure if there could be more than one TreeLB
    }
  }
  if (!found) CkAbort("LBManager: TreeLB is not in my list of load balancers");
}

void LBManager::ResetAdaptive()
{
#if CMK_LBDB_ON
  if (_lb_args.metaLbOn())
  {
    if (metabalancer == NULL)
    {
      metabalancer = CProxy_MetaBalancer(_metalb).ckLocalBranch();
    }
    if (metabalancer != NULL)
    {
      metabalancer->ResetAdaptive();
    }
  }
#endif
}

void LBManager::ResumeClients()
{
#if CMK_LBDB_ON
  if (_lb_args.metaLbOn())
  {
    if (metabalancer == NULL)
    {
      metabalancer = CProxy_MetaBalancer(_metalb).ckLocalBranch();
    }
    if (metabalancer != NULL)
    {
      metabalancer->ResumeClients();
    }
  }

  // If periodic is enabled, reset the timer and don't resume clients
  if (_lb_args.lbperiod() != -1.0)
  {
    setTimer();
  }
  else
  {
    CkSyncBarrier::Object()->ResumeClients();
  }
#endif
}

void LBManager::SetMigrationCost(double cost)
{
#if CMK_LBDB_ON
  if (_lb_args.metaLbOn())
  {
    if (metabalancer == NULL)
    {
      metabalancer = (MetaBalancer*)CkLocalBranch(_metalb);
    }
    if (metabalancer != NULL)
    {
      metabalancer->SetMigrationCost(cost);
    }
  }
#endif
}

void LBManager::SetStrategyCost(double cost)
{
#if CMK_LBDB_ON
  if (_lb_args.metaLbOn())
  {
    if (metabalancer == NULL)
    {
      metabalancer = (MetaBalancer*)CkLocalBranch(_metalb);
    }
    if (metabalancer != NULL)
    {
      metabalancer->SetStrategyCost(cost);
    }
  }
#endif
}

void LBManager::UpdateDataAfterLB(double mLoad, double mCpuLoad, double avgLoad)
{
#if CMK_LBDB_ON
  if (_lb_args.metaLbOn())
  {
    if (metabalancer == NULL)
    {
      metabalancer = (MetaBalancer*)CkLocalBranch(_metalb);
    }
    if (metabalancer != NULL)
    {
      metabalancer->UpdateAfterLBData(mLoad, mCpuLoad, avgLoad);
    }
  }
#endif
}

LDBarrierClient LBManager::AddLocalBarrierClient(Chare* obj, std::function<void()> fn)
{
  return CkSyncBarrier::Object()->AddClient(obj, fn);
}

void LBManager::RemoveLocalBarrierClient(LDBarrierClient h)
{
  CkSyncBarrier::Object()->RemoveClient(h);
}

LDBarrierReceiver LBManager::AddLocalBarrierReceiver(std::function<void()> fn)
{
  return CkSyncBarrier::Object()->AddReceiver(fn);
}

void LBManager::RemoveLocalBarrierReceiver(LDBarrierReceiver h)
{
  CkSyncBarrier::Object()->RemoveReceiver(h);
}

void LBManager::AtLocalBarrier(LDBarrierClient _n_c)
{
  if (useBarrier) CkSyncBarrier::Object()->AtBarrier(_n_c);
}

void LBManager::DecreaseLocalBarrier(int c)
{
  if (useBarrier) CkSyncBarrier::Object()->DecreaseBarrier(c);
}

void LBManager::TurnOnBarrierReceiver(LDBarrierReceiver h)
{
  CkSyncBarrier::Object()->TurnOnReceiver(h);
}

void LBManager::TurnOffBarrierReceiver(LDBarrierReceiver h)
{
  CkSyncBarrier::Object()->TurnOffReceiver(h);
}

void LBManager::LocalBarrierOn(void) { CkSyncBarrier::Object()->TurnOn(); };
void LBManager::LocalBarrierOff(void) { CkSyncBarrier::Object()->TurnOff(); };

#if CMK_LBDB_ON
static void work(int iter_block, volatile int* result)
{
  int i;
  *result = 1;
  for (i = 0; i < iter_block; i++)
  {
    double b = 0.1 + 0.1 * *result;
    *result = (int)(sqrt(1 + cos(b * 1.57)));
  }
}

int LDProcessorSpeed()
{
  if (_lb_args.samePeSpeed() ||
      CkNumPes() == 1)  // I think it is safe to assume that we can
    return 1;           // skip this if we are only using 1 PE

  volatile int result = 0;

  int wps = 0;
  const double elapse = 0.2;
  // First, count how many iterations happen in "elapse" seconds.
  // Since we are doing lots of function calls, this will be rough
  const double end_time = CmiCpuTimer() + elapse;
  while (CmiCpuTimer() < end_time)
  {
    work(1000, &result);
    wps += 1000;
  }

  // Now we have a rough idea of how many iterations happen in
  // "elapse" seconds, so just perform a few cycles of correction
  // by running for what should take that long. Then correct the
  // number of iterations if needed.

  for (int i = 0; i < 2; i++)
  {
    const double start_time = CmiCpuTimer();
    work(wps, &result);
    const double end_time = CmiCpuTimer();
    const double correction = elapse / (end_time - start_time);
    wps = (int)((double)wps * correction + 0.5);
  }

  return wps;
}

#else
int LDProcessorSpeed() { return 1; }
#endif  // CMK_LBDB_ON

int LBManager::ProcessorSpeed()
{
  static int peSpeed = LDProcessorSpeed();
  return peSpeed;
}

/*
  callable from user's code
*/
void TurnManualLBOn()
{
#if CMK_LBDB_ON
  LBManager* myLbdb = LBManager::Object();
  if (myLbdb)
  {
    myLbdb->TurnManualLBOn();
  }
  else
  {
    LBManager::manualOn = true;
  }
#endif
}

void TurnManualLBOff()
{
#if CMK_LBDB_ON
  LBManager* myLbdb = LBManager::Object();
  if (myLbdb)
  {
    myLbdb->TurnManualLBOff();
  }
  else
  {
    LBManager::manualOn = true;
  }
#endif
}

void LBTurnInstrumentOn()
{
#if CMK_LBDB_ON
  if (CkpvAccess(lbmanagerInited))
    LBManager::Object()->CollectStatsOn();
  else
    _lb_args.statsOn() = 1;
#endif
}

void LBTurnInstrumentOff()
{
#if CMK_LBDB_ON
  if (CkpvAccess(lbmanagerInited))
    LBManager::Object()->CollectStatsOff();
  else
    _lb_args.statsOn() = 0;
#endif
}

void LBTurnCommOn()
{
#if CMK_LBDB_ON
  _lb_args.traceComm() = 1;
#endif
}

void LBTurnCommOff()
{
#if CMK_LBDB_ON
  _lb_args.traceComm() = 0;
#endif
}

void LBClearLoads()
{
#if CMK_LBDB_ON
  LBManager::Object()->ClearLoads();
#endif
}

void LBTurnPredictorOn(LBPredictorFunction* model)
{
#if CMK_LBDB_ON
  LBManager::Object()->PredictorOn(model);
#endif
}

void LBTurnPredictorOn(LBPredictorFunction* model, int wind)
{
#if CMK_LBDB_ON
  LBManager::Object()->PredictorOn(model, wind);
#endif
}

void LBTurnPredictorOff()
{
#if CMK_LBDB_ON
  LBManager::Object()->PredictorOff();
#endif
}

void LBChangePredictor(LBPredictorFunction* model)
{
#if CMK_LBDB_ON
  LBManager::Object()->ChangePredictor(model);
#endif
}

void LBSetPeriod(double second)
{
#if CMK_LBDB_ON
  if (CkpvAccess(lbmanagerInited))
    LBManager::Object()->SetLBPeriod(second);
  else
    _lb_args.lbperiod() = second;
#endif
}

int LBRegisterObjUserData(int size) { return CkpvAccess(lbobjdatalayout).claim(size); }

#include "LBManager.def.h"

/*@}*/
