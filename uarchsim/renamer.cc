#include <iostream>
#include <inttypes.h>
#include <cassert>
#include <vector>
using std::vector;
#include <bits/stdc++.h>
#include <math.h>
#include <renamer.h>

using namespace std;

renamer::renamer(uint64_t n_log_regs, uint64_t n_phys_regs, uint64_t n_branches, uint64_t n_active)
{
    //std::cout << "Inputs Passed -- " << n_log_regs << " " << n_phys_regs << " " << n_branches << " " << n_active << '\n';
    assert (n_phys_regs > n_log_regs);
    assert ((n_branches >= 1) && (n_branches <= 64));
    assert (n_active > 0);
    
    initializeRMT(n_log_regs);
    initializeFreeList(n_phys_regs, n_log_regs);
    initializePRF(n_phys_regs);
    initializePRFReadyBits(n_phys_regs);
    initializeCPR(n_phys_regs, n_log_regs);
    initializecheckPointBuffer(n_phys_regs, n_log_regs, n_branches, n_active);
    //printf("* Completed renamer()\n");
    //printDetailedStates();
    //printCheckpointBufferState();
    //printfreeListState();
    //printUsageCounterState();
}

///////////////////
// Rename Stage. //
///////////////////
bool renamer::stall_reg(uint64_t bundle_dst)
{
    if (bundle_dst > noOfFreeRegistersInFreeList())
    {
        //printf("* Completed stall_reg(): Insufficient Free Registers (required=%llu vs available=%llu)\n", bundle_dst, noOfFreeRegistersInFreeList());
        return true;
    }
    else
    {
        //printf("* Completed stall_reg(): There are Free Registers (required=%llu vs available=%llu)\n", bundle_dst, noOfFreeRegistersInFreeList());
        return false;
    }
}

/////////////////////////////////////////////////////////////////////
// This function is used to rename a single source register.
/////////////////////////////////////////////////////////////////////
uint64_t renamer::rename_rsrc(uint64_t log_reg)
{
    uint64_t phys_reg = RMT.entry[log_reg];
    //printf("Increament usage counter because of register source renaming\n");
    //printf("* Completed rename_rsrc() Renaming of Source Register r%llu to p%llu from RMT\n", log_reg, phys_reg);
    inc_usage_counter(phys_reg);
    return phys_reg;
}

/////////////////////////////////////////////////////////////////////
// This function is used to rename a single destination register.
/////////////////////////////////////////////////////////////////////
uint64_t renamer::rename_rdst(uint64_t log_reg)
{
    uint64_t old_phys_reg = RMT.entry[log_reg];
    unmap(old_phys_reg);

    uint64_t new_phys_reg = popRegisterFromFreeList();
    RMT.entry[log_reg] = new_phys_reg;
    //printf("* Completed rename_rdst() Renaming of Destination Register r%llu to p%llu in RMT\n", log_reg, new_phys_reg);
    inc_usage_counter(new_phys_reg);
    map(new_phys_reg);
        
    return new_phys_reg;
}

