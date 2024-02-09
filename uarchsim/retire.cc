#include "pipeline.h"
#include "trap.h"
#include "mmu.h"

//P4-D Added extra parameter in pipeline_t::retire
void pipeline_t::retire(size_t& instret, size_t instret_limit) {
   bool head_valid = false;
   bool completed, exception, load_viol, br_misp, val_misp, load, store, branch, amo, csr;
   reg_t offending_PC;

   bool amo_success;
   trap_t *trap = NULL; // Supress uninitialized warning.

   //P4-D
   //RETSTATE.state = retire_state_e::RETIRE_IDLE;
   if (RETSTATE.state == retire_state_e::RETIRE_IDLE)
   {
      bool proceed = REN->precommit(RETSTATE.chkpt_id, RETSTATE.num_loads_left, RETSTATE.num_stores_left, RETSTATE.num_branches_left, RETSTATE.amo, RETSTATE.csr, RETSTATE.exception);
      //printf("RETSTATE.chkpt_id=%llu and proceed=%d and RETSTATE.exception=%d\n", RETSTATE.chkpt_id, proceed, RETSTATE.exception);
      if(proceed == false) {
         return;
      }
      else if(proceed) {
         // Sanity checks of the 'amo' and 'csr' flags.
         assert(!RETSTATE.amo || IS_AMO(PAY.buf[PAY.head].flags));
         assert(!RETSTATE.csr || IS_CSR(PAY.buf[PAY.head].flags));
         if (RETSTATE.amo || RETSTATE.csr) {
            // There should be only 1 instruction – the amo or csr –
            // between the oldest and next oldest checkpoint.
            // So the following assertions should succeed.
            assert(RETSTATE.num_loads_left <= 1);
            assert(RETSTATE.num_stores_left <= 1);
            assert(RETSTATE.num_branches_left == 0);
            // load and store are declared as local variables (bool)
            load = (RETSTATE.num_loads_left > 0);
            store = (RETSTATE.num_stores_left > 0);
         }
         if (!RETSTATE.exception) {
            if (RETSTATE.amo && !(load || store)) { // amo, excluding load-with-reservation (LR) and store-conditional (SC)
               RETSTATE.exception = execute_amo();
            }
            else if (RETSTATE.csr) {
               RETSTATE.exception = execute_csr();
            }
            // This is probably optional.
            // Just doing it out of completeness and adapting existing code.
            if (RETSTATE.exception)
               REN->set_exception(RETSTATE.chkpt_id);
         }

         if (RETSTATE.exception)   // exception is true
         {
            trap = PAY.buf[PAY.head].trap.get();
            // CSR exceptions are micro-architectural exceptions and are
            // not defined by the ISA. These must be handled exclusively by
            // the micro-arch and is different from other exceptions specified
            // in the ISA.
            // This is a serialize trap - Refetch the CSR instruction
            reg_t jump_PC;
            offending_PC = PAY.buf[PAY.head].pc;
            if (trap->cause() == CAUSE_CSR_INSTRUCTION) {
               jump_PC = offending_PC;
            } 
            else {
               jump_PC = take_trap(*trap, offending_PC);
            }

            // Keep track of the number of retired instructions.
            instret++;
            num_insn++;	
            inc_counter(commit_count);
            inc_counter(exception_count);
            // Compare pipeline simulator against functional simulator.
            checker();
                     // Squash the pipeline.
            squash_complete(jump_PC);
            inc_counter(recovery_count);
            // Flush PAY.
            PAY.clear();
            RETSTATE.state = retire_state_e::RETIRE_IDLE;

            update_timer(&state, 1); // Update timer by 1 retired instr.
            assert(instret <= instret_limit);

            return;
         }
         else
         {
            RETSTATE.state = retire_state_e::RETIRE_BULK_COMMIT;
            RETSTATE.log_reg = 0;
         }
      }
   }
   else if (RETSTATE.state == retire_state_e::RETIRE_BULK_COMMIT)
   {
      for (unsigned int x=0; x<RETIRE_WIDTH; x++) {
         if(RETSTATE.num_loads_left !=0) {
            LSU.train(true);	     // Train MDP and update stats.
            amo_success = LSU.commit(true, RETSTATE.amo);
            RETSTATE.num_loads_left--;
            assert(amo_success);
         }
         else if (RETSTATE.num_loads_left == 0) {
            break;
         }
      }
      for (unsigned int x = 0; x<RETIRE_WIDTH; x++) {
         if(RETSTATE.num_stores_left != 0){
            LSU.train(false);
            amo_success = LSU.commit(false,RETSTATE.amo);
            RETSTATE.num_stores_left--;
            assert(amo_success);
         }
         else if (RETSTATE.num_stores_left == 0) {
            break;
         }
      }
      for (unsigned int x =0; x<RETIRE_WIDTH; x++) {
         if (RETSTATE.num_branches_left != 0) {
            FetchUnit->commit();
            RETSTATE.num_branches_left--;
         }
         else if (RETSTATE.num_branches_left == 0) {
            break;
         }
      }
      for (unsigned int x =0; x<RETIRE_WIDTH; x++) {
         if (RETSTATE.log_reg != NXPR+NFPR) {
               REN->commit(RETSTATE.log_reg);
               RETSTATE.log_reg++;
         }
         else if (RETSTATE.log_reg == NXPR+NFPR) {
            break;
         }
      }
      //printf("Number of loads left = %llu, Number of Stores left = %llu and Number of branches left = %llu Number of logic register = %llu Checkpoint_id = %llu\n", RETSTATE.num_loads_left, RETSTATE.num_stores_left, RETSTATE.num_branches_left, RETSTATE.log_reg, RETSTATE.chkpt_id);
      if((RETSTATE.num_loads_left == 0) && (RETSTATE.num_stores_left == 0) && (RETSTATE.num_branches_left == 0) && (RETSTATE.log_reg == NXPR+NFPR)) { 
         REN->free_checkpoint();
         RETSTATE.state = retire_state_e::RETIRE_FINALIZE;
      }
   }
   else if (RETSTATE.state == retire_state_e::RETIRE_FINALIZE)
   {
      while((PAY.buf[PAY.head].chkpt_id == RETSTATE.chkpt_id) && (PAY.head != PAY.tail))
      {
         if (IS_FP_OP(PAY.buf[PAY.head].flags)) {
            // post the FP exception bit to CSR fflags (the Accrued Exception Flags)
            get_state()->fflags |= PAY.buf[PAY.head].fflags;
         }

         // Check results.
         checker();

         // Keep track of the number of retired instructions.
         num_insn++;
         instret++;
         inc_counter(commit_count);
         if (PAY.buf[PAY.head].split && PAY.buf[PAY.head].upper)
            num_insn_split++;

         if (RETSTATE.amo || RETSTATE.csr) {   // Resume the stalled fetch unit after committing a serializing instruction.
            assert(!RETSTATE.amo || IS_AMO(PAY.buf[PAY.head].flags));
            assert(!RETSTATE.csr || IS_CSR(PAY.buf[PAY.head].flags));
            insn_t inst = PAY.buf[PAY.head].inst;
            reg_t next_inst_pc;
            if ((inst.funct3() == FN3_SC_SB) && (inst.funct12() == FN12_SRET))  // SRET instruction.
            {
               next_inst_pc = state.epc;
            }
            else
            {
               next_inst_pc = INCREMENT_PC(PAY.buf[PAY.head].pc);
            }

            // The serializing instruction stalled the fetch unit so the pipeline is now empty. Resume fetch.
            FetchUnit->flush(next_inst_pc);
         }
         
         if (!PAY.buf[PAY.head].split) PAY.pop();
         PAY.pop();
         
         update_timer(&state, 1); // Update timer by 1 retired instr.
         // Pause, but remain in the RETIRE_FINALIZE state for
         // the next cycle, if it’s time for an HTIF tick,
         // as this will change state.
         if (instret == instret_limit)
            return; // Pause and remain in the state RETIRE_FINALIZE.
      }
      RETSTATE.state = retire_state_e::RETIRE_IDLE;
   }
}


