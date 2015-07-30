/*------------------------------------------------------------------*/
/* 								    */
/* Name        - tramp-s390x.c      			  	    */
/* 								    */
/* Function    - JIT trampoline code for S/390.                     */
/* 								    */
/* Name	       - Neale Ferguson (Neale.Ferguson@SoftwareAG-usa.com) */
/* 								    */
/* Date        - January, 2004					    */
/* 								    */
/* Derivation  - From exceptions-x86 & exceptions-ppc		    */
/* 	         Paolo Molaro (lupus@ximian.com) 		    */
/* 		 Dietmar Maurer (dietmar@ximian.com)		    */
/* 								    */
/* Copyright   - 2001 Ximian, Inc.				    */
/* 								    */
/*------------------------------------------------------------------*/

/*------------------------------------------------------------------*/
/*                 D e f i n e s                                    */
/*------------------------------------------------------------------*/

#define LMFReg	s390_r13

/*
 * Method-specific trampoline code fragment sizes		    
 */
#define SPECIFIC_TRAMPOLINE_SIZE	96

/*========================= End of Defines =========================*/

/*------------------------------------------------------------------*/
/*                 I n c l u d e s                                  */
/*------------------------------------------------------------------*/

#include <config.h>
#include <glib.h>
#include <string.h>

#include <mono/metadata/abi-details.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/gc-internal.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/monitor.h>
#include <mono/metadata/profiler-private.h>
#include <mono/metadata/tabledefs.h>
#include <mono/arch/s390x/s390x-codegen.h>

#include "mini.h"
#include "mini-s390x.h"
#include "support-s390x.h"

/*========================= End of Includes ========================*/

/*------------------------------------------------------------------*/
/*                 T y p e d e f s                                  */
/*------------------------------------------------------------------*/

typedef struct {
	guint8	stk[S390_MINIMAL_STACK_SIZE];	/* Standard s390x stack	*/
	struct MonoLMF  LMF;			/* LMF			*/
} trampStack_t;

/*========================= End of Typedefs ========================*/

/*------------------------------------------------------------------*/
/*                   P r o t o t y p e s                            */
/*------------------------------------------------------------------*/

/*========================= End of Prototypes ======================*/

/*------------------------------------------------------------------*/
/*                 G l o b a l   V a r i a b l e s                  */
/*------------------------------------------------------------------*/


/*====================== End of Global Variables ===================*/

/*------------------------------------------------------------------*/
/*                                                                  */
/* Name		- mono_arch_get_unbox_trampoline                    */
/*                                                                  */
/* Function	- Return a pointer to a trampoline which does the   */
/*		  unboxing before calling the method.		    */
/*                                                                  */
/*                When value type methods are called through the    */
/*		  vtable we need to unbox the 'this' argument.	    */
/*		                               		 	    */
/* Parameters   - method - Methd pointer			    */
/*		  addr   - Pointer to native code for method	    */
/*		                               		 	    */
/*------------------------------------------------------------------*/

gpointer
mono_arch_get_unbox_trampoline (MonoMethod *method, gpointer addr)
{
	guint8 *code, *start;
	int this_pos = s390_r2;
	MonoDomain *domain = mono_domain_get ();

	start = code = mono_domain_code_reserve (domain, 28);

	S390_SET  (code, s390_r1, addr);
	s390_aghi (code, this_pos, sizeof(MonoObject));
	s390_br   (code, s390_r1);

	g_assert ((code - start) <= 28);

	mono_arch_flush_icache (start, code - start);
	mono_profiler_code_buffer_new (start, code - start, MONO_PROFILER_CODE_BUFFER_UNBOX_TRAMPOLINE, method);

	mono_tramp_info_register (mono_tramp_info_create (NULL, start, code - start, NULL, NULL), domain);

	return start;
}

/*========================= End of Function ========================*/

/*------------------------------------------------------------------*/
/*                                                                  */
/* Name		- mono_arch_patch_callsite                          */
/*                                                                  */
/* Function	- Patch a non-virtual callsite so it calls @addr.   */
/*                                                                  */
/*------------------------------------------------------------------*/

void
mono_arch_patch_callsite (guint8 *method_start, guint8 *orig_code, guint8 *addr)
{
	gint32 displace;
	unsigned short opcode;

	opcode = *((unsigned short *) (orig_code - 2));
	if (opcode == 0x0dee) {
		/* This should be a 'iihf/iilf' sequence */
		S390_EMIT_CALL((orig_code - 14), addr);
		mono_arch_flush_icache (orig_code - 14, 12);
	} else {
		/* This is the 'brasl' instruction */
		orig_code    -= 4;
		displace = ((gssize) addr - (gssize) (orig_code - 2)) / 2;
		s390_patch_rel (orig_code, displace);
		mono_arch_flush_icache (orig_code, 4);
	}
}