/////////////////////////////////////////////////////////////////////
// This function creates a new branch checkpoint.
/////////////////////////////////////////////////////////////////////
// P4 - checkpoint()
void renamer::checkpoint()
{
    //printf("* Requested checkpoint_Tail() at checkpoint chkpt_id=%llu\n", checkPointBuffer.tail);
    // Asserting that the checkpoint at checkPointBuffer's tail is not valid (empty)
    //printf(" Before Checkpoint taking place\n");
    //printf("Head = %llu     HeadPhase = %llu      Tail = %llu    TailPhase = %llu\n", checkPointBuffer.head,checkPointBuffer.headPhase, checkPointBuffer.tail, checkPointBuffer.tailPhase);
    assert(checkPointBuffer.valid[checkPointBuffer.tail] == false);
    
    checkPointBuffer.valid[checkPointBuffer.tail] = true;
    checkPointBuffer.RMT[checkPointBuffer.tail] = RMT;
    checkPointBuffer.CPR[checkPointBuffer.tail] = CPR;
    
    for (uint64_t i = 0; i < RMT.size; i++)
    {
        // RMT.entry[i] contains Physical Reg number and we increament all Phy Regs
        // which are currently mapped/present in RMT to Logical Registers
        inc_usage_counter(RMT.entry[i]);
    }

    checkPointBuffer.tail = (checkPointBuffer.tail + 1) % checkPointBuffer.size;
    if (checkPointBuffer.tail ==  0)
        checkPointBuffer.tailPhase  = !(checkPointBuffer.tailPhase);

    //printf("* Checkpoint_tail after Completed checkpoint(). So next chkpt_id=%llu\n", checkPointBuffer.tail);
    //printf("After Checkpoint takes place\n");
    //printf("Head = %llu     HeadPhase = %llu      Tail = %llu    TailPhase = %llu\n", checkPointBuffer.head,checkPointBuffer.headPhase, checkPointBuffer.tail, checkPointBuffer.tailPhase);
}

// P4-D get_checkpoint_id
uint64_t renamer::get_checkpoint_id(bool load, bool store, bool branch, bool amo, bool csr)
{
    uint64_t latest_chkpt_id = 0;

    if (checkPointBuffer.tail == 0)
    {
        latest_chkpt_id = checkPointBuffer.size - 1;
    }
    else
    {
        latest_chkpt_id = checkPointBuffer.tail - 1;
    }

    //printf("* Checking checkpoint at chkpt_id=%llu\n", latest_chkpt_id);
    assert(checkPointBuffer.valid[latest_chkpt_id] == true);
    
    if ((checkPointBuffer.CPR[latest_chkpt_id].amoFlag == false && amo == true) || (checkPointBuffer.CPR[latest_chkpt_id].csrFlag == false && csr == true))
    {
        if (load == true)
            incr_load_count(latest_chkpt_id);
        if (store == true)
            incr_store_count(latest_chkpt_id);
        if (branch == true)
            incr_branch_count(latest_chkpt_id);
        incr_uncomp_instr(latest_chkpt_id);
    }
    else if (amo == false && csr == false)
    {
        if (load == true)
            incr_load_count(latest_chkpt_id);
        if (store == true)
            incr_store_count(latest_chkpt_id);
        if (branch == true)
            incr_branch_count(latest_chkpt_id);
        incr_uncomp_instr(latest_chkpt_id);
    }

    /*if (latest_chkpt_id==4)
    {
        //printf("uncomp_instr=%llu\n",checkPointBuffer.CPR[latest_chkpt_id].uncomp_instr);
        //printf("load=%llu, store=%llu, branch=%llu, amo=%llu, csr=%llu\n", load, store, branch, amo, csr);
    }*/

    if (checkPointBuffer.CPR[latest_chkpt_id].amoFlag == false)
        checkPointBuffer.CPR[latest_chkpt_id].amoFlag = amo;
    if (checkPointBuffer.CPR[latest_chkpt_id].csrFlag == false)
        checkPointBuffer.CPR[latest_chkpt_id].csrFlag = csr;
    //printf("Number of load_count = %llu, Number of store_count = %llu, Number of branch_count = %llu\n", checkPointBuffer.CPR[latest_chkpt_id].load_count, checkPointBuffer.CPR[latest_chkpt_id].store_count, checkPointBuffer.CPR[latest_chkpt_id].branch_count);
    //printf("* Completed get_checkpoint_id uncomp_instr=%llu chkpt_id=%llu\n", checkPointBuffer.CPR[latest_chkpt_id].uncomp_instr, latest_chkpt_id);

    return latest_chkpt_id;
}

