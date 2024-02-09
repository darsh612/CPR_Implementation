#include "pipeline.h"


////////////////////////////////////////////////////////////////////////////////////
// The Rename Stage has two sub-stages:
// rename1: Get the next rename bundle from the FQ.
// rename2: Rename the current rename bundle.
////////////////////////////////////////////////////////////////////////////////////

void pipeline_t::rename1() {
   unsigned int i;
   unsigned int rename1_bundle_width;

   ////////////////////////////////////////////////////////////////////////////////////
   // Try to get the next rename bundle.
   // Two conditions might prevent getting the next rename bundle, either:
   // (1) The current rename bundle is stalled in rename2.
   // (2) The FQ does not have enough instructions for a full rename bundle,
   //     and it's not because the fetch unit is stalled waiting for a
   //     serializing instruction to retire (fetch exception, amo, or csr instruction).
   ////////////////////////////////////////////////////////////////////////////////////

   // Check the first condition. Is the current rename bundle stalled, preventing
   // insertion of the next rename bundle? Check whether or not the pipeline register
   // between rename1 and rename2 still has a rename bundle.

   if (RENAME2[0].valid) {	// The current rename bundle is stalled.
      //printf("current rename bundle is stalled\n");
      return;
   }

   // Check the second condition.
   // Stall if the fetch unit is active (it's not waiting for a serializing
   // instruction to retire) and the FQ doesn't have enough instructions for a full
   // rename bundle.

   rename1_bundle_width = ((FQ.get_length() < dispatch_width) ? FQ.get_length() : dispatch_width);

   if (FetchUnit->active() && (rename1_bundle_width < dispatch_width)) {
      //printf("FQ does not have enough instructions for a full rename bundle\n");
      return;
   }

   // Get the next rename bundle.
   for (i = 0; i < rename1_bundle_width; i++) {
      assert(!RENAME2[i].valid);
      RENAME2[i].valid = true;
      RENAME2[i].index = FQ.pop();
   }
}