/*========================= End of Function ========================*/

/*------------------------------------------------------------------*/
/*                                                                  */
/* Name		- mono_arch_patch_plt_entry.                        */
/*                                                                  */
/* Function	- Patch a PLT entry - unused as yet.                */
/*                                                                  */
/*------------------------------------------------------------------*/

void
mono_arch_patch_plt_entry (guint8 *code, gpointer *got, mgreg_t *regs, guint8 *addr)
{
	g_assert_not_reached ();
}

/*========================= End of Function ========================*/

/*------------------------------------------------------------------*/
/*                                                                  */
/* Name		- mono_arch_create_trampoline_code                  */
/*                                                                  */
/* Function	- Create the designated type of trampoline according*/
/*                to the 'tramp_type' parameter.                    */
/*                                                                  */
/*------------------------------------------------------------------*/

guchar*
mono_arch_create_generic_trampoline (MonoTrampolineType tramp_type, MonoTrampInfo **info, gboolean aot)
{
	char *tramp_name;
	guint8 *buf, *tramp, *code;
	int i, offset, has_caller;
	GSList *unwind_ops = NULL;
	MonoJumpInfo *ji = NULL;

	g_assert (!aot);

	/* Now we'll create in 'buf' the S/390 trampoline code. This
	   is the trampoline code common to all methods  */
		
	code = buf = mono_global_codeman_reserve(512);
		
	if ((tramp_type == MONO_TRAMPOLINE_JUMP) ||
	    (tramp_type == MONO_TRAMPOLINE_HANDLER_BLOCK_GUARD)) 
		has_caller = 0;
	else
		has_caller = 1;

	/*-----------------------------------------------------------
	  STEP 0: First create a non-standard function prologue with a
	  stack size big enough to save our registers.
	  -----------------------------------------------------------*/
		
	s390_stmg (buf, s390_r6, s390_r15, STK_BASE, S390_REG_SAVE_OFFSET);
	s390_lgr  (buf, s390_r11, s390_r15);
	s390_aghi (buf, STK_BASE, -sizeof(trampStack_t));
	s390_stg  (buf, s390_r11, 0, STK_BASE, 0);

	/*---------------------------------------------------------------*/
	/* we build the MonoLMF structure on the stack - see mini-s390.h */
	/* Keep in sync with the code in mono_arch_emit_prolog 		 */
	/*---------------------------------------------------------------*/
											
	s390_lgr   (buf, LMFReg, STK_BASE);
	s390_aghi  (buf, LMFReg, G_STRUCT_OFFSET(trampStack_t, LMF));
											
	/*---------------------------------------------------------------*/	
	/* Save general and floating point registers in LMF		 */	
	/*---------------------------------------------------------------*/	
	s390_stmg (buf, s390_r0, s390_r1, LMFReg, G_STRUCT_OFFSET(MonoLMF, gregs[0]));
	s390_stmg (buf, s390_r2, s390_r5, LMFReg, G_STRUCT_OFFSET(MonoLMF, gregs[2]));
	s390_mvc  (buf, 10*sizeof(gulong), LMFReg, G_STRUCT_OFFSET(MonoLMF, gregs[6]),
		   s390_r11, S390_REG_SAVE_OFFSET);

	offset = G_STRUCT_OFFSET(MonoLMF, fregs[0]);
	for (i = s390_f0; i <= s390_f15; ++i) {
		s390_std  (buf, i, 0, LMFReg, offset);
		offset += sizeof(gdouble);
	}

	/*----------------------------------------------------------
	  STEP 1: call 'mono_get_lmf_addr()' to get the address of our
	  LMF. We'll need to restore it after the call to
	  's390_magic_trampoline' and before the call to the native
	  method.
	  ----------------------------------------------------------*/
				
	S390_SET  (buf, s390_r1, mono_get_lmf_addr);
	s390_basr (buf, s390_r14, s390_r1);
											
	/*---------------------------------------------------------------*/	
	/* Set lmf.lmf_addr = jit_tls->lmf				 */	
	/*---------------------------------------------------------------*/	
	s390_stg   (buf, s390_r2, 0, LMFReg, 				
			    G_STRUCT_OFFSET(MonoLMF, lmf_addr));			
											
	/*---------------------------------------------------------------*/	
	/* Get current lmf						 */	
	/*---------------------------------------------------------------*/	
	s390_lg    (buf, s390_r0, 0, s390_r2, 0);				
											
	/*---------------------------------------------------------------*/	
	/* Set our lmf as the current lmf				 */	
	/*---------------------------------------------------------------*/	
	s390_stg   (buf, LMFReg, 0, s390_r2, 0);				
											
	/*---------------------------------------------------------------*/	
	/* Have our lmf.previous_lmf point to the last lmf		 */	
	/*---------------------------------------------------------------*/	
	s390_stg   (buf, s390_r0, 0, LMFReg, 				
			    G_STRUCT_OFFSET(MonoLMF, previous_lmf));			
											
	/*---------------------------------------------------------------*/	
	/* save method info						 */	
	/*---------------------------------------------------------------*/	
	s390_lg    (buf, s390_r1, 0, LMFReg, G_STRUCT_OFFSET(MonoLMF, gregs[1]));
	s390_stg   (buf, s390_r1, 0, LMFReg, G_STRUCT_OFFSET(MonoLMF, method));				
									
	/*---------------------------------------------------------------*/	
	/* save the current SP						 */	
	/*---------------------------------------------------------------*/	
	s390_lg    (buf, s390_r1, 0, STK_BASE, 0);
	s390_stg   (buf, s390_r1, 0, LMFReg, G_STRUCT_OFFSET(MonoLMF, ebp));	
									
	/*---------------------------------------------------------------*/	
	/* save the current IP						 */	
	/*---------------------------------------------------------------*/	
	if (has_caller) {
		s390_lg    (buf, s390_r1, 0, s390_r1, S390_RET_ADDR_OFFSET);
	} else {
		s390_lghi  (buf, s390_r1, 0);
	}
	s390_stg   (buf, s390_r1, 0, LMFReg, G_STRUCT_OFFSET(MonoLMF, eip));	
											
	/*---------------------------------------------------------------*/
	/* STEP 2: call the C trampoline function                        */
	/*---------------------------------------------------------------*/
				
	/* Set arguments */

	/* Arg 1: mgreg_t *regs */
	s390_la  (buf, s390_r2, 0, LMFReg, G_STRUCT_OFFSET(MonoLMF, gregs[0]));
		
	/* Arg 2: code (next address to the instruction that called us) */
	if (has_caller) {
		s390_lg   (buf, s390_r3, 0, s390_r11, S390_RET_ADDR_OFFSET);
	} else {
		s390_lghi (buf, s390_r3, 0);
	}

	/* Arg 3: Trampoline argument */
	s390_lg (buf, s390_r4, 0, LMFReg, G_STRUCT_OFFSET(MonoLMF, gregs[1]));

	/* Arg 4: trampoline address. */
	S390_SET (buf, s390_r5, buf);
		
	/* Calculate call address and call the C trampoline. Return value will be in r2 */
	tramp = (guint8*)mono_get_trampoline_func (tramp_type);
	S390_SET  (buf, s390_r1, tramp);
	s390_basr (buf, s390_r14, s390_r1);
		
	/* OK, code address is now on r2. Move it to r1, so that we
	   can restore r2 and use it from r1 later */
	s390_lgr  (buf, s390_r1, s390_r2);

	/*----------------------------------------------------------
	  STEP 3: Restore the LMF
	  ----------------------------------------------------------*/
	restoreLMF(buf, STK_BASE, sizeof(trampStack_t));
	
	/*----------------------------------------------------------
	  STEP 4: call the compiled method
	  ----------------------------------------------------------*/
		
	/* Restore parameter registers */
	s390_lmg (buf, s390_r2, s390_r5, LMFReg, G_STRUCT_OFFSET(MonoLMF, gregs[2]));
		
	/* Restore the FP registers */
	offset = G_STRUCT_OFFSET(MonoLMF, fregs[0]);
	for (i = s390_f0; i <= s390_f15; ++i) {
		s390_ld  (buf, i, 0, LMFReg, offset);
		offset += sizeof(gdouble);
	}

	/* Restore stack pointer and jump to the code -
	 * R14 contains the return address to our caller 
	 */
	s390_lgr  (buf, STK_BASE, s390_r11);
	s390_lmg  (buf, s390_r6, s390_r14, STK_BASE, S390_REG_SAVE_OFFSET);

	if (MONO_TRAMPOLINE_TYPE_MUST_RETURN(tramp_type)) {
		s390_lgr (buf, s390_r2, s390_r1);
		s390_br  (buf, s390_r14);
	} else {
		s390_br  (buf, s390_r1);
	}

	/* Flush instruction cache, since we've generated code */
	mono_arch_flush_icache (code, buf - code);
	mono_profiler_code_buffer_new (buf, code - buf, MONO_PROFILER_CODE_BUFFER_GENERICS_TRAMPOLINE, NULL);
	
	g_assert (info);
	tramp_name = mono_get_generic_trampoline_name (tramp_type);
	*info = mono_tramp_info_create (tramp_name, buf, buf - code, ji, unwind_ops);
	g_free (tramp_name);

	/* Sanity check */
	g_assert ((buf - code) <= 512);

	return code;
}