//P4-D free_checkpoint()
void renamer::free_checkpoint()
{
    //printFreeRegs("start of free_checkpoint\n");
    //printf("Before freeing Checkpoint\n");
    //printf("Head = %llu     HeadPhase = %llu      Tail = %llu    TailPhase = %llu\n", checkPointBuffer.head,checkPointBuffer.headPhase, checkPointBuffer.tail, checkPointBuffer.tailPhase);
    uint64_t oldest_chkpt_id = checkPointBuffer.head;
    //printf("Old head before free checkpoint = %llu\n", oldest_chkpt_id);
    assert(checkPointBuffer.CPR[oldest_chkpt_id].uncomp_instr == 0);
    assert(checkPointBuffer.valid[oldest_chkpt_id] == true);
    checkPointBuffer.valid[oldest_chkpt_id] = false;

    checkPointBuffer.head = (checkPointBuffer.head + 1) % checkPointBuffer.size;
    if(checkPointBuffer.head == 0)
        checkPointBuffer.headPhase = !(checkPointBuffer.headPhase);
    
    //printFreeRegs("end of free_checkpoint\n");

    //printf("* Completed free_checkpoint for oldest_chkpt_id=%llu\n", oldest_chkpt_id);
    assert (checkPointBuffer.head != checkPointBuffer.tail);
}

bool renamer::stall_checkpoint(uint64_t bundle_chkpts)
{
    if (bundle_chkpts > noOfFreeCheckpoints())
    {
        //printf("* Completed stall_checkpoint(): Insufficient checkPointBuffer entries (required=%llu vs available=%llu)\n", bundle_chkpts, noOfFreeCheckpoints());
        return true;
    }
    else
    {
        //printf("* Completed stall_checkpoint(): sufficient checkPointBuffer entries (required=%llu vs available=%llu)\n", bundle_chkpts, noOfFreeCheckpoints());
        return false;
    }
}

/////////////////////
// Dispatch Stage. //
/////////////////////
/////////////////////////////////////////////////////////////////////
// The Dispatch Stage must stall if there are not enough free
// entries in the Active List for all instructions in the current
// dispatch bundle.
//
// Inputs:
// 1. bundle_inst: number of instructions in current dispatch bundle
//
// Return value:
// Return "true" (stall) if the Active List does not have enough
// space for all instructions in the dispatch bundle.
/////////////////////////////////////////////////////////////////////
bool renamer::stall_dispatch(uint64_t bundle_inst)
{
    if (bundle_inst > noOfFreeEntriesInActiveList())
    {
        //printf("* Completed stall_dispatch(): Insufficient Active List entries (required=%llu vs available=%llu)\n", bundle_inst, noOfFreeEntriesInActiveList());
        return true;
    }
    else
    {
        //printf("* Completed stall_dispatch(): sufficient Active List entries (required=%llu vs available=%llu)\n", bundle_inst, noOfFreeEntriesInActiveList());
        return false;
    }
}

