#include <linux/module.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ioport.h>
#include <linux/rcupdate.h>
#include <linux/sprintf.h>
#include <linux/completion.h>
#include <vdso/limits.h>
#include <asm-generic/cacheflush.h>
#include <linux/pid.h>
#include <linux/pid_types.h>
#include <linux/kthread.h>
#include <linux/pfn_t.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>

#include <linux/cxlshm_msg.h>
#include "../../drivers/dax/dax-private.h"
#include "../../kernel/cxlshm_msg/conn_manager.h"
#include "cxlshm_handler_private.h"

#define THIS_MOD "cxlshm_invalidator_internal: "

static struct task_struct *invalidator_thread;
DECLARE_COMPLETION(ownership_transfer_arrival_var);

int invalidate_mem_area(void *data);
struct task_struct *get_task_from_int_pid(pid_t pid);
int flush_mem_task(pid_t pid);
int flush_mem_task_page(pid_t pid, unsigned long pfn_to_flush);

/*
 * This is the invalidator thread. it will run and sleep until a completion event from the connection manager.
 * The thread will read ownership info on first 2 MiB of memory, and take that ownership info to flush page or vma.
 */
int invalidate_mem_area(void *data) 
{
    int ret = 0;
    
    while(!kthread_should_stop()) 
    {
        long completion_ret_val = wait_for_completion_interruptible(&ownership_transfer_arrival_var);
        if (completion_ret_val >= 0) 
        {
            char *received_copy = kzalloc(MAX_BUFFER_NET * sizeof(char), GFP_NOWAIT); //using nowait as this is IO
            memset(received_copy, 0, MAX_BUFFER_NET * sizeof(char));
            spin_lock(&ctr_lock);
            strscpy(received_copy, ownership_transfer_message, MAX_BUFFER_NET - 1);
            memset(ownership_transfer_message, 0, sizeof(ownership_transfer_message));
            spin_unlock(&ctr_lock);
            int message_type = get_message_type(received_copy);
            pr_info(THIS_MOD "message: %s message_type %d\n", received_copy, message_type);
            struct ownership *owner;
            get_owner_info_on_mem(&owner);
            pr_info(THIS_MOD "owner: %d\n", owner->owner_pid);
            if(message_type == WHOLE_VMA)
            {
                flush_mem_task(owner->owner_pid);
            }
            else if (message_type == PAGE)
            {
                unsigned long pfn_to_flush = get_pfn_by_offset(owner->offset);
                flush_mem_task_page(owner->owner_pid, pfn_to_flush);
            }
            ret = _send_response("DONE");
            reinit_completion(&ownership_transfer_arrival_var);
            kfree(received_copy);
        } 
        else 
        {
            pr_info(THIS_MOD "interrupted\n");
            return -EINTR;
        }
    }
    pr_info(THIS_MOD "thread returns\n");
    return ret;
}

/*
 * Find task_struct from pid_t 
 */
struct task_struct *get_task_from_int_pid(pid_t pid) 
{
	struct pid *the_pid = find_get_pid(pid);
	if (the_pid != NULL)
        return get_pid_task(the_pid, PIDTYPE_PID);
    else 
        return NULL;
}

extern int invalidate_vma(struct vm_area_struct *vma);

/*
 * Flush a specific VMA of a PID
 */
int flush_mem_task(pid_t pid) 
{
	int ret = 0;
	struct vm_area_struct *this_vma = NULL;
    struct task_struct *the_task = NULL;
    struct ownership *owner_on_mem;
	if (pid != -1) 
    {
		the_task = get_task_from_int_pid(pid);
		if (the_task != NULL) 
        {
            struct mm_struct *mm = the_task->mm;
            if (mm == NULL) 
                return -1;
            struct vm_area_struct *vma;
            MA_STATE(mas, &mm->mm_mt, 0, 0);
            get_owner_info_on_mem(&owner_on_mem);
            pid_t pid_on_mem = owner_on_mem->owner_pid;
            pr_info(THIS_MOD "pid %d vm_start: 0x%lx\n", pid_on_mem, owner_on_mem->vm_start);
            mas_for_each(&mas, vma, ULONG_MAX) 
            {
                if (vma->vm_flags & VM_CXLSHM && vma->vm_start == owner_on_mem->vm_start && vma->vm_end == owner_on_mem->vm_end) 
                {
                    this_vma = vma;
                    invalidate_vma(vma);
                    zap_vma_ptes(this_vma, this_vma->vm_start, this_vma->vm_end - this_vma->vm_start); //temporar
                    flush_cache_range(this_vma, this_vma->vm_start, this_vma->vm_end);
                    struct mm_struct *mm = this_vma->vm_mm;
                    flush_tlb_mm(mm);
                    break;
                }
            }
            if (this_vma) 
            {
                pr_info(THIS_MOD "found vma addr: 0x%lx\n", this_vma->vm_start);
                struct mm_struct *mm = this_vma->vm_mm;
                struct mmu_gather tlb;
                mmap_write_lock(mm);
                tlb_gather_mmu(&tlb, mm);
                change_vma_protection_range(&tlb, this_vma, this_vma->vm_start, this_vma->vm_end, PAGE_NONE, MM_CP_UFFD_WP);
                tlb_finish_mmu(&tlb);
                zap_vma_ptes(this_vma, this_vma->vm_start, this_vma->vm_end - this_vma->vm_start);
                flush_tlb_mm(mm);
                flush_cache_range(this_vma, this_vma->vm_start, this_vma->vm_end);
                mmap_write_unlock(mm);
                pr_info(THIS_MOD "Flush CPU cache. Size: %ld\n", this_vma->vm_end - this_vma->vm_start);
            } 
            else 
            {
                pr_info(THIS_MOD "VMA not found\n");
                ret = -1;
            }
        } 
        else 
        {
            pr_info(THIS_MOD "task not found\n");
            ret = -1;
        }
	} 
    else 
    {
		ret = -1;
	}
	return ret;
}