/*========================= End of Function ========================*/

/*------------------------------------------------------------------*/
/*                                                                  */
/* Name		- mono_arch_invalidate_method                       */
/*                                                                  */
/* Function	- 						    */
/*                                                                  */
/*------------------------------------------------------------------*/

void
mono_arch_invalidate_method (MonoJitInfo *ji, void *func, gpointer func_arg)
{
	/* FIXME: This is not thread safe */
	guint8 *code = ji->code_start;

	S390_SET  (code, s390_r1, func);
	S390_SET  (code, s390_r2, func_arg);
	s390_br   (code, s390_r1);

}

/*========================= End of Function ========================*/

/*------------------------------------------------------------------*/
/*                                                                  */
/* Name		- mono_arch_create_specific_trampoline              */
/*                                                                  */
/* Function	- Creates the given kind of specific trampoline     */
/*                                                                  */
/*------------------------------------------------------------------*/

gpointer
mono_arch_create_specific_trampoline (gpointer arg1, MonoTrampolineType tramp_type, MonoDomain *domain, guint32 *code_len)
{
	guint8 *code, *buf, *tramp;
	gint32 displace;

	tramp = mono_get_trampoline_code (tramp_type);

	/*----------------------------------------------------------*/
	/* This is the method-specific part of the trampoline. Its  */
	/* purpose is to provide the generic part with the          */
	/* MonoMethod *method pointer. We'll use r1 to keep it.     */
	/*----------------------------------------------------------*/
	code = buf = mono_domain_code_reserve (domain, SPECIFIC_TRAMPOLINE_SIZE);

	switch (tramp_type) {
	/*
	 * Monitor tramps have the object in r2
	 */
	case MONO_TRAMPOLINE_MONITOR_ENTER:
	case MONO_TRAMPOLINE_MONITOR_ENTER_V4:
	case MONO_TRAMPOLINE_MONITOR_EXIT:
		s390_lgr (buf, s390_r1, s390_r2);
		break;
	default :
		S390_SET  (buf, s390_r1, arg1);
	}
	displace = (tramp - buf) / 2;
	s390_jg   (buf, displace);

	/* Flush instruction cache, since we've generated code */
	mono_arch_flush_icache (code, buf - code);
	mono_profiler_code_buffer_new (buf, code - buf, MONO_PROFILER_CODE_BUFFER_SPECIFIC_TRAMPOLINE, 
				       (void *) mono_get_generic_trampoline_simple_name (tramp_type));

	/* Sanity check */
	g_assert ((buf - code) <= SPECIFIC_TRAMPOLINE_SIZE);

	if (code_len)
		*code_len = buf - code;
	
	return code;
}	