/////////////////////////////////////////////////////////////////////
// This function dispatches a single instruction into the Active
// List.
//
// Inputs:
// 1. dest_valid: If 'true', the instr. has a destination register,
//    otherwise it does not. If it does not, then the log_reg and
//    phys_reg inputs should be ignored.
// 2. log_reg: Logical register number of the instruction's
//    destination.
// 3. phys_reg: Physical register number of the instruction's
//    destination.
// 4. load: If 'true', the instr. is a load, otherwise it isn't.
// 5. store: If 'true', the instr. is a store, otherwise it isn't.
// 6. branch: If 'true', the instr. is a branch, otherwise it isn't.
// 7. amo: If 'true', this is an atomic memory operation.
// 8. csr: If 'true', this is a system instruction.
// 9. PC: Program counter of the instruction.
//
// Return value:
// Return the instruction's index in the Active List.
//
// Tips:
//
// Before dispatching the instruction into the Active List, assert
// that the Active List isn't full: it is the user's responsibility
// to avoid a structural hazard by calling stall_dispatch()
// in advance.
/////////////////////////////////////////////////////////////////////
uint64_t renamer::dispatch_inst(bool dest_valid, uint64_t log_reg, uint64_t phys_reg, 
                                    bool load, bool store, bool branch, bool amo, bool csr, uint64_t PC)
{
    TS_activeListEntries entry;
    (dest_valid == true) ? entry.destinationFlag = 1 : entry.destinationFlag = 0;
    entry.logicalRegisterNumber = log_reg;
    entry.physicalRegisterNumber = phys_reg;
    
    entry.completedBit = 0;
    entry.exceptionBit = 0;
    entry.loadViolationBit = 0;
    entry.branchMispredictionBit = 0;
    entry.valueMispredictionBit = 0;

    (load == true) ? entry.loadFlag = 1 : entry.loadFlag = 0;
    (store == true) ? entry.storeFlag = 1 : entry.storeFlag = 0;
    (branch == true) ? entry.branchFlag = 1 : entry.branchFlag = 0;
    (amo == true) ? entry.amoFlag = 1 : entry.amoFlag = 0;
    (csr == true) ? entry.csrFlag = 1 : entry.csrFlag = 0;

    entry.PC = PC;

    assert ((activeList.head != activeList.tail) || (activeList.headPhase == activeList.tailPhase));

    activeList.entry[activeList.tail] = entry;
    uint64_t indexInActiveList = activeList.tail;
    if (activeList.tail < activeList.size - 1)
    {
        activeList.tail++;
    }
    else
    {
        assert (activeList.tailPhase == activeList.headPhase);

        activeList.tail = 0;
        activeList.tailPhase  = !(activeList.tailPhase);
    }

    //printf("* Completed dispatch_inst() inserting entry in Active List at AL_index=%llu\n", indexInActiveList);
    //printDetailedStates();
    return indexInActiveList;
}

/////////////////////////////////////////////////////////////////////
// Test the ready bit of the indicated physical register.
// Returns 'true' if ready.
/////////////////////////////////////////////////////////////////////
bool renamer::is_ready(uint64_t phys_reg)
{
    //printf("* Completed is_ready() for PRF Ready Bit entry at p%llu value=%llu\n", phys_reg, PRFReadyBits.entry[phys_reg]);
    return PRFReadyBits.entry[phys_reg];
}

/////////////////////////////////////////////////////////////////////
// Clear the ready bit of the indicated physical register.
/////////////////////////////////////////////////////////////////////
void renamer::clear_ready(uint64_t phys_reg)
{
    PRFReadyBits.entry[phys_reg] = 0;
    //printf("* Completed clear_ready() to PRF Ready Bit entry at p%llu value=%llu\n", phys_reg, PRFReadyBits.entry[phys_reg]);
}

/////////////////////////////////////////////////////////////
// Functions related to the RegisterRead and Execute Stage //
/////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////
// Return the contents (value) of the indicated physical register.
/////////////////////////////////////////////////////////////////////
uint64_t renamer::read(uint64_t phys_reg)
{
    //printf("* Completed read() for PRF entry at p%llu value=%llu\n", phys_reg, PRF.entry[phys_reg]);
    dec_usage_counter(phys_reg);
    return PRF.entry[phys_reg];
}
/////////////////////////////////////////////////////////////////////
// Set the ready bit of the indicated physical register.
/////////////////////////////////////////////////////////////////////
void renamer::set_ready(uint64_t phys_reg)
{
    PRFReadyBits.entry[phys_reg] = 1;
    //printf("* Completed set_ready() to PRF Ready Bit entry at p%llu value=%llu\n", phys_reg, PRFReadyBits.entry[phys_reg]);
}

//////////////////////////////////////////
// Functions related to Writeback Stage //
//////////////////////////////////////////

/////////////////////////////////////////////////////////////////////
// Write a value into the indicated physical register.
/////////////////////////////////////////////////////////////////////
void renamer::write(uint64_t phys_reg, uint64_t value)
{
    //printf("* Completed write() to PRF entry at p%llu value=%llu\n", phys_reg, value);
    dec_usage_counter(phys_reg);
    PRF.entry[phys_reg] = value;
}