/*
 * Flush specific page (in PTE) of a process specified by pid and pfn_to_flush
 */
int flush_mem_task_page(pid_t pid, unsigned long pfn_to_flush) 
{
	int ret = 0;
	struct vm_area_struct *this_vma = NULL;
    struct task_struct *the_task = NULL;
    struct ownership *owner_on_mem;
	if (pid != -1) 
    {
		the_task = get_task_from_int_pid(pid);
		if (the_task != NULL) 
        {
            get_owner_info_on_mem(&owner_on_mem);
            struct mm_struct *mm = the_task->mm;
            if (mm == NULL) 
                return -1;
            struct vm_area_struct *vma;
            MA_STATE(mas, &mm->mm_mt, 0, 0);
            
            pid_t pid_on_mem = owner_on_mem->owner_pid;
            pr_info(THIS_MOD "pid %d vm_start: 0x%lx\n", pid_on_mem, owner_on_mem->vm_start);
            mas_for_each(&mas, vma, ULONG_MAX) {
                if (vma->vm_flags & VM_CXLDSM) 
                {
                    this_vma = vma;
                    pr_info(THIS_MOD "found vma addr: 0x%lx\n", vma->vm_start);
                    break;
                }
            }
            if (this_vma) 
            {
                pr_info(THIS_MOD "Flush CPU cache. Size: %ld\n", this_vma->vm_end - this_vma->vm_start);
                
                pte_t *ptep;
                unsigned long addr = 0;
                struct mm_struct *this_mm = this_vma->vm_mm;
                spinlock_t *sp;
                int found = 0;
                down_read(&(this_mm->mmap_lock));
                for (addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE)
                {
                    pgd_t *pgd = pgd_offset(this_mm, addr);
                    if (pgd_none(*pgd) || pgd_bad(*pgd))
                        continue;
                    p4d_t *p4d = p4d_offset(pgd, addr);
                    if (p4d_none(*p4d) || p4d_bad(*p4d))
                        continue;
                    pud_t *pud = pud_offset(p4d, addr);
                    if (pud_none(*pud) || pud_bad(*pud))
                        continue;
                    pmd_t *pmd = pmd_offset(pud, addr);
                    if (pmd_none(*pmd) || pmd_bad(*pmd))
                        continue;
                    pte_t temporary_pte;
                    ptep = pte_offset_map_lock(this_mm, pmd, addr, &sp);
                    temporary_pte = *ptep;
                    if (pte_pfn(*ptep) == pfn_to_flush)
                    {
                        ptep_clear_flush(vma, addr, ptep);
                        found = 1;
                        spin_unlock(sp);
                        rcu_read_unlock();
                        break;
                    }
                    else
                    {
                        pte_unmap_unlock(ptep, sp);
                    }
                    pr_info(THIS_MOD "pte 0x%lx pte to flush: 0x%lx\n", pte_pfn(temporary_pte), pfn_to_flush);
                }
                up_read(&(this_mm->mmap_lock));
                if (found == 1)
                {
                    pr_info(THIS_MOD "address with pfn found\n");
                }
            } 
            else 
            {
                pr_info(THIS_MOD "VMA not found\n");
                ret = -1;
            }
        } 
        else 
        {
            pr_info(THIS_MOD "task not found\n");
            ret = -1;
        }
	} 
    else 
    {
		ret = -1;
	}
	return ret;
}

static int __init cxlshm_invalidator_init(void) 
{	
    void *data = NULL;
    set_ownership_completion(&ownership_transfer_arrival_var);
    invalidator_thread = kthread_run(invalidate_mem_area, (void *)data, "invalidate_mem_area");

	//init done
	pr_info(THIS_MOD "loaded\n");
	return 0;
}

static void __exit cxlshm_invalidator_exit(void) 
{
    set_ownership_completion(NULL);
    if (task_is_running(invalidator_thread)) {
        pr_info(THIS_MOD "stop invalidator thread\n"); 
        kthread_stop(invalidator_thread);
    }

	//exit done
	pr_info(THIS_MOD ": unloaded\n"); 
}


module_init(cxlshm_invalidator_init);
module_exit(cxlshm_invalidator_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CXL shared memory area invalidator");