/*========================= End of Function ========================*/

/*------------------------------------------------------------------*/
/*                                                                  */
/* Name		- mono_arch_create_rgctx_lazy_fetch_trampoline      */
/*                                                                  */
/* Function	- 						    */
/*                                                                  */
/*------------------------------------------------------------------*/

gpointer
mono_arch_create_rgctx_lazy_fetch_trampoline (guint32 slot, MonoTrampInfo **info, gboolean aot)
{
	guint8 *tramp;
	guint8 *code, *buf;
	guint8 **rgctx_null_jumps;
	gint32 displace;
	int tramp_size,
	    depth, 
	    index, 
	    iPatch = 0,
	    i;
	gboolean mrgctx;
	MonoJumpInfo *ji = NULL;
	GSList *unwind_ops = NULL;

	mrgctx = MONO_RGCTX_SLOT_IS_MRGCTX (slot);
	index = MONO_RGCTX_SLOT_INDEX (slot);
	if (mrgctx)
		index += MONO_SIZEOF_METHOD_RUNTIME_GENERIC_CONTEXT / sizeof (gpointer);
	for (depth = 0; ; ++depth) {
		int size = mono_class_rgctx_get_array_size (depth, mrgctx);

		if (index < size - 1)
			break;
		index -= size - 1;
	}

	tramp_size = 48 + 16 * depth;
	if (mrgctx)
		tramp_size += 4;
	else
		tramp_size += 12;

	code = buf = mono_global_codeman_reserve (tramp_size);

	unwind_ops = mono_arch_get_cie_program ();

	rgctx_null_jumps = g_malloc (sizeof (guint8*) * (depth + 2));

	if (mrgctx) {
		/* get mrgctx ptr */
		s390_lgr (code, s390_r1, s390_r2);
	} else {
		/* load rgctx ptr from vtable */
		s390_lg (code, s390_r1, 0, s390_r2, MONO_STRUCT_OFFSET(MonoVTable, runtime_generic_context));
		/* is the rgctx ptr null? */
		s390_ltgr (code, s390_r1, s390_r1);
		/* if yes, jump to actual trampoline */
		rgctx_null_jumps [iPatch++] = code;
		s390_jge (code, 0);
	}

	for (i = 0; i < depth; ++i) {
		/* load ptr to next array */
		if (mrgctx && i == 0)
			s390_lg (code, s390_r1, 0, s390_r1, MONO_SIZEOF_METHOD_RUNTIME_GENERIC_CONTEXT);
		else
			s390_lg (code, s390_r1, 0, s390_r1, 0);
		s390_ltgr (code, s390_r1, s390_r1);
		/* if the ptr is null then jump to actual trampoline */
		rgctx_null_jumps [iPatch++] = code;
		s390_jge (code, 0);
	}

	/* fetch slot */
	s390_lg (code, s390_r1, 0, s390_r1, (sizeof (gpointer) * (index  + 1)));
	/* is the slot null? */
	s390_ltgr (code, s390_r1, s390_r1);
	/* if yes, jump to actual trampoline */
	rgctx_null_jumps [iPatch++] = code;
	s390_jge (code, 0);
	/* otherwise return r1 */
	s390_lgr (code, s390_r2, s390_r1);
	s390_br  (code, s390_r14);

	for (i = 0; i < iPatch; i++) {
		displace = ((uintptr_t) code - (uintptr_t) rgctx_null_jumps[i]) / 2;
		s390_patch_rel ((rgctx_null_jumps [i] + 2), displace);
	}

	g_free (rgctx_null_jumps);

	/* move the rgctx pointer to the VTABLE register */
	s390_lgr (code, MONO_ARCH_VTABLE_REG, s390_r2);

	tramp = mono_arch_create_specific_trampoline (GUINT_TO_POINTER (slot),
		MONO_TRAMPOLINE_RGCTX_LAZY_FETCH, mono_get_root_domain (), NULL);

	/* jump to the actual trampoline */
	displace = (tramp - code) / 2;
	s390_jg (code, displace);

	mono_arch_flush_icache (buf, code - buf);
	mono_profiler_code_buffer_new (buf, code - buf, MONO_PROFILER_CODE_BUFFER_GENERICS_TRAMPOLINE, NULL);

	g_assert (code - buf <= tramp_size);

	char *name = mono_get_rgctx_fetch_trampoline_name (slot);
	*info = mono_tramp_info_create (name, buf, code - buf, ji, unwind_ops);
	g_free (name);

	return(buf);
}	