void pipeline_t::rename2() {
   //printf("rename2 func called\n");
   unsigned int i;
   unsigned int index;
   unsigned int bundle_dst, bundle_branch;
   unsigned int bundle_chkpts;
   unsigned int instrucs_w_valid_regs;
   bool serializing_flag, mispredictedBranch_flag, exception_flag, exception_checkpoint_placed_before;

   //P4-D
   bool load_flag = false;
   bool store_flag = false;
   bool branch_flag = false;
   bool amo_flag = false;
   bool csr_flag = false;

   // Stall the rename2 sub-stage if either:
   // (1) There isn't a current rename bundle.
   // (2) The Dispatch Stage is stalled.
   // (3) There aren't enough rename resources for the current rename bundle.
   if (!RENAME2[0].valid)
   {
      //printf("There isn't a current rename bundle.\n");
   }
   if (DISPATCH[0].valid) {
      //printf("The Dispatch Stage is stalled.\n");
   }
   //printf("RENAME2[0].valid=%d and DISPATH[0].valid=%d\n", RENAME2[0].valid, DISPATCH[0].valid);
   if (!RENAME2[0].valid ||	// First stall condition: There isn't a current rename bundle.
       DISPATCH[0].valid) {	// Second stall condition: The Dispatch Stage is stalled.
      return;
   }

   // Third stall condition: There aren't enough rename resources for the current rename bundle.
   bundle_dst = 0;
   bundle_branch = 0;
   bundle_chkpts = 0;
   instrucs_w_valid_regs = 0;
   int temp_instr_renamed_since_last_checkpoint = instr_renamed_since_last_checkpoint;

   db_t * actual;
   for (i = 0; i < dispatch_width; i++) {
      if (!RENAME2[i].valid)
         break;			// Not a valid instruction: Reached the end of the rename bundle so exit loop.

      index = RENAME2[i].index;

      // FIX_ME #1
      // Count the number of instructions in the rename bundle that need a checkpoint (most branches).
      // Count the number of instructions in the rename bundle that have a destination register.
      // With these counts, you will be able to query the renamer for resource availability
      // (checkpoints and physical registers).
      //
      // Tips:
      // 1. The loop construct, for iterating through all instructions in the rename bundle (0 to dispatch_width),
      //    is already provided for you, above. Note that this comment is within the loop.
      // 2. At this point of the code, 'index' is the instruction's index into PAY.buf[] (payload).
      // 3. The instruction's payload has all the information you need to count resource needs.
      //    There is a flag in the instruction's payload that *directly* tells you if this instruction needs a checkpoint.
      //    Another field indicates whether or not the instruction has a destination register.

      // FIX_ME #1 BEGIN
      if (PAY.buf[index].checkpoint)
         bundle_branch++;
      if (PAY.buf[index].C_valid)
         bundle_dst++;
      // FIX_ME #1 END
      // P4 - bundle_chkpts
      serializing_flag = false;
      mispredictedBranch_flag = false;
      exception_flag = false;
      exception_checkpoint_placed_before = false;
      if (PAY.buf[index].good_instruction)
      {
         actual = get_pipe()->peek(PAY.buf[index].db_index);
         if (actual->a_exception)
         {
            exception_flag = true;
         }
         else
         {
            if (PAY.buf[index].inst.opcode() == OP_JAL || PAY.buf[index].inst.opcode() == OP_JALR || PAY.buf[index].inst.opcode() == OP_BRANCH)
            {
               if (PAY.buf[index].next_pc != actual->a_next_pc)
               {
                  mispredictedBranch_flag = true;
               }
            }
         }
      }
      serializing_flag = PAY.buf[index].inst.opcode() == OP_AMO || PAY.buf[index].inst.opcode() == OP_SYSTEM;

      if (temp_instr_renamed_since_last_checkpoint > 0)
      {
         if (serializing_flag)
         {
            // place a checkpoint before and after a serializing instruction
            bundle_chkpts++;
            temp_instr_renamed_since_last_checkpoint = 0;
         }
         else if (exception_flag)
         {
            // place a checkpoint before an exception instruction
            bundle_chkpts++;
            temp_instr_renamed_since_last_checkpoint = 0;
            exception_checkpoint_placed_before = true;
         }
      }
      // P4 - instr_renamed_since_last_checkpoint
      //if (PAY.buf[index].A_valid || PAY.buf[index].B_valid || PAY.buf[index].D_valid || PAY.buf[index].C_valid)
         temp_instr_renamed_since_last_checkpoint++;

      //printf("serializing_flag=%d, exception_flag=%d, mispredictedBranch_flag=%d, instr_renamed_since_last_checkpoint=%llu ? max_instr_bw_checkpoints=%llu\n", serializing_flag, exception_flag, mispredictedBranch_flag, instr_renamed_since_last_checkpoint, REN->get_max_instr_bw_checkpoints());

      if (serializing_flag)
      {
         // place a checkpoint after a serializing instruction
         bundle_chkpts++;
         temp_instr_renamed_since_last_checkpoint = 0;
      }
      else if ((mispredictedBranch_flag == true) || (temp_instr_renamed_since_last_checkpoint == REN->get_max_instr_bw_checkpoints()))
      {
         // place a checkpoint after a mispredicted branch
         bundle_chkpts++;
         temp_instr_renamed_since_last_checkpoint = 0;
      }
   }

   //printf("bundle_chkpts=%llu\n",bundle_chkpts);

   // FIX_ME #2
   // Check if the Rename2 Stage must stall due to any of the following conditions:
   // * Not enough free checkpoints.
   // * Not enough free physical registers.
   //
   // If there are not enough resources for the *whole* rename bundle, then stall the Rename2 Stage.
   // Stalling is achieved by returning from this function ('return').
   // If there are enough resources for the *whole* rename bundle, then do not stall the Rename2 Stage.
   // This is achieved by doing nothing and proceeding to the next statements.

   // FIX_ME #2 BEGIN
   if ((REN->stall_reg(bundle_dst) == true) || (REN->stall_checkpoint(bundle_chkpts) == true))
   {
      //std::cout << "No of Free Regs = " << REN->noOfFreeRegistersInFreeList() << '\n';
      //REN->printMappedRegs();
      //REN->printRMTState();
      return;
   }
   // FIX_ME #2 END

   //
   // Sufficient resources are available to rename the rename bundle.
   //
   for (i = 0; i < dispatch_width; i++) {
      if (!RENAME2[i].valid)
         break;			// Not a valid instruction: Reached the end of the rename bundle so exit loop.

      index = RENAME2[i].index;

      load_flag = IS_LOAD(PAY.buf[index].flags);
      store_flag = IS_STORE(PAY.buf[index].flags);
      branch_flag = IS_BRANCH(PAY.buf[index].flags);
      amo_flag = IS_AMO(PAY.buf[index].flags);
      csr_flag = IS_CSR(PAY.buf[index].flags);

      //printf("load_flag = %d, store_flag = %d, branch_flag = %d, amo_flag = %d, csr_flag = %d\n", load_flag, store_flag, branch_flag, amo_flag, csr_flag);

      // P4 - Inserting a checkpoint BEFORE the instruction is renamed
      serializing_flag = false;
      mispredictedBranch_flag = false;
      exception_flag = false;
      exception_checkpoint_placed_before = false;
      if (PAY.buf[index].good_instruction)
      {
         actual = get_pipe()->peek(PAY.buf[index].db_index);
         if (actual->a_exception)
         {
            exception_flag = true;
         }
         else
         {
            if (PAY.buf[index].inst.opcode() == OP_JAL || PAY.buf[index].inst.opcode() == OP_JALR || PAY.buf[index].inst.opcode() == OP_BRANCH)
            {
               if (PAY.buf[index].next_pc != actual->a_next_pc)
               {
                  mispredictedBranch_flag = true;
               }
            }
         }
      }
      serializing_flag = PAY.buf[index].inst.opcode() == OP_AMO || PAY.buf[index].inst.opcode() == OP_SYSTEM;

      //printf("exception_flag=%d, mispredictedBranch_flag=%d, serializing_flag=%d, instr_renamed_since_last_checkpoint=%llu\n", exception_flag, mispredictedBranch_flag, serializing_flag, instr_renamed_since_last_checkpoint);

      if (instr_renamed_since_last_checkpoint > 0)
      {
         if (serializing_flag)
         {
            // place a checkpoint before and after a serializing instruction
            //printf("place a checkpoint before a serializing instruction\n");
            REN->checkpoint();
            // Place get_checkpoint_id after 1st checkpoint
            PAY.buf[index].chkpt_id = REN->get_checkpoint_id(load_flag, store_flag, branch_flag, amo_flag, csr_flag);
            
            //printf("index=%llu\n", index);
            //printf("place a checkpoint before a serializing instruction: chkpt_id=%llu and index=%llu\n",PAY.buf[index].chkpt_id, index);
            instr_renamed_since_last_checkpoint = 0;
         }
         else if (exception_flag)
         {
            // place a checkpoint before an exception instruction
            //printf("place a checkpoint before an exception instruction\n");
            REN->checkpoint();
            // Place get_checkpoint_id after checkpoint function of exception
            PAY.buf[index].chkpt_id = REN->get_checkpoint_id(load_flag, store_flag, branch_flag, amo_flag, csr_flag);
            
            //printf("index=%llu\n", index);
            //printf("place a checkpoint before an exception instruction: chkpt_id=%llu and index=%llu\n",PAY.buf[index].chkpt_id, index);
            instr_renamed_since_last_checkpoint = 0;
            exception_checkpoint_placed_before = true;
         }
      }
      // FIX_ME #3
      // Rename source registers (first) and destination register (second).
      //
      // Tips:
      // 1. At this point of the code, 'index' is the instruction's index into PAY.buf[] (payload).
      // 2. The instruction's payload has all the information you need to rename registers, if they exist. In particular:
      //    * whether or not the instruction has a first source register, and its logical register number
      //    * whether or not the instruction has a second source register, and its logical register number
      //    * whether or not the instruction has a third source register, and its logical register number
      //    * whether or not the instruction has a destination register, and its logical register number
      // 3. When you rename a logical register to a physical register, remember to *update* the instruction's payload with the physical register specifier,
      //    so that the physical register specifier can be used in subsequent pipeline stages.

      // FIX_ME #3 BEGIN
      if (PAY.buf[index].A_valid)
         PAY.buf[index].A_phys_reg = REN->rename_rsrc(PAY.buf[index].A_log_reg);
      if (PAY.buf[index].B_valid)
         PAY.buf[index].B_phys_reg = REN->rename_rsrc(PAY.buf[index].B_log_reg);
      if (PAY.buf[index].D_valid)
         PAY.buf[index].D_phys_reg = REN->rename_rsrc(PAY.buf[index].D_log_reg);
      if (PAY.buf[index].C_valid)
         PAY.buf[index].C_phys_reg = REN->rename_rdst(PAY.buf[index].C_log_reg);
      // FIX_ME #3 END
      //if (PAY.buf[index].A_valid || PAY.buf[index].B_valid || PAY.buf[index].D_valid || PAY.buf[index].C_valid)
         instr_renamed_since_last_checkpoint++;
      //printf("instr_renamed_since_last_checkpoint=%llu\n", instr_renamed_since_last_checkpoint);

      // P4 - Inserting a checkpoint AFTER the instruction is renamed
      if (serializing_flag)
      {
         // place a checkpoint before and after a serializing instruction
         PAY.buf[index].chkpt_id = REN->get_checkpoint_id(load_flag, store_flag, branch_flag, amo_flag, csr_flag);
         
         //printf("index=%llu\n", index);
         //printf("place a checkpoint after a serializing instruction: chkpt_id=%llu and index=%llu\n",PAY.buf[index].chkpt_id, index);
         REN->checkpoint();
         instr_renamed_since_last_checkpoint = 0;
      }
      else if ((mispredictedBranch_flag == true) || (instr_renamed_since_last_checkpoint == REN->get_max_instr_bw_checkpoints()))
      {
         // place a checkpoint after a mispredicted branch or instr_renamed_since_last_checkpoint reached max max_instr_bw_checkpoint
         PAY.buf[index].chkpt_id = REN->get_checkpoint_id(load_flag, store_flag, branch_flag, amo_flag, csr_flag);
         
         //printf("index=%llu\n", index);
         if (mispredictedBranch_flag == true)
         {
            //printf("place a checkpoint after a mispredicted branch: chkpt_id=%llu and index=%llu\n",PAY.buf[index].chkpt_id, index);
         }
         if (instr_renamed_since_last_checkpoint == REN->get_max_instr_bw_checkpoints())
         {
            //printf("place a checkpoint after reaching max_instr_bw_checkpoints: chkpt_id=%llu and index=%llu\n",PAY.buf[index].chkpt_id, index);
         }
         REN->checkpoint();
         instr_renamed_since_last_checkpoint = 0;
      }
      else if (exception_flag == false)
      {
         PAY.buf[index].chkpt_id = REN->get_checkpoint_id(load_flag, store_flag, branch_flag, amo_flag, csr_flag);
         
         //printf("index=%llu\n", index);
         //printf("no checkpoint for a instruc on correct path: chkpt_id=%llu and index=%llu\n",PAY.buf[index].chkpt_id, index);
      }
      else if (exception_flag == true && exception_checkpoint_placed_before == false)
      {
         PAY.buf[index].chkpt_id = REN->get_checkpoint_id(load_flag, store_flag, branch_flag, amo_flag, csr_flag);
         
         //printf("index=%llu\n", index);
         //printf("no checkpoint exception is the first instruc after max_instr_bw_checkpoints: chkpt_id=%llu and index=%llu\n",PAY.buf[index].chkpt_id, index);
      }

      // FIX_ME #4
      // Get the instruction's branch mask.
      //
      // Tips:
      // 1. Every instruction gets a chkpt_id. An instruction needs to know which branches it depends on, for possible squashing.
      // 2. The chkpt_id is not held in the instruction's PAY.buf[] entry. Rather, it explicitly moves with the instruction
      //    from one pipeline stage to the next. Normally the chkpt_id would be wires at this point in the logic but since we
      //    don't have wires place it temporarily in the RENAME2[] pipeline register alongside the instruction, until it advances
      //    to the DISPATCH[] pipeline register. The required left-hand side of the assignment statement is already provided for you below:
      //    RENAME2[i].chkpt_id = ??;

      // FIX_ME #4 BEGIN
      // P4 - replaced branch mask with checkpoint ID
      // Thus chkpt_id actually becomes chkpt_id
      RENAME2[i].chkpt_id = PAY.buf[index].chkpt_id;
      // FIX_ME #4 END

      // FIX_ME #5
      // If this instruction requires a checkpoint (most branches), then create a checkpoint.
      //
      // Tips:
      // 1. At this point of the code, 'index' is the instruction's index into PAY.buf[] (payload).
      // 2. There is a flag in the instruction's payload that *directly* tells you if this instruction needs a checkpoint.
      // 3. If you create a checkpoint, remember to *update* the instruction's payload with its branch ID
      //    so that the branch ID can be used in subsequent pipeline stages.

      // FIX_ME #5 BEGIN
      /*if (PAY.buf[index].checkpoint)
      {
         PAY.buf[index].branch_ID = REN->checkpoint();
         // ---------------------------------- //
         instr_renamed_since_last_checkpoint = 0;
         // ---------------------------------- //
      }*/
      // FIX_ME #5 END
   }

   //
   // Transfer the rename bundle from the Rename Stage to the Dispatch Stage.
   //
   for (i = 0; i < dispatch_width; i++) {
      if (!RENAME2[i].valid)
         break;			// Not a valid instruction: Reached the end of the rename bundle so exit loop.

      assert(!DISPATCH[i].valid);
      RENAME2[i].valid = false;
      DISPATCH[i].valid = true;
      DISPATCH[i].index = RENAME2[i].index;
      DISPATCH[i].chkpt_id = RENAME2[i].chkpt_id;
   }
}
