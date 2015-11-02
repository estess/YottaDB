/****************************************************************
 *								*
 *	Copyright 2001, 2008 Fidelity Information Services, Inc	*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#ifdef UNIX
#include "aswp.h"
#elif defined(VMS)
#include <descrip.h>
#endif

#include "gdsroot.h"
#include "gdsbt.h"
#include "gdskill.h"
#include "gtm_facility.h"
#include "fileinfo.h"
#include "gdsfhead.h"
#include "filestruct.h"
#include "cdb_sc.h"
#include "copy.h"
#include "error.h"
#include "gdscc.h"
#include "interlock.h"
#include "jnl.h"
#include "buddy_list.h"		/* needed for tp.h */
#include "hashtab_int4.h"	/* needed for tp.h */
#include "tp.h"
#include "secshr_db_clnup.h"
#include "t_commit_cleanup.h"
#include "process_deferred_stale.h"
#include "repl_msg.h"		/* for gtmsource.h */
#include "gtmsource.h"		/* for jnlpool_addrs structure definition */
#include "send_msg.h"

GBLREF unsigned char		cw_set_depth;
GBLREF cw_set_element		cw_set[];
GBLREF sgmnt_addrs		*cs_addrs;
GBLREF sgmnt_data_ptr_t		cs_data;
GBLREF gd_region		*gv_cur_region;
GBLREF unsigned int		t_tries;
GBLREF short			dollar_tlevel;
GBLREF sgm_info			*first_sgm_info;
GBLREF cache_rec_ptr_t		cr_array[((MAX_BT_DEPTH * 2) - 1) * 2]; /* Maximum number of blocks that can be in transaction */
GBLREF unsigned int		cr_array_index;
GBLREF boolean_t		unhandled_stale_timer_pop;
GBLREF jnlpool_addrs		jnlpool;
GBLREF uint4			process_id;
GBLREF jnlpool_ctl_ptr_t	jnlpool_ctl, temp_jnlpool_ctl;
GBLREF gv_namehead		*gv_target;
GBLREF int4			update_trans;
GBLREF	sgm_info		*first_tp_si_by_ftok; /* List of participating regions in the TP transaction sorted on ftok order */

#define CACHE_REC_CLEANUP(csa, cr)		\
{						\
	assert(!cr->in_tend);			\
	assert(!cr->data_invalid);		\
	assert(csa->now_crit);			\
	assert(cr->in_cw_set);			\
	cr->in_cw_set = FALSE;			\
}

#define	RESET_EARLY_TN_IF_NEEDED(csa)						\
{										\
	assert(!csa->t_commit_crit);						\
	csa->t_commit_crit = FALSE;						\
	if (csa->now_crit && (csa->ti->curr_tn == csa->ti->early_tn - 1))	\
		csa->ti->early_tn = csa->ti->curr_tn;           		\
	assert(!csa->now_crit || csa->ti->curr_tn == csa->ti->early_tn);	\
}

#define	RELEASE_JNLPOOL_LOCK_IF_NEEDED(jpl_reg)	\
{						\
	if (NULL != jpl_reg)			\
		rel_lock(jpl_reg);		\
}

#define	RESET_REG_SEQNO_IF_NEEDED(csa, jpl_csa)					\
{										\
	if (reg_seqno_reset)							\
	{									\
		assert(csa->hdr->reg_seqno <= (jnlpool_ctl->jnl_seqno + 1));	\
		assert(csa->now_crit && jpl_csa->now_crit);			\
		csa->hdr->reg_seqno = jnlpool_ctl->jnl_seqno;			\
	}									\
}