/*========================= End of Function ========================*/

/*------------------------------------------------------------------*/
/*                                                                  */
/* Name    	- mono_arch_get_static_rgctx_trampoline		    */
/*                                                                  */
/* Function	- Create a trampoline which sets RGCTX_REG to MRGCTX*/
/*		  then jumps to ADDR.				    */
/*                                                                  */
/*------------------------------------------------------------------*/

gpointer
mono_arch_get_static_rgctx_trampoline (MonoMethod *m, 
					MonoMethodRuntimeGenericContext *mrgctx, 
					gpointer addr)
{
	guint8 *code, *start;
	gint32 displace;
	int buf_len;

	MonoDomain *domain = mono_domain_get ();

	buf_len = 32;

	start = code = mono_domain_code_reserve (domain, buf_len);

	S390_SET  (code, MONO_ARCH_RGCTX_REG, mrgctx);
	displace = ((uintptr_t) addr - (uintptr_t) code) / 2;
	s390_jg   (code, displace);
	g_assert ((code - start) < buf_len);

	mono_arch_flush_icache (start, code - start);
	mono_profiler_code_buffer_new (start, code - start, MONO_PROFILER_CODE_BUFFER_HELPER, NULL);

	mono_tramp_info_register (mono_tramp_info_create (NULL, start, code - start, NULL, NULL), domain);

	return(start);
}	

/*========================= End of Function ========================*/

/*------------------------------------------------------------------*/
/*                                                                  */
/* Name		- handler_block_trampoline_helper                   */
/*                                                                  */
/* Function	- 						    */
/*                                                                  */
/*------------------------------------------------------------------*/

static void
handler_block_trampoline_helper (gpointer *ptr)
{
	MonoJitTlsData *jit_tls = mono_native_tls_get_value (mono_jit_tls_id);
	*ptr = jit_tls->handler_block_return_address;
}

/*========================= End of Function ========================*/

/*------------------------------------------------------------------*/
/*                                                                  */
/* Name		- mono_arch_create_handler_block_trampoline         */
/*                                                                  */
/* Function	- 						    */
/*                                                                  */
/*------------------------------------------------------------------*/