/////////////////////////////////////////////////////////////////////
// Set the completed bit of the indicated entry in the Active List.
/////////////////////////////////////////////////////////////////////
//P4-D
void renamer::set_complete(uint64_t chkpt_id) {
    checkPointBuffer.CPR[chkpt_id].uncomp_instr--;
    //printf("* Completed set_complete for chkpt_id=%llu and uncomp_instr=%llu\n", chkpt_id, checkPointBuffer.CPR[chkpt_id].uncomp_instr);
}

/////////////////////////////////////////////////////////////////////
// This function is for handling branch resolution.
/////////////////////////////////////////////////////////////////////
//void renamer::resolve(uint64_t AL_index, uint64_t branch_ID, bool correct)
uint64_t renamer::rollback(uint64_t chkpt_id, bool next, uint64_t &total_loads, uint64_t &total_stores, uint64_t &total_branches)
{
    if (next == true)
        chkpt_id = (chkpt_id + 1) % checkPointBuffer.size;  //rollback_chkpt_id

    assert (checkPointBuffer.valid[chkpt_id] == true);
    for(uint64_t i = 0; i< RMT.size; i++) 
    {
        if (checkPointBuffer.RMT[chkpt_id].entry[i] != RMT.entry[i])
        {
            unmap(RMT.entry[i]);
            map(checkPointBuffer.RMT[chkpt_id].entry[i]);
            RMT.entry[i] = checkPointBuffer.RMT[chkpt_id].entry[i];
        }
    }
    
    checkPointBuffer.CPR[chkpt_id].branchFlag = 0;
    checkPointBuffer.CPR[chkpt_id].amoFlag = 0;
    checkPointBuffer.CPR[chkpt_id].csrFlag = 0;
    checkPointBuffer.CPR[chkpt_id].exceptionBit = 0;
    checkPointBuffer.CPR[chkpt_id].uncomp_instr = 0;
    checkPointBuffer.CPR[chkpt_id].load_count = 0;
    checkPointBuffer.CPR[chkpt_id].store_count = 0;
    checkPointBuffer.CPR[chkpt_id].branch_count = 0;
    CPR = checkPointBuffer.CPR[chkpt_id];

    uint64_t squash_mask = 0;
    uint64_t youngest_chkpt_id = 0;
    (checkPointBuffer.tail == 0) ? youngest_chkpt_id = checkPointBuffer.size - 1 : youngest_chkpt_id = checkPointBuffer.tail - 1;
    uint64_t id = 0;
    for (id = (checkPointBuffer.size-1); id > 0; id--)
    {
        if (inBetween(id, chkpt_id, youngest_chkpt_id) == true)
        {
            squash_mask = squash_mask | 1;
        }
        squash_mask = squash_mask << 1;
    }
    
    id = 0;
    if (inBetween(id, chkpt_id, youngest_chkpt_id) == true)
    {
        squash_mask = squash_mask | 1;
    }

    uint64_t c = (chkpt_id+1) % checkPointBuffer.size;
    while (c != checkPointBuffer.tail) {
        checkPointBuffer.valid[c] = 0;
        for(uint64_t i = 0; i< RMT.size; i++) {
            int phys_reg = checkPointBuffer.RMT[c].entry[i];
            assert(checkPointBuffer.CPR[c].unmappedBit[phys_reg] == 0); 
            dec_usage_counter(phys_reg);
        }
        c = (c+1) % checkPointBuffer.size;
    }

    c = checkPointBuffer.head;
    while (c != chkpt_id)
    {
        total_loads += checkPointBuffer.CPR[c].load_count;
        total_stores += checkPointBuffer.CPR[c].store_count;
        total_branches += checkPointBuffer.CPR[c].branch_count;
        c = (c + 1) % checkPointBuffer.size;
    }
    bool chkpt_id_phase = false;   
    if (checkPointBuffer.headPhase == checkPointBuffer.tailPhase) {
        chkpt_id_phase = checkPointBuffer.headPhase; 
    }
    else {
        if (chkpt_id > checkPointBuffer.head){
            chkpt_id_phase = checkPointBuffer.headPhase;
        }
        else {
            chkpt_id_phase = !(checkPointBuffer.headPhase);
        }
    }
    checkPointBuffer.tail = (chkpt_id + 1) % checkPointBuffer.size;
    if (checkPointBuffer.tail == 0)
    {
        checkPointBuffer.tailPhase = !(chkpt_id_phase);
    }
    else{
        checkPointBuffer.tailPhase = chkpt_id_phase;
    }
    return squash_mask;
}

