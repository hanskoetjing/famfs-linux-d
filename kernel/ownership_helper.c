#include <linux/mm.h>
#include <linux/atomic.h>
#include <asm/tlb.h>
#include <linux/pid.h>
#include <linux/sched.h>

int invalidate_vma(struct vm_area_struct *vma);

int invalidate_vma(struct vm_area_struct *vma) {
    struct mm_struct *task_mm = vma->vm_mm;
    struct mmu_gather tlb;
    pr_info("Invalidating VMA 0x%lx - 0x%lx\n", vma->vm_start, vma->vm_end);
    tlb_gather_mmu(&tlb, task_mm);
    change_protection_range_vma(&tlb, vma, vma->vm_start, vma->vm_end, PAGE_NONE, MM_CP_UFFD_WP);
    tlb_finish_mmu(&tlb);
    return 0;
}
EXPORT_SYMBOL(invalidate_vma);