gpointer
mono_arch_create_handler_block_trampoline (MonoTrampInfo **info, gboolean aot)
{
	guint8 *tramp = mono_get_trampoline_code (MONO_TRAMPOLINE_HANDLER_BLOCK_GUARD);
	guint8 *code, *buf;
	int tramp_size = 64;
	MonoJumpInfo *ji = NULL;
	GSList *unwind_ops = NULL;

	g_assert (!aot);

	code = buf = mono_global_codeman_reserve (tramp_size);

	/*
	 * This trampoline restore the call chain of the handler block 
	 * then jumps into the code that deals with it.
	 */

	if (mono_get_jit_tls_offset () != -1) {
		s390_ear  (code, s390_r1, 0);
		s390_sllg (code, s390_r1, s390_r1, 0, 32);
		s390_ear  (code, s390_r1, 1);
		S390_SET  (code, s390_r14, mono_get_jit_tls_offset());
		s390_lg   (code, s390_r14, s390_r1, 0, G_STRUCT_OFFSET(MonoJitTlsData, handler_block_return_address));
		/* 
		 * Simulate a call 
		 */
		S390_SET  (code, s390_r1, tramp);
		s390_br   (code, s390_r1);
	} else {
		/*
		 * Slow path uses a C helper
		 */
		S390_SET  (code, s390_r2, tramp);
		S390_SET  (code, s390_r1, handler_block_trampoline_helper);
		s390_br	  (code, s390_r1);
	}

	mono_arch_flush_icache (buf, code - buf);
	mono_profiler_code_buffer_new (buf, code - buf, MONO_PROFILER_CODE_BUFFER_HELPER, NULL);
	g_assert (code - buf <= tramp_size);

	*info = mono_tramp_info_create ("handler_block_trampoline", buf, code - buf, ji, unwind_ops);

	return buf;
}

/*========================= End of Function ========================*/

#ifdef MONO_ARCH_MONITOR_OBJECT_REG
/*------------------------------------------------------------------*/
/*                                                                  */
/* Name		- mono_arch_create_monitor_enter_trampoline         */
/*                                                                  */
/* Function	- 						    */
/*                                                                  */
/*------------------------------------------------------------------*/

