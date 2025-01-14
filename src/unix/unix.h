typedef struct kernel_heaps *kernel_heaps;
typedef struct unix_heaps *unix_heaps;
typedef struct process *process;
typedef struct thread *thread;

process init_unix(kernel_heaps kh, tuple root, filesystem fs);
process create_process(unix_heaps uh, tuple root, filesystem fs);
void process_get_cwd(process p, filesystem *cwd_fs, inode *cwd);
thread create_thread(process p, u64 tid);
process exec_elf(buffer ex, process kernel_process);

void dump_mem_stats(buffer b);

void filesystem_sync(filesystem fs, status_handler sh);
void filesystem_sync_node(filesystem fs, pagecache_node pn, status_handler sh);

timestamp proc_utime(process p);
timestamp proc_stime(process p);

timestamp thread_utime(thread t);
timestamp thread_stime(thread t);