bool pipeline_t::execute_amo() {
   unsigned int index = PAY.head;
   insn_t inst = PAY.buf[index].inst;
   reg_t read_amo_value = 0xdeadbeef;
   bool exception = false;

   try {
      if (inst.funct3() == FN3_AMO_W) {
         read_amo_value = mmu->load_int32(PAY.buf[index].A_value.dw);
         uint32_t write_amo_value;
         switch (inst.funct5()) {
            case FN5_AMO_SWAP:
               write_amo_value = PAY.buf[index].B_value.dw;
               break;
            case FN5_AMO_ADD:
               write_amo_value = PAY.buf[index].B_value.dw + read_amo_value;
               break;
            case FN5_AMO_XOR:
               write_amo_value = PAY.buf[index].B_value.dw ^ read_amo_value;
               break;
            case FN5_AMO_AND:
               write_amo_value = PAY.buf[index].B_value.dw & read_amo_value;
               break;
            case FN5_AMO_OR:
               write_amo_value = PAY.buf[index].B_value.dw | read_amo_value;
               break;
            case FN5_AMO_MIN:
               write_amo_value = std::min(int32_t(PAY.buf[index].B_value.dw), int32_t(read_amo_value));
               break;
            case FN5_AMO_MAX:
               write_amo_value = std::max(int32_t(PAY.buf[index].B_value.dw), int32_t(read_amo_value));
               break;
            case FN5_AMO_MINU:
               write_amo_value = std::min(uint32_t(PAY.buf[index].B_value.dw), uint32_t(read_amo_value));
               break;
            case FN5_AMO_MAXU:
               write_amo_value = std::max(uint32_t(PAY.buf[index].B_value.dw), uint32_t(read_amo_value));
               break;
            default:
               assert(0);
               break;
         }
         mmu->store_uint32(PAY.buf[index].A_value.dw, write_amo_value);
      }
      else if (inst.funct3() == FN3_AMO_D) {
         read_amo_value = mmu->load_int64(PAY.buf[index].A_value.dw);
         reg_t write_amo_value;
         switch (inst.funct5()) {
            case FN5_AMO_SWAP:
               write_amo_value = PAY.buf[index].B_value.dw;
               break;
            case FN5_AMO_ADD:
               write_amo_value = PAY.buf[index].B_value.dw + read_amo_value;
               break;
            case FN5_AMO_XOR:
               write_amo_value = PAY.buf[index].B_value.dw ^ read_amo_value;
               break;
            case FN5_AMO_AND:
               write_amo_value = PAY.buf[index].B_value.dw & read_amo_value;
               break;
            case FN5_AMO_OR:
               write_amo_value = PAY.buf[index].B_value.dw | read_amo_value;
               break;
            case FN5_AMO_MIN:
               write_amo_value = std::min(int64_t(PAY.buf[index].B_value.dw), int64_t(read_amo_value));
               break;
            case FN5_AMO_MAX:
               write_amo_value = std::max(int64_t(PAY.buf[index].B_value.dw), int64_t(read_amo_value));
               break;
            case FN5_AMO_MINU:
               write_amo_value = std::min(PAY.buf[index].B_value.dw, read_amo_value);
               break;
            case FN5_AMO_MAXU:
               write_amo_value = std::max(PAY.buf[index].B_value.dw, read_amo_value);
               break;
            default:
               assert(0);
               break;
         }
         mmu->store_uint64(PAY.buf[index].A_value.dw, write_amo_value);
      }
      else {
         assert(0);
      }
   }
   catch (mem_trap_t& t) {
      exception = true;
      assert(t.cause() == CAUSE_FAULT_STORE || t.cause() == CAUSE_MISALIGNED_STORE);
      PAY.buf[index].trap.post(t);
   }

   // Record the loaded value in the payload buffer for checking purposes.
   PAY.buf[index].C_value.dw = read_amo_value;

   // Write the loaded value to the destination physical register.
   // "amoswap" may have rd=x0 (effectively no destination register) to implement a "sequentially consistent store" (see RISCV ISA spec).
   //assert(PAY.buf[index].C_valid);
   if (PAY.buf[index].C_valid) {
      REN->set_ready(PAY.buf[index].C_phys_reg);
      REN->write(PAY.buf[index].C_phys_reg, PAY.buf[index].C_value.dw);
   }

   return(exception);
}