gpointer
mono_arch_create_monitor_enter_trampoline (MonoTrampInfo **info, gboolean is_v4, gboolean aot)
{
	guint8	*tramp,
		*code, *buf;
	gint16	*jump_obj_null, 
		*jump_sync_null, 
		*jump_cs_failed, 
		*jump_other_owner, 
		*jump_tid, 
		*jump_sync_thin_hash = NULL,
		*jump_lock_taken_true = NULL;
	int tramp_size,
	    status_reg = s390_r0,
	    lock_taken_reg = s390_r1,
	    obj_reg = s390_r2,
	    sync_reg = s390_r3,
	    tid_reg = s390_r4,
	    status_offset,
	    nest_offset;
	MonoJumpInfo *ji = NULL;
	GSList *unwind_ops = NULL;

	g_assert (MONO_ARCH_MONITOR_OBJECT_REG == obj_reg);
#ifdef MONO_ARCH_MONITOR_LOCK_TAKEN_REG
	g_assert (MONO_ARCH_MONITOR_LOCK_TAKEN_REG == lock_taken_reg);
#else
	g_assert (!is_v4);
#endif

	mono_monitor_threads_sync_members_offset (&status_offset, &nest_offset);
	g_assert (MONO_THREADS_SYNC_MEMBER_SIZE (status_offset) == sizeof (guint32));
	g_assert (MONO_THREADS_SYNC_MEMBER_SIZE (nest_offset) == sizeof (guint32));
	status_offset = MONO_THREADS_SYNC_MEMBER_OFFSET (status_offset);
	nest_offset = MONO_THREADS_SYNC_MEMBER_OFFSET (nest_offset);

	tramp_size = 160;

	code = buf = mono_global_codeman_reserve (tramp_size);

	unwind_ops = mono_arch_get_cie_program ();

	if (mono_thread_get_tls_offset () != -1) {
		/* MonoObject* obj is in obj_reg */
		/* is obj null? */
		s390_ltgr (code, obj_reg, obj_reg);
		/* if yes, jump to actual trampoline */
		s390_jz (code, 0); CODEPTR(code, jump_obj_null);

		if (is_v4) {
			s390_cli (code, lock_taken_reg, 0, 1);
			/* if *lock_taken is 1, jump to actual trampoline */
			s390_je (code, 0); CODEPTR(code, jump_lock_taken_true);
		}

		/* load obj->synchronization to sync_reg */
		s390_lg (code, sync_reg, 0, obj_reg, MONO_STRUCT_OFFSET (MonoObject, synchronisation));

		if (mono_gc_is_moving ()) {
			/*if bit zero is set it's a thin hash*/
			s390_tmll (code, sync_reg, 1);
			s390_jo  (code, 0); CODEPTR(code, jump_sync_thin_hash);

			/* Clear bits used by the gc */
			s390_nill (code, sync_reg, ~0x3);
		}

		/* is synchronization null? */
		s390_ltgr (code, sync_reg, sync_reg);
		/* if yes, jump to actual trampoline */
		s390_jz (code, 0); CODEPTR(code, jump_sync_null);

		/* load MonoInternalThread* into tid_reg */
		s390_ear (code, s390_r5, 0);
		s390_sllg(code, s390_r5, s390_r5, 0, 32);
		s390_ear (code, s390_r5, 1);
		/* load tid */
		s390_lg  (code, tid_reg, 0, s390_r5, mono_thread_get_tls_offset ());
		s390_lgf (code, tid_reg, 0, tid_reg, MONO_STRUCT_OFFSET (MonoInternalThread, small_id));

		/* is synchronization->owner free */
		s390_lgf  (code, status_reg, 0, sync_reg, status_offset);
		s390_nilf (code, status_reg, OWNER_MASK);
		/* if not, jump to next case */
		s390_jnz  (code, 0); CODEPTR(code, jump_tid);

		/* if yes, try a compare-exchange with the TID */
		/* Form new status in tid_reg */
		s390_xr (code, tid_reg, status_reg);
		/* compare and exchange */
		s390_cs (code, status_reg, tid_reg, sync_reg, status_offset);
		s390_jnz (code, 0); CODEPTR(code, jump_cs_failed);
		/* if successful, return */
		if (is_v4)
			s390_mvi (code, lock_taken_reg, 0, 1);
		s390_br (code, s390_r14);

		/* next case: synchronization->owner is not null */
		PTRSLOT(code, jump_tid);
		/* is synchronization->owner == TID? */
		s390_nilf (code, status_reg, OWNER_MASK);
		s390_cr (code, status_reg, tid_reg);
		/* if not, jump to actual trampoline */
		s390_jnz (code, 0); CODEPTR(code, jump_other_owner);
		/* if yes, increment nest */
		s390_lgf (code, s390_r5, 0, sync_reg, nest_offset);
		s390_ahi (code, s390_r5, 1);
		s390_st  (code, s390_r5, 0, sync_reg, nest_offset);
		/* return */
		if (is_v4)
			s390_mvi (code, lock_taken_reg, 0, 1);
		s390_br (code, s390_r14);

		PTRSLOT (code, jump_obj_null);
		if (jump_sync_thin_hash)
			PTRSLOT (code, jump_sync_thin_hash);
		PTRSLOT (code, jump_sync_null);
		PTRSLOT (code, jump_cs_failed);
		PTRSLOT (code, jump_other_owner);
		if (is_v4)
			PTRSLOT (code, jump_lock_taken_true);
	}

	/* jump to the actual trampoline */
	if (is_v4)
		tramp = mono_arch_create_specific_trampoline (NULL, MONO_TRAMPOLINE_MONITOR_ENTER_V4, mono_get_root_domain (), NULL);
	else
		tramp = mono_arch_create_specific_trampoline (NULL, MONO_TRAMPOLINE_MONITOR_ENTER, mono_get_root_domain (), NULL);

	/* jump to the actual trampoline */
	S390_SET (code, s390_r1, tramp);
	s390_br (code, s390_r1);

	mono_arch_flush_icache (code, code - buf);
	mono_profiler_code_buffer_new (buf, code - buf, MONO_PROFILER_CODE_BUFFER_MONITOR, NULL);
	g_assert (code - buf <= tramp_size);

	if (info) {
		if (is_v4)
			*info = mono_tramp_info_create ("monitor_enter_v4_trampoline", buf, code - buf, ji, unwind_ops);
		else
			*info = mono_tramp_info_create ("monitor_enter_trampoline", buf, code - buf, ji, unwind_ops);
	}

	return buf;
}

/*========================= End of Function ========================*/

/*------------------------------------------------------------------*/
/*                                                                  */
/* Name		- mono_arch_create_monitor_exit_trampoline          */
/*                                                                  */
/* Function	- 						    */
/*                                                                  */
/*------------------------------------------------------------------*/