boolean_t t_commit_cleanup(enum cdb_sc status, int signal)
{
	boolean_t	update_underway, reg_seqno_reset = FALSE;
	cache_rec_ptr_t	cr;
	sgm_info	*si;
	sgmnt_addrs	*csa, *jpl_csa = NULL;
	char		*trstr;
	gd_region	*xactn_err_region, *jpl_reg = NULL;

	error_def(ERR_DBCOMMITCLNUP);

	assert(cdb_sc_normal != status);
	xactn_err_region = gv_cur_region;

	/* see comments in secshr_db_clnup for the commit logic flow as a sequence of steps in t_end and tp_tend and how
	 * t_commit_cleanup() and secshr_db_clnup() complement each other (one does the rollback and one the roll forward)
	 * update_underway is set to TRUE to indicate the commit is beyond rollback. It is set only if we hold crit on the region.
	 */
	update_underway = FALSE;
	if (dollar_tlevel > 0)
	{
		trstr = "TP";
		/* Regions are committed in the ftok order using "first_tp_si_by_ftok". Also crit is released on each region
		 * as the commit completes. Take that into account while determining if update is underway. Note that this
		 * update_underway determining logic is shared by secshr_db_clnup as well so any changes here need to be made there.
		 */
		for (si = first_tp_si_by_ftok;  (NULL != si);  si = si->next_tp_si_by_ftok)
		{
			if (T_COMMIT_STARTED == si->update_trans)
			{	/* Two possibilities.
				 *	(a) case of duplicate set not creating any cw-sets but updating db curr_tn++.
				 *	(b) Have completed commit for this region and have released crit on this region.
				 *		(in a potentially multi-region TP transaction).
				 * In either case, update is underway and the transaction cannot be rolled back.
				 */
				update_underway = TRUE;
				break;
			}
			if (NULL != si->first_cw_set)
			{
				csa = si->tp_csa;
				/* Assert that if we are in the midst of commit in a region, we better hold crit */
				assert(!csa->t_commit_crit || csa->now_crit);
				/* Just to be safe, set update_underway to TRUE only if we have crit on this region. */
				if (csa->now_crit && csa->t_commit_crit)
				{
					update_underway = TRUE;
					break;
				}
			}
		}
	} else
	{
		trstr = "NON-TP";
		update_underway = (cs_addrs->now_crit && (cs_addrs->t_commit_crit || (T_COMMIT_STARTED == update_trans)));
		if (NULL != gv_target)	/* gv_target can be NULL in case of DSE MAPS command etc. */
			gv_target->clue.end = 0; /* in case t_end() had set history's tn to be "valid_thru++", undo it */
	}
	if (!update_underway)
	{	/* Rollback (undo) the transaction. the comments below refer to step numbers as documented in secshr_db_clnup */
		if ((NULL != jnlpool.jnlpool_dummy_reg) && jnlpool.jnlpool_dummy_reg->open)
		{
			csa = &FILE_INFO(jnlpool.jnlpool_dummy_reg)->s_addrs;
			if (csa->now_crit)
			{	/* reset any csa->hdr->early_tn like increments that might have occurred in jnlpool */
				assert((sm_uc_ptr_t)csa->critical == ((sm_uc_ptr_t)jnlpool_ctl + JNLPOOL_CTL_SIZE));
				if (jnlpool_ctl->early_write_addr != jnlpool_ctl->write_addr)
				{
					reg_seqno_reset = TRUE;	/* reset reg_seqnos of all regions to jnlpool_ctl->jnl_seqno */
					DEBUG_ONLY(jpl_csa = csa;)
					jnlpool_ctl->early_write_addr = jnlpool_ctl->write_addr; /* step (3) gets undone here */
				}
				assert(jnlpool_ctl->write == jnlpool_ctl->write_addr % jnlpool_ctl->jnlpool_size);
				if (CDB_STAGNATE > t_tries)
					jpl_reg = jnlpool.jnlpool_dummy_reg;	/* note down to release crit later */
			}
		}
		if (dollar_tlevel > 0)
		{	/* At this point we know a TP update is NOT underway. In this case, use "first_sgm_info" and not
			 * "first_tp_si_by_ftok" as the latter might be NULL even though we have gotten crit on all the
			 * regions and are in the final retry. In this case using "first_sgm_info" will guarantee that
			 * we release crit on all the regions.
			 */
			for (si = first_sgm_info; NULL != si; si = si->next_sgm_info)
			{
				TP_CHANGE_REG(si->gv_cur_region);
				csa = cs_addrs;
				while (si->cr_array_index > 0)
				{
					cr = si->cr_array[--si->cr_array_index];
					CACHE_REC_CLEANUP(csa, cr);
				}
				if (si->update_trans)
				{
					RESET_EARLY_TN_IF_NEEDED(csa);		/* step (4) of the commit logic is undone here */
					RESET_REG_SEQNO_IF_NEEDED(csa, jpl_csa);/* step (5) of the commit logic is undone here */
				}
				assert(!csa->t_commit_crit);
				assert(!csa->now_crit || csa->ti->curr_tn == csa->ti->early_tn);
				ASSERT_JNL_SEQNO_FILEHDR_JNLPOOL(csa->hdr, jnlpool_ctl); /* debug-only sanity check between
											  * seqno of filehdr and jnlpool */
				/* Do not release crit on the region until reg_seqno has been reset above */
				rel_crit(gv_cur_region); /* step (1) of the commit logic is iteratively undone here */
			}
			/* Do not release crit on jnlpool until reg_seqno has been reset above */
			RELEASE_JNLPOOL_LOCK_IF_NEEDED(jpl_reg);/* step (2) of the commit logic is undone here */
		} else
		{
			csa = cs_addrs;
			while (cr_array_index > 0)
			{
				cr = cr_array[--cr_array_index];
				CACHE_REC_CLEANUP(csa, cr);
			}
			if (update_trans)
			{
				RESET_EARLY_TN_IF_NEEDED(csa);		/* step (4) of the commit logic is undone here */
				RESET_REG_SEQNO_IF_NEEDED(csa, jpl_csa);/* step (5) of the commit logic is undone here */
			}
			/* Do not release crit on jnlpool or the region until reg_seqno has been reset above */
			RELEASE_JNLPOOL_LOCK_IF_NEEDED(jpl_reg);/* step (2) of the commit logic is undone here */
			rel_crit(gv_cur_region);	/* step (1) of the commit logic is undone here */
		}
		if ((t_tries < CDB_STAGNATE) && unhandled_stale_timer_pop)
			process_deferred_stale();
	} else
	{	/* Roll forward (complete the partial commit of) the transaction by invoking secshr_db_clnup() */
		send_msg(VARLSTCNT(7) ERR_DBCOMMITCLNUP, 5, process_id, process_id, trstr, DB_LEN_STR(xactn_err_region));
		/* if t_ch() (a condition handler) was driving this routine, then doing send_msg() here is not a good idea
		 * as it will overlay the current error message string driving t_ch(), but this case is an exception since
		 * we currently do not know of any way by which we will be in this "update_underway == TRUE" code if t_ch()
		 * calls us (there is an assert in t_ch to that effect in terms of testing the return value of this routine)
		 */
		secshr_db_clnup(COMMIT_INCOMPLETE);
		if (unhandled_stale_timer_pop)
			process_deferred_stale();
	}
	return update_underway;
}