//////////////////////////////////////////
// Functions related to Retire Stage.   //
//////////////////////////////////////////

///////////////////////////////////////////////////////////////////
// This function allows the caller to examine the instruction at the head
// of the Active List.
///////////////////////////////////////////////////////////////////
// P4-D
bool renamer::precommit(uint64_t &chkpt_id, uint64_t &num_loads, uint64_t &num_stores, uint64_t &num_branches, bool &amo, bool &csr, bool &exception)
{
    //assert(checkPointBuffer.head <= chkpt_id);
    //assert(checkPointBuffer.tail > chkpt_id);
    uint64_t oldest_chkpt_id = checkPointBuffer.head;    
    chkpt_id = oldest_chkpt_id;

    num_loads = checkPointBuffer.CPR[chkpt_id].load_count;
    num_stores = checkPointBuffer.CPR[chkpt_id].store_count;
    num_branches = checkPointBuffer.CPR[chkpt_id].branch_count;
    amo = checkPointBuffer.CPR[chkpt_id].amoFlag;
    csr = checkPointBuffer.CPR[chkpt_id].csrFlag;
    exception = checkPointBuffer.CPR[chkpt_id].exceptionBit;

    uint64_t next_oldest_chkpt_id;
    if (oldest_chkpt_id < checkPointBuffer.size - 1)
    {
        next_oldest_chkpt_id = oldest_chkpt_id + 1;
    }
    else
    {
        next_oldest_chkpt_id = 0;
    }
    //printf("Precommit: Number of loads = %llu, Number of Stores = %llu, Number of branch = %llu, exception = %d for Chkpt_id = %llu\n", num_loads, num_stores, num_branches, exception, chkpt_id);
    //if (exception == true)
        //printf("* Completed precommit Oldest chkpt_id=%llu & Uncompleted Instruc=%llu -- Next chkpt_id=%llu & Valid=%d\n", oldest_chkpt_id, checkPointBuffer.CPR[oldest_chkpt_id].uncomp_instr, next_oldest_chkpt_id, checkPointBuffer.valid[next_oldest_chkpt_id]);
    if ((checkPointBuffer.CPR[oldest_chkpt_id].uncomp_instr==0) && (checkPointBuffer.valid[next_oldest_chkpt_id]==1 || checkPointBuffer.CPR[oldest_chkpt_id].exceptionBit==1))
    {
        return true;
    }
    else
    {
        return false;
    }
}

/////////////////////////////////////////////////////////////////////
// This function commits the instruction at the head of the Active List.
/////////////////////////////////////////////////////////////////////
// P4-D
void renamer::commit(uint64_t log_reg) {
    //printf("* Started commit of log_reg=%llu\n", log_reg);
    uint64_t oldest_chkpt_id = checkPointBuffer.head;
    assert(log_reg < RMT.size);                                 // Num of Logical Regs = RMT.size
    uint64_t phys_reg = checkPointBuffer.RMT[oldest_chkpt_id].entry[log_reg];
    dec_usage_counter(phys_reg);
    //printf("* Completed commit of log_reg=%llu phys_reg=%llu\n", log_reg, phys_reg);
}