gpointer
mono_arch_create_monitor_exit_trampoline (MonoTrampInfo **info, gboolean aot)
{
	guint8	*tramp,
		*code, *buf;
	gint16	*jump_obj_null, 
		*jump_have_waiters, 
		*jump_sync_null, 
		*jump_not_owned, 
		*jump_cs_failed,
		*jump_next,
		*jump_sync_thin_hash = NULL;
	int	tramp_size,
		status_offset, nest_offset;
	MonoJumpInfo *ji = NULL;
	GSList *unwind_ops = NULL;
	int	obj_reg = s390_r2,
		sync_reg = s390_r3,
		status_reg = s390_r4;

	g_assert (obj_reg == MONO_ARCH_MONITOR_OBJECT_REG);

	mono_monitor_threads_sync_members_offset (&status_offset, &nest_offset);
	g_assert (MONO_THREADS_SYNC_MEMBER_SIZE (status_offset) == sizeof (guint32));
	g_assert (MONO_THREADS_SYNC_MEMBER_SIZE (nest_offset) == sizeof (guint32));
	status_offset = MONO_THREADS_SYNC_MEMBER_OFFSET (status_offset);
	nest_offset = MONO_THREADS_SYNC_MEMBER_OFFSET (nest_offset);

	tramp_size = 160;

	code = buf = mono_global_codeman_reserve (tramp_size);

	unwind_ops = mono_arch_get_cie_program ();

	if (mono_thread_get_tls_offset () != -1) {
		/* MonoObject* obj is in obj_reg */
		/* is obj null? */
		s390_ltgr (code, obj_reg, obj_reg);
		/* if yes, jump to actual trampoline */
		s390_jz (code, 0); CODEPTR(code, jump_obj_null);

		/* load obj->synchronization to RCX */
		s390_lg (code, sync_reg, 0, obj_reg, MONO_STRUCT_OFFSET (MonoObject, synchronisation));

		if (mono_gc_is_moving ()) {
			/*if bit zero is set it's a thin hash*/
			s390_tmll (code, sync_reg, 1);
			s390_jo   (code, 0); CODEPTR(code, jump_sync_thin_hash);

			/* Clear bits used by the gc */
			s390_nill (code, sync_reg, ~0x3);
		}

		/* is synchronization null? */
		s390_ltgr (code, sync_reg, sync_reg);
		/* if yes, jump to actual trampoline */
		s390_jz (code, 0); CODEPTR(code, jump_sync_null);

		/* next case: synchronization is not null */
		/* load MonoInternalThread* into r5 */
		s390_ear (code, s390_r5, 0);
		s390_sllg(code, s390_r5, s390_r5, 0, 32);
		s390_ear (code, s390_r5, 1);
		/* load TID into r1 */
		s390_lg  (code, s390_r1, 0, s390_r5, mono_thread_get_tls_offset ());
		s390_lgf (code, s390_r1, 0, s390_r1, MONO_STRUCT_OFFSET (MonoInternalThread, small_id));
		/* is synchronization->owner == TID */
		s390_lgf (code, status_reg, 0, sync_reg, status_offset);
		s390_xr  (code, s390_r1, status_reg);
		s390_tmlh (code, s390_r1, OWNER_MASK);
		/* if not, jump to actual trampoline */
		s390_jno (code, 0); CODEPTR(code, jump_not_owned);

		/* next case: synchronization->owner == TID */
		/* is synchronization->nest == 1 */
		s390_lgf (code, s390_r0, 0, sync_reg, nest_offset);
		s390_chi (code, s390_r0, 1);
		/* if not, jump to next case */
		s390_jne (code, 0); CODEPTR(code, jump_next);
		/* if yes, is synchronization->entry_count greater than zero */
		s390_cfi (code, status_reg, ENTRY_COUNT_WAITERS);
		/* if not, jump to actual trampoline */
		s390_jnz (code, 0); CODEPTR(code, jump_have_waiters);
		/* if yes, try to set synchronization->owner to null and return */
		/* old status in s390_r0 */
		s390_lgfr (code, s390_r0, status_reg);
		/* form new status */
		s390_nilf (code, status_reg, ENTRY_COUNT_MASK);
		/* compare and exchange */
		s390_cs (code, s390_r0, status_reg, sync_reg, status_offset);
		/* if not successful, jump to actual trampoline */
		s390_jnz (code, 0); CODEPTR(code, jump_cs_failed);
		s390_br  (code, s390_r14);

		/* next case: synchronization->nest is not 1 */
		PTRSLOT (code, jump_next);
		/* decrease synchronization->nest and return */
		s390_lgf (code, s390_r0, 0, sync_reg, nest_offset);
		s390_ahi (code, s390_r0, -1);
		s390_st  (code, s390_r0, 0, sync_reg, nest_offset);
		s390_br  (code, s390_r14);

		PTRSLOT (code, jump_obj_null);
		if (jump_sync_thin_hash)
			PTRSLOT (code, jump_sync_thin_hash);
		PTRSLOT (code, jump_have_waiters);
		PTRSLOT (code, jump_not_owned);
		PTRSLOT (code, jump_cs_failed);
		PTRSLOT (code, jump_sync_null);
	}

	/* jump to the actual trampoline */
	tramp = mono_arch_create_specific_trampoline (NULL, MONO_TRAMPOLINE_MONITOR_EXIT, mono_get_root_domain (), NULL);

	S390_SET (code, s390_r1, tramp);
	s390_br (code, s390_r1);

	mono_arch_flush_icache (code, code - buf);
	mono_profiler_code_buffer_new (buf, code - buf, MONO_PROFILER_CODE_BUFFER_MONITOR, NULL);
	g_assert (code - buf <= tramp_size);

	if (info)
		*info = mono_tramp_info_create ("monitor_exit_trampoline", buf, code - buf, ji, unwind_ops);

	return buf;
}

/*========================= End of Function ========================*/
#endif