bool pipeline_t::execute_csr() {
   unsigned int index = PAY.head;
   insn_t inst = PAY.buf[index].inst;
   pipeline_t *p = this; // *p is assumed by the validate_csr and require_supervisor macros.
   int csr;
   bool exception = false;

   // CSR instructions:
   // 1. read the addressed CSR and write its old value into a destination register,
   // 2. modify the old value to get a new value, and
   // 3. write the new value into the addressed CSR.
   reg_t old_value;
   reg_t new_value;

   try {
      if (inst.funct3() != FN3_SC_SB) {
         switch (inst.funct3()) {
            case FN3_CLR:
               csr = validate_csr(PAY.buf[index].CSR_addr, true);
	       old_value = get_pcr(csr);
               new_value = (old_value & ~PAY.buf[index].A_value.dw);
               set_pcr(csr, new_value);
               break;
            case FN3_RW:
               csr = validate_csr(PAY.buf[index].CSR_addr, true);
	       old_value = get_pcr(csr);
               new_value = PAY.buf[index].A_value.dw;
               set_pcr(csr, new_value);
               break;
            case FN3_SET:
               csr = validate_csr(PAY.buf[index].CSR_addr, (PAY.buf[index].A_log_reg != 0));
	       old_value = get_pcr(csr);
               new_value = (old_value | PAY.buf[index].A_value.dw);
               set_pcr(csr, new_value);
               break;
            case FN3_CLR_IMM:
               csr = validate_csr(PAY.buf[index].CSR_addr, true);
	       old_value = get_pcr(csr);
               new_value = (old_value & ~(reg_t)PAY.buf[index].A_log_reg);
               set_pcr(csr, new_value);
               break;
            case FN3_RW_IMM:
               csr = validate_csr(PAY.buf[index].CSR_addr, true);
	       old_value = get_pcr(csr);
               new_value = (reg_t)PAY.buf[index].A_log_reg;
               set_pcr(csr, new_value);
               break;
            case FN3_SET_IMM:
               csr = validate_csr(PAY.buf[index].CSR_addr, true);
	       old_value = get_pcr(csr);
               new_value = (old_value | (reg_t)PAY.buf[index].A_log_reg);
               set_pcr(csr, new_value);
               break;
            default:
               assert(0);
               break;
         }
      }
      else if (inst.funct12() == FN12_SRET) {
         // This is a macro defined in decode.h.
         // This will throw a privileged_instruction trap if processor not in supervisor mode.
         require_supervisor; 
         csr = validate_csr(PAY.buf[index].CSR_addr, true);
         old_value = get_pcr(csr);
         new_value = ((old_value & ~(SR_S | SR_EI)) | ((old_value & SR_PS) ? SR_S : 0) | ((old_value & SR_PEI) ? SR_EI : 0));
         set_pcr(csr, new_value);
      }
      else {
         // SCALL and SBREAK.
         // These skip the IQ and execution lanes (completed in Dispatch Stage).
         assert(0);
      }

      if (PAY.buf[index].C_valid) {
         // Write the result (old value of CSR) to the payload buffer for checking purposes.
         PAY.buf[index].C_value.dw = old_value;
         // Write the result (old value of CSR) to the physical destination register.
         REN->set_ready(PAY.buf[index].C_phys_reg);
         REN->write(PAY.buf[index].C_phys_reg, PAY.buf[index].C_value.dw);
      }
   }
   catch (trap_t& t) {
      exception = true;
      assert(t.cause() == CAUSE_PRIVILEGED_INSTRUCTION || t.cause() == CAUSE_FP_DISABLED);
      PAY.buf[index].trap.post(t);
   }
   catch (serialize_t& s) {
      exception = true;
      PAY.buf[index].trap.post(trap_csr_instruction());
   }

   return(exception);
}