//////////////////////////////////////////////////////////////////////
// Squash the renamer class.
/////////////////////////////////////////////////////////////////////
// P4 - D
void renamer::squash()
{
   //printf("* Started squash() of the pipeline\n");
    uint64_t chkpt_id = checkPointBuffer.head;
    RMT = checkPointBuffer.RMT[chkpt_id];

    for(uint64_t i = 0; i< RMT.size; i++) 
    {
        if (checkPointBuffer.RMT[chkpt_id].entry[i] != RMT.entry[i])
        {
            unmap(RMT.entry[i]);
            map(checkPointBuffer.RMT[chkpt_id].entry[i]);
            RMT.entry[i] = checkPointBuffer.RMT[chkpt_id].entry[i];
        }
    }

    checkPointBuffer.CPR[chkpt_id].branchFlag = 0;
    checkPointBuffer.CPR[chkpt_id].amoFlag = 0;
    checkPointBuffer.CPR[chkpt_id].csrFlag = 0;
    checkPointBuffer.CPR[chkpt_id].exceptionBit = 0;
    checkPointBuffer.CPR[chkpt_id].uncomp_instr = 0;
    checkPointBuffer.CPR[chkpt_id].load_count = 0;
    checkPointBuffer.CPR[chkpt_id].store_count = 0;
    checkPointBuffer.CPR[chkpt_id].branch_count = 0;
    CPR = checkPointBuffer.CPR[chkpt_id];

    // Iterate from next oldest to the newest checkpoint and set them to invalid
    //printf("Squash's dec_usage_counter\n");
    uint64_t c = (chkpt_id+1) % checkPointBuffer.size;
    while (c != checkPointBuffer.tail) {
        checkPointBuffer.valid[c] = 0;
        for(uint64_t i =0; i< RMT.size; i++) {
            int phys_reg = checkPointBuffer.RMT[c].entry[i];
            assert(checkPointBuffer.CPR[c].unmappedBit[phys_reg] == 0); 
            dec_usage_counter(phys_reg);
        }
        c = (c+1) % checkPointBuffer.size;
    }

    // rollback tail to oldest chkpt_id + 1
    bool chkpt_id_phase = false;
    if(checkPointBuffer.headPhase == checkPointBuffer.tailPhase) {
        chkpt_id_phase = checkPointBuffer.headPhase; 
    }
    else {
        if (chkpt_id > checkPointBuffer.head){
            chkpt_id_phase = checkPointBuffer.headPhase;
        }
        else {
            chkpt_id_phase = !(checkPointBuffer.headPhase);
        }
    }
    checkPointBuffer.tail = (chkpt_id + 1) % checkPointBuffer.size;
    if (checkPointBuffer.tail == 0)
    {
        checkPointBuffer.tailPhase = !(chkpt_id_phase);
    }
    else{
        checkPointBuffer.tailPhase = chkpt_id_phase;
    }

    //printf("* Completed squash() of the pipeline\n");
}

//////////////////////////////////////////
// Functions not tied to specific stage //
//////////////////////////////////////////
void renamer::set_exception(uint64_t chkpt_id) {
    checkPointBuffer.CPR[chkpt_id].exceptionBit = 1;
    //printf("* Completed set_exception(): set exceptionBit chkpt_id=%llu\n", chkpt_id);
}

void renamer::set_load_violation(uint64_t AL_index)
{
    activeList.entry[AL_index].loadViolationBit = 1;
    //printf("* Completed set_load_violation() AL_index=%llu\n", AL_index);
}

void renamer::set_branch_misprediction(uint64_t AL_index)
{
    activeList.entry[AL_index].branchMispredictionBit = 1;
    //printf("* Completed set_branch_misprediction() AL_index=%llu\n", AL_index);
}

void renamer::set_value_misprediction(uint64_t AL_index)
{
    activeList.entry[AL_index].valueMispredictionBit = 1;
    //printf("* Completed set_value_misprediction() AL_index=%llu\n", AL_index);
}

/////////////////////////////////////////////////////////////////////
// Query the exception bit of the indicated entry in the Active List.
/////////////////////////////////////////////////////////////////////
bool renamer::get_exception(uint64_t chkpt_id)
{
    //printf("* Completed get_exception(): passed exceptionBit to chkpt_id=%llu\n", chkpt_id);
    return checkPointBuffer.CPR[chkpt_id].exceptionBit;
}